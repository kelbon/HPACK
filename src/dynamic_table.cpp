
#include "hpack/dynamic_table.hpp"

#include <utility>
#include <cstring>  // memcpy

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
  swap(entries, other.entries);
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
  // Evict without freeing: name/value may point into entries being evicted.
  // entry_t::create() copies them first, then we free the evicted entries.
  auto evicted = evict_until_fits_into(_max_size - new_entry_size);
  noexport::scope_exit e{
      [&] { evicted.clear_and_dispose([this](entry_t* e) { entry_t::destroy(e, _resource); }); }};
  entry_t* entry = entry_t::create(name, value, ++_insert_count, _resource);
  entries.push_back(entry);
  set.insert(*entries.back());
  _current_size += new_entry_size;
  return entry;
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
  auto evicted = evict_until_fits_into(new_max_size);
  _max_size = new_max_size;
  evicted.clear_and_dispose([this](entry_t* e) { entry_t::destroy(e, _resource); });
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
  for (entry_t* e : entries)
    entry_t::destroy(e, _resource);
  entries.clear();
  _current_size = 0;
}

dynamic_table_t::entry_set_t dynamic_table_t::evict_until_fits_into(size_type bytes) noexcept {
  entry_set_t evicted;
  size_type i = 0;
  for (; _current_size > bytes; ++i) {
    _current_size -= entry_size(*entries[i]);
    set.erase(set.iterator_to(*entries[i]));
    evicted.insert(*entries[i]);
  }
  // evicts should be rare operation
  entries.erase(entries.begin(), entries.begin() + i);
  return evicted;
}

table_entry dynamic_table_t::get_entry(index_type index) const noexcept {
  assert(index != 0 && index <= current_max_index());
  if (index < static_table_t::first_unused_index)
    return static_table_t::get_entry(index);
  auto& e = *(&entries.back() - (index - static_table_t::first_unused_index));
  return table_entry{e->name(), e->value()};
}

}  // namespace hpack
