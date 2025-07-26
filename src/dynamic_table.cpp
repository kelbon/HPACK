
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
