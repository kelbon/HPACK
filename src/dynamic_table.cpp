
#include "hpack/dynamic_table.hpp"

#include <utility>
#include <cstring>  // memcpy

namespace hpack {

struct dynamic_table_t::entry_t : entry_set_hook {
  const size_type name_end;
  const size_type value_end;
  const size_t _insert_c;
  char data[];

  entry_t(size_type name_len, size_type value_len, size_t insert_c) noexcept
      : name_end(name_len), value_end(name_len + value_len), _insert_c(insert_c) {
  }

  std::string_view name() const noexcept {
    return {data, data + name_end};
  }
  std::string_view value() const noexcept {
    return {data + name_end, data + value_end};
  }
  size_type size() const noexcept {
    return value_end;
  }

  static entry_t* create(std::string_view name, std::string_view value, size_t insert_c,
                         std::pmr::memory_resource* resource) {
    assert(resource);
    void* bytes = resource->allocate(sizeof(entry_t) + name.size() + value.size(), alignof(entry_t));
    entry_t* e = new (bytes) entry_t(name.size(), value.size(), insert_c);
    if (!name.empty())
      memcpy(+e->data, name.data(), name.size());
    if (!value.empty())
      memcpy(e->data + name.size(), value.data(), value.size());
    return e;
  }
  static void destroy(const entry_t* e, std::pmr::memory_resource* resource) noexcept {
    assert(e && resource);
    std::destroy_at(e);
    resource->deallocate((void*)e, sizeof(entry_t) + e->value_end, alignof(entry_t));
  }
};

std::string_view dynamic_table_t::key_of_entry::operator()(const dynamic_table_t::entry_t& v) const noexcept {
  return v.name();
}

static size_t hash_calc(std::string_view bytes) noexcept {
  // standard hash is bad sometimes
  constexpr uint64_t fnv_offset_basis = 14695981039346656037ULL;
  constexpr uint64_t fnv_prime = 1099511628211ULL;
  size_t hash = fnv_offset_basis;
  for (auto byte : bytes) {
    hash ^= static_cast<uint64_t>(byte);
    hash *= fnv_prime;
  }
  return hash;
}

size_t dynamic_table_t::hash_by_namevalue::operator()(
    dynamic_table_t::key_of_entry::type str) const noexcept {
  return hash_calc(str);
}

// precondition: 'e' now in entries
index_type dynamic_table_t::indexof(const dynamic_table_t::entry_t& e) const noexcept {
  return static_table_t::first_unused_index + (_insert_count - e._insert_c);
}

static size_type entry_size(const dynamic_table_t::entry_t& entry) noexcept {
  /*
      The size of an entry is the sum of its name's length in octets (as
      defined in Section 5.2), its value's length in octets, and 32.

      entry.name().size() + entry.value().size() + 32;
 */
  return entry.value_end + 32;
}

dynamic_table_t::dynamic_table_t(size_type max_size, std::pmr::memory_resource* m) noexcept
    : buckets(initial_buckets_count),
      set({buckets.data(), buckets.size()}),
      _current_size(0),
      _max_size(max_size),
      _user_protocol_max_size(max_size),
      _insert_count(0),
      _resource(m ? m : std::pmr::get_default_resource()) {
}

dynamic_table_t::dynamic_table_t(dynamic_table_t&& other) noexcept
    : entries(std::move(other.entries)),
      set(std::move(other.set)),
      _current_size(std::exchange(other._current_size, 0)),
      _max_size(std::exchange(other._max_size, 0)),
      _insert_count(std::exchange(other._insert_count, 0)),
      _resource(std::exchange(other._resource, std::pmr::get_default_resource())) {
}

dynamic_table_t& dynamic_table_t::operator=(dynamic_table_t&& other) noexcept {
  if (this == &other) [[unlikely]]
    return *this;
  reset();
  entries = std::move(other.entries);
  set = std::move(other.set);
  _current_size = std::exchange(other._current_size, 0);
  _max_size = std::exchange(other._max_size, 0);
  _insert_count = std::exchange(other._insert_count, 0);
  _resource = std::exchange(other._resource, std::pmr::get_default_resource());
  return *this;
}

dynamic_table_t::~dynamic_table_t() {
  reset();
}

// returns index of added pair, 0 if cannot add
index_type dynamic_table_t::add_entry(std::string_view name, std::string_view value) {
  size_type new_entry_size = name.size() + value.size() + 32;
  if (_max_size < new_entry_size) [[unlikely]] {
    reset();
    return 0;
  }
  evict_until_fits_into(_max_size - new_entry_size);
  entries.push_back(entry_t::create(name, value, ++_insert_count, _resource));
  set.insert(*entries.back());
  if (entries.size() > buckets.size() / 2) {
    // https://github.com/boostorg/intrusive/issues/96
    // workaround:
    // unordered set bucket copy(move) ctor does nothing, so .resize will be UB
    decltype(buckets) new_buckets(buckets.size() * 2);
    set.rehash({new_buckets.data(), new_buckets.size()});
    buckets = std::move(new_buckets);
  }
  _current_size += new_entry_size;
  return static_table_t::first_unused_index;
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
  evict_until_fits_into(new_max_size);
  _max_size = new_max_size;
}

find_result_t dynamic_table_t::find(std::string_view name, std::string_view value) noexcept {
  find_result_t r;
  auto i = set.bucket(name);
  auto b = set.begin(i);
  auto e = set.end(i);
  for (; b != e; ++b) {
    if (b->name() == name) {
      r.header_name_index = indexof(*b);
      if (b->value() == value) {
        r.value_indexed = true;
        return r;
      }
    }
  }
  return r;
}

find_result_t dynamic_table_t::find(index_type name, std::string_view value) noexcept {
  assert(name <= current_max_index());
  if (name == 0) [[unlikely]]
    return {};
  return find(get_entry(name).name, value);
}

void dynamic_table_t::reset() noexcept {
  set.clear();
  for (entry_t* e : entries)
    entry_t::destroy(e, _resource);
  entries.clear();
  _current_size = 0;
}

void dynamic_table_t::evict_until_fits_into(size_type bytes) noexcept {
  size_type i = 0;
  for (; _current_size > bytes; ++i) {
    _current_size -= entry_size(*entries[i]);
    set.erase(set.iterator_to(*entries[i]));
    entry_t::destroy(entries[i], _resource);
  }
  // evicts should be rare operation
  entries.erase(entries.begin(), entries.begin() + i);
}

table_entry dynamic_table_t::get_entry(index_type index) const noexcept {
  assert(index != 0 && index <= current_max_index());
  if (index < static_table_t::first_unused_index)
    return static_table_t::get_entry(index);
  auto& e = *(&entries.back() - (index - static_table_t::first_unused_index));
  return table_entry{e->name(), e->value()};
}

}  // namespace hpack
