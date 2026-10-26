
#include "hpack/dynamic_table.hpp"

#include <utility>

namespace hpack {

std::string_view dynamic_table_t::key_of_entry::operator()(const dynamic_table_t::entry_t& v) const noexcept {
  return v.name();
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
  swap(set, other.set);
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
  entry_t* entry = entry_t::create(name, value, _insert_count + 1, _resource);
  evict(evicted);
  ++_insert_count;
  ++_count;
  entry_at(_insert_count) = entry;
  set.insert(*entry);
  _current_size += new_entry_size;
  return entry;
}

void dynamic_table_t::grow_ring() {
  std::vector<entry_t*> new_ring(ring.empty() ? 8 : ring.size() * 2);
  for (size_t c = oldest(); c <= _insert_count; ++c)
    new_ring[c & (new_ring.size() - 1)] = entry_at(c);
  ring.swap(new_ring);
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
    set.erase(set.iterator_to(*entry));
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
  auto [b, e] = set.equal_range(name);
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

find_result_t dynamic_table_t::find(index_type name, std::string_view value) const noexcept {
  assert(name <= current_max_index());
  if (name == 0) [[unlikely]]
    return {};
  return find(get_entry(name).name, value);
}

void dynamic_table_t::reset() noexcept {
  set.clear();
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
