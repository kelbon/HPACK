#pragma once

#include <memory_resource>

#include <boost/intrusive/unordered_set.hpp>

#include "hpack/basic_types.hpp"
#include "hpack/static_table.hpp"

namespace hpack {

namespace bi = boost::intrusive;

struct dynamic_table_t {
  struct entry_t;

 private:
  struct key_of_entry {
    using type = std::string_view;
    std::string_view operator()(const entry_t& v) const noexcept;
  };
  struct equal_by_namevalue {
    bool operator()(const key_of_entry::type& l, const key_of_entry::type& r) const noexcept {
      return l == r;
    }
  };
  struct hash_by_namevalue {
    size_t operator()(key_of_entry::type) const noexcept;
  };

  // invariant: do not contain nullptrs
  std::vector<entry_t*> entries;
  using entry_set_hook = bi::unordered_set_base_hook<bi::link_mode<bi::normal_link>, bi::store_hash<true>,
                                                     bi::optimize_multikey<true>>;
  // hashed by name
  using entry_set_t = bi::unordered_multiset<entry_t, bi::base_hook<entry_set_hook>,
                                             bi::key_of_value<key_of_entry>, bi::equal<equal_by_namevalue>,
                                             bi::hash<hash_by_namevalue>, bi::power_2_buckets<true>>;

  static constexpr inline size_t initial_buckets_count = 4;

  // Note: must be before `set` because of destroy ordering
  // invariant: .size is always pow of 2
  std::vector<entry_set_t::bucket_type> buckets;
  entry_set_t set;
  // in bytes
  // invariant: <= _max_size
  size_type _current_size = 0;
  size_type _max_size = 0;
  // https://datatracker.ietf.org/doc/html/rfc7541#section-6.3
  //  "The new maximum size MUST be lower than or equal to the limit
  // determined by the protocol using HPACK"
  //
  // This value is hard limit for updating '_max_size'
  size_type _user_protocol_max_size = 0;
  size_t _insert_count = 0;
  // invariant: != nullptr
  std::pmr::memory_resource* _resource = std::pmr::get_default_resource();
  /*
         <----------  Index Address Space ---------->
         <-- Static  Table -->  <-- Dynamic Table -->
         +---+-----------+---+  +---+-----------+---+
         | 1 |    ...    | s |  |s+1|    ...    |s+k|
         +---+-----------+---+  +---+-----------+---+
                                ^                   |
                                |                   V
                         Insertion Point      Dropping Point
  */
 public:
  // 4096 - default size by protocol
  dynamic_table_t() : dynamic_table_t(4096) {
  }
  // `user_protocol_max_size` and `max_size()` both initialized to `max_size`
  explicit dynamic_table_t(size_type max_size,
                           std::pmr::memory_resource* m = std::pmr::get_default_resource()) noexcept;

  // this constructor may be used to set `user_protocol_max_size` to value other than `max_size`
  // precondition: `protocol_max_size` <= `max_size`
  explicit dynamic_table_t(size_type max_size, size_type protocol_max_size,
                           std::pmr::memory_resource* m = std::pmr::get_default_resource()) noexcept
      : dynamic_table_t(max_size, m) {
    assert(protocol_max_size >= max_size);
    _user_protocol_max_size = protocol_max_size;
  }

  dynamic_table_t(const dynamic_table_t&) = delete;

  dynamic_table_t(dynamic_table_t&& other) noexcept;

  void operator=(const dynamic_table_t&) = delete;

  dynamic_table_t& operator=(dynamic_table_t&& other) noexcept;

  ~dynamic_table_t();

  // returns index of added pair, 0 if cannot add
  index_type add_entry(std::string_view name, std::string_view value);

  size_type current_size() const noexcept {
    return _current_size;
  }
  // Note: its current max size, not max size defined by protocol which using hpack
  size_type max_size() const noexcept {
    return _max_size;
  }

  void set_user_protocol_max_size(size_type) noexcept;
  size_type user_protocol_max_size() const noexcept {
    return _user_protocol_max_size;
  }

  // throws if `new_max_size` > `user_protocol_max_size`
  void update_size(size_type new_max_size);

  // min value is static_table_t::first_unused_index
  index_type current_max_index() const noexcept {
    return entries.size() + static_table_t::first_unused_index - 1;
  }

  // searches both static and dynamic table
  find_result_t find(std::string_view name, std::string_view value) noexcept;
  // searches both static and dynamic table
  // precondition: name <= current_max_index()
  find_result_t find(index_type name, std::string_view value) noexcept;

  // precondition: 0 < index <= current_max_index()
  // Note: returned value may be invalidated on next .add_entry()
  // searches both in static and dynamic tables
  table_entry get_entry(index_type index) const noexcept;

  void reset() noexcept;
  std::pmr::memory_resource* get_resource() const noexcept {
    return _resource;
  }

 private:
  void evict_until_fits_into(size_type bytes) noexcept;
  // precondition: entry now in 'entries'
  index_type indexof(const entry_t& e) const noexcept;
};

// searches in both static and dynamic tables
// dyntab is used only if required (index >= 62)
[[nodiscard]] inline table_entry get_by_index(index_type header_index, dynamic_table_t* dyntab) {
  /*
     Indices strictly greater than the sum of the lengths of both tables
     MUST be treated as a decoding error.
  */
  if (header_index == 0) [[unlikely]]
    throw HPACK_PROTOCOL_ERROR(invalid dynamic table header index == 0);
  if (header_index < static_table_t::first_unused_index)
    return static_table_t::get_entry(header_index);
  if (header_index > dyntab->current_max_index()) [[unlikely]]
    throw HPACK_PROTOCOL_ERROR(invalid dynamic table header index > max);
  return dyntab->get_entry(header_index);
}

}  // namespace hpack
