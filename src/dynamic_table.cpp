
#include "hpack/dynamic_table.hpp"

#include <cstring>
#include <utility>

namespace hpack {

index_type dynamic_table_t::indexof(const dynamic_table_t::entry_t& e) const noexcept {
  return static_table_t::first_unused_index + (_insert_count - e._insert_c);
}

namespace {

using entry_t = dynamic_table_t::entry_t;

// MurmurHash64A (Austin Appleby, public domain), тот же, что std::hash в libstdc++ на 64-битных платформах
// Note: не защищён от hash flooding, но длина списков в корзинах всё равно ограничена числом записей
uint64_t hash_bytes(std::string_view s, uint64_t seed) noexcept {
  constexpr uint64_t m = 0xc6a4a7935bd1e995;
  constexpr int r = 47;
  uint64_t h = seed ^ (s.size() * m);
  const char* p = s.data();
  size_t n = s.size();
  for (; n >= 8; p += 8, n -= 8) {
    uint64_t k;
    memcpy(&k, p, 8);
    k *= m;
    k ^= k >> r;
    k *= m;
    h ^= k;
    h *= m;
  }
  if (n != 0) {
    uint64_t tail = 0;
    for (size_t i = 0; i < n; ++i)
      tail |= uint64_t(uint8_t(p[i])) << (i * 8);
    h ^= tail;
    h *= m;
  }
  h ^= h >> r;
  h *= m;
  h ^= h >> r;
  return h;
}

uint64_t hash_name(std::string_view name) noexcept {
  return hash_bytes(name, 0);
}

uint64_t hash_pair(uint64_t name_hash, std::string_view value) noexcept {
  return hash_bytes(value, name_hash);
}

bool has_name(const entry_t& e, uint64_t name_hash, std::string_view name) noexcept {
  return e.name_hash == name_hash && e.name() == name;
}

bool has_pair(const entry_t& e, uint64_t pair_hash, std::string_view name, std::string_view value) noexcept {
  return e.pair_hash == pair_hash && e.name() == name && e.value() == value;
}

// precondition: buckets.size() — степень двойки
template <typename Bucket>
Bucket& bucket_for(std::vector<Bucket>& buckets, uint64_t hash) noexcept {
  return buckets[hash & (buckets.size() - 1)];
}
template <typename Bucket>
const Bucket& bucket_for(const std::vector<Bucket>& buckets, uint64_t hash) noexcept {
  return buckets[hash & (buckets.size() - 1)];
}

// первая запись в 'bucket', для которой 'pred' вернул true, или nullptr
template <typename Bucket, typename Pred>
const entry_t* find_in(const Bucket& bucket, Pred pred) noexcept {
  for (const entry_t& e : bucket)
    if (pred(e))
      return &e;
  return nullptr;
}

}  // namespace

static size_type entry_size(const dynamic_table_t::entry_t& entry) noexcept {
  /*
      The size of an entry is the sum of its name's length in octets (as
      defined in Section 5.2), its value's length in octets, and 32.

      entry.name().size() + entry.value().size() + 32;
 */
  return entry.value_end + 32;
}

dynamic_table_t::dynamic_table_t(size_type max_size, std::pmr::memory_resource* m) noexcept
    : _current_size(0),
      _max_size(max_size),
      _user_protocol_max_size(max_size),
      _insert_count(0),
      _resource(m ? m : std::pmr::get_default_resource()) {
}

void dynamic_table_t::swap(dynamic_table_t& other) noexcept {
  using std::swap;
  swap(ring, other.ring);
  swap(_count, other._count);
  swap(by_name, other.by_name);
  swap(by_pair, other.by_pair);
  swap(_current_size, other._current_size);
  swap(_max_size, other._max_size);
  swap(_user_protocol_max_size, other._user_protocol_max_size);
  swap(_insert_count, other._insert_count);
  swap(_resource, other._resource);
}

dynamic_table_t::dynamic_table_t(dynamic_table_t&& other) noexcept {
  swap(other);
}

dynamic_table_t& dynamic_table_t::operator=(dynamic_table_t&& other) noexcept {
  swap(other);
  return *this;
}

dynamic_table_t::~dynamic_table_t() {
  reset();
}

dynamic_table_t::entry_t* dynamic_table_t::add_entry(std::string_view name, std::string_view value) {
  size_type new_entry_size = name.size() + value.size() + 32;
  if (!can_fit_entry(new_entry_size)) [[unlikely]] {
    reset();
    return nullptr;
  }
  // name/value may point into evicted entries, so they are freed after create()
  evict_count_t evicted = need_evict_to_fit(_max_size - new_entry_size);
  // if throws, table is not changed
  if (_count - evicted.count == ring.size())
    grow_ring();
  uint64_t name_hash = hash_name(name);
  entry_t* entry =
      entry_t::create(name, value, _insert_count + 1, name_hash, hash_pair(name_hash, value), _resource);
  evict(evicted);
  ++_insert_count;
  ++_count;
  entry_at(_insert_count) = entry;
  index_add(*entry);
  _current_size += new_entry_size;
  return entry;
}

void dynamic_table_t::grow_ring() {
  size_t new_size = ring.empty() ? 8 : ring.size() * 2;
  std::vector<entry_t*> new_ring(new_size);
  std::vector<name_bucket_t> new_by_name(new_size);
  std::vector<pair_bucket_t> new_by_pair(new_size);
  for (size_t c = oldest(); c <= _insert_count; ++c)
    new_ring[c & (new_ring.size() - 1)] = entry_at(c);
  ring.swap(new_ring);
  by_name.swap(new_by_name);
  by_pair.swap(new_by_pair);
  // индексы строятся заново от старых записей к новым, так что более новые заменяют более старые
  for (size_t c = oldest(); c <= _insert_count; ++c)
    index_add(*entry_at(c));
}

void dynamic_table_t::index_add(entry_t& e) noexcept {
  name_bucket_t& nb = bucket_for(by_name, e.name_hash);
  nb.remove_if([&](const entry_t& x) { return has_name(x, e.name_hash, e.name()); });
  nb.push_front(e);
  pair_bucket_t& pb = bucket_for(by_pair, e.pair_hash);
  pb.remove_if([&](const entry_t& x) { return has_pair(x, e.pair_hash, e.name(), e.value()); });
  pb.push_front(e);
}

void dynamic_table_t::index_remove(entry_t& e) noexcept {
  bucket_for(by_name, e.name_hash).remove_if([&](const entry_t& x) { return &x == &e; });
  bucket_for(by_pair, e.pair_hash).remove_if([&](const entry_t& x) { return &x == &e; });
}

dynamic_table_t::evict_count_t dynamic_table_t::need_evict_to_fit(size_type size_limit) const noexcept {
  evict_count_t r;
  while (_current_size - r.bytes > size_limit) {
    r.bytes += entry_size(*entry_at(oldest() + r.count));
    ++r.count;
  }
  return r;
}

void dynamic_table_t::evict(evict_count_t e) noexcept {
  assert(e.count <= _count);
  for (size_t c = oldest(), end = c + e.count; c != end; ++c) {
    entry_t*& entry = entry_at(c);
    index_remove(*entry);
    entry_t::destroy(entry, _resource);
    entry = nullptr;
  }
  _count -= e.count;
  _current_size -= e.bytes;
}

void dynamic_table_t::set_user_protocol_max_size(size_type new_max_size) noexcept {
  _user_protocol_max_size = new_max_size;
  if (_user_protocol_max_size < max_size())
    update_size(new_max_size);
}

void dynamic_table_t::update_size(size_type new_max_size) {
  //  "The new maximum size MUST be lower than or equal to the limit
  // determined by the protocol using HPACK. A value that exceeds this
  // limit MUST be treated as a decoding error"
  if (new_max_size > _user_protocol_max_size)
    throw HPACK_PROTOCOL_ERROR(dynamic table max size exceeds limit determined by protocol);
  evict(need_evict_to_fit(new_max_size));
  _max_size = new_max_size;
}

find_result_t dynamic_table_t::find(std::string_view name, std::string_view value) const noexcept {
  find_result_t r;
  if (_count == 0)
    return r;
  uint64_t name_hash = hash_name(name);
  const entry_t* e =
      find_in(bucket_for(by_name, name_hash), [&](const entry_t& x) { return has_name(x, name_hash, name); });
  if (!e)
    return r;
  r.header_name_index = indexof(*e);
  uint64_t pair_hash = hash_pair(name_hash, value);
  e = find_in(bucket_for(by_pair, pair_hash),
              [&](const entry_t& x) { return has_pair(x, pair_hash, name, value); });
  if (e) {
    r.header_name_index = indexof(*e);
    r.value_indexed = true;
  }
  return r;
}

index_type dynamic_table_t::find_name(std::string_view name) const noexcept {
  if (_count == 0)
    return 0;
  uint64_t name_hash = hash_name(name);
  const entry_t* e =
      find_in(bucket_for(by_name, name_hash), [&](const entry_t& x) { return has_name(x, name_hash, name); });
  return e ? indexof(*e) : 0;
}

find_result_t dynamic_table_t::find(index_type name, std::string_view value) const noexcept {
  assert(name <= current_max_index());
  if (name == 0) [[unlikely]]
    return {};
  return find(get_entry(name).name, value);
}

void dynamic_table_t::reset() noexcept {
  for (name_bucket_t& b : by_name)
    b.clear();
  for (pair_bucket_t& b : by_pair)
    b.clear();
  for (size_t c = oldest(); c <= _insert_count; ++c) {
    entry_t::destroy(entry_at(c), _resource);
    entry_at(c) = nullptr;
  }
  _count = 0;
  _current_size = 0;
}

table_entry dynamic_table_t::get_entry(index_type index) const noexcept {
  assert(index != 0 && index <= current_max_index());
  if (index < static_table_t::first_unused_index)
    return static_table_t::get_entry(index);
  const entry_t* e = entry_at(_insert_count - (index - static_table_t::first_unused_index));
  return table_entry{e->name(), e->value()};
}

}  // namespace hpack
