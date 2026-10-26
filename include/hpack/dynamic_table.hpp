#pragma once

#include <memory_resource>
#include <vector>
#include <cstring>  // memcpy
#include <cassert>
#include <memory>

#include <boost/intrusive/slist.hpp>

#include "hpack/basic_types.hpp"
#include "hpack/static_table.hpp"

namespace hpack {

namespace bi = boost::intrusive;

struct dynamic_table_t {
  using index_hook_t = bi::slist_member_hook<bi::link_mode<bi::normal_link>>;

  struct entry_t {
    // хуки хеш-индексов, см. 'by_name' и 'by_pair'
    index_hook_t name_hook;
    index_hook_t pair_hook;
    const uint64_t name_hash;
    const uint64_t pair_hash;  // хеш имени + значения
    const size_type name_end;
    const size_type value_end;
    const size_t _insert_c;
    char data[];

    entry_t(size_type name_len, size_type value_len, size_t insert_c, uint64_t name_hash,
            uint64_t pair_hash) noexcept
        : name_hash(name_hash),
          pair_hash(pair_hash),
          name_end(name_len),
          value_end(name_len + value_len),
          _insert_c(insert_c) {
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

    static entry_t* create(std::string_view name, std::string_view value, size_t insert_c, uint64_t name_hash,
                           uint64_t pair_hash, std::pmr::memory_resource* resource) {
      assert(resource);
      void* bytes = resource->allocate(sizeof(entry_t) + name.size() + value.size(), alignof(entry_t));
      entry_t* e = new (bytes) entry_t(name.size(), value.size(), insert_c, name_hash, pair_hash);
      if (!name.empty())
        memcpy(+e->data, name.data(), name.size());
      if (!value.empty())
        memcpy(e->data + name.size(), value.data(), value.size());
      return e;
    }
    static void destroy(const entry_t* e, std::pmr::memory_resource* resource) noexcept {
      assert(e && resource);
      size_t value_end = e->value_end;
      std::destroy_at(e);
      resource->deallocate((void*)e, sizeof(entry_t) + value_end, alignof(entry_t));
    }
  };

 private:
  // ring buffer of entries, entry with insert number 'c' is in ring[c & (ring.size() - 1)]
  // ring.size() is 0 or power of 2
  // invariant: _count <= ring.size(), live entries are not nullptrs
  std::vector<entry_t*> ring;
  // count of entries in table
  size_type _count = 0;

  // корзина хеш-индекса: односвязный интрузивный список записей, ключ которых попал в эту корзину.
  // linear<true>: список заканчивается nullptr (а не замкнут на заголовок), поэтому список — это просто
  // указатель на первую запись и его можно хранить в std::vector
  template <index_hook_t entry_t::* Hook>
  using bucket_t = bi::slist<entry_t, bi::member_hook<entry_t, index_hook_t, Hook>, bi::linear<true>,
                             bi::constant_time_size<false>, bi::cache_last<false>>;
  using name_bucket_t = bucket_t<&entry_t::name_hook>;
  using pair_bucket_t = bucket_t<&entry_t::pair_hook>;
  static_assert(sizeof(name_bucket_t) == sizeof(void*));

  // Хеш-индексы для find:
  //   by_name: имя -> самая новая запись с этим именем,
  //   by_pair: имя + значение -> самая новая запись с ними.
  // Количество корзин всегда равно ring.size(), корзина выбирается по hash & (size - 1).
  //
  // В таблице может быть несколько записей с одним ключом, но в индексе лежит только самая новая из них:
  // при добавлении новой записи старая с тем же ключом удаляется из списка корзины (см. index_add).
  // Поэтому в каждой корзине все ключи разные, и поиск не перебирает дубликаты.
  // Это корректно, потому что записи вытесняются строго от старых к новым: самая новая запись с ключом
  // вытесняется последней, когда других записей с этим ключом уже нет. Значит, пока ключ есть в таблице,
  // в индексе по нему лежит живая запись.
  std::vector<name_bucket_t> by_name;
  std::vector<pair_bucket_t> by_pair;

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
  // precondition: `protocol_max_size` >= `max_size`
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

  void swap(dynamic_table_t&) noexcept;

  friend void swap(dynamic_table_t& a, dynamic_table_t& b) noexcept {
    return a.swap(b);
  }

  ~dynamic_table_t();

  [[nodiscard]] bool can_fit_entry(size_t new_entry_size) const {
    return new_entry_size <= _max_size;
  }
  // возвращает true если такая entry вообще может уместится в таблицу, хотя бы одна
  [[nodiscard]] bool can_fit_entry(std::string_view name, std::string_view value) const {
    size_type new_entry_size = name.size() + value.size() + 32;
    return can_fit_entry(new_entry_size);
  }

  // возвращает добавленную entry (или nullptr если вообще не влезает)
  // индекс новой добавленной entry в динамической таблице всегда static_table_t::first_unused_index
  entry_t* add_entry(std::string_view name, std::string_view value);

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
    return _count + static_table_t::first_unused_index - 1;
  }

  // searches both static and dynamic table
  find_result_t find(std::string_view name, std::string_view value) const noexcept;
  // returns index of newest entry with 'name' or 0 if not found
  // Note: searches only dynamic table
  index_type find_name(std::string_view name) const noexcept;
  // searches both static and dynamic table
  // precondition: name <= current_max_index()
  find_result_t find(index_type name, std::string_view value) const noexcept;

  // precondition: 0 < index <= current_max_index()
  // Note: returned value may be invalidated on next .add_entry()
  // searches both in static and dynamic tables
  table_entry get_entry(index_type index) const noexcept;

  void reset() noexcept;
  std::pmr::memory_resource* get_resource() const noexcept {
    return _resource;
  }

 private:
  // returns entry with insert number 'c'
  entry_t*& entry_at(size_t c) noexcept {
    return ring[c & (ring.size() - 1)];
  }
  entry_t* const& entry_at(size_t c) const noexcept {
    return ring[c & (ring.size() - 1)];
  }
  // insert number of the oldest entry
  // if table is empty, its insert number of the next entry (no entry to access)
  size_t oldest() const noexcept {
    return _insert_count - _count + 1;
  }
  // maintains invariant: rint.size() is power of 2 or 0
  void grow_ring();

  struct evict_count_t {
    size_type count = 0;  // count of oldest entries to evict
    size_type bytes = 0;  // size of these entries
  };
  // counts oldest entries which must be evicted for table to fit into 'size_limit' bytes, does not evict
  evict_count_t need_evict_to_fit(size_type size_limit) const noexcept;
  // removes and destroys oldest entries
  void evict(evict_count_t) noexcept;
  // добавляет 'e' в хеш-индексы, более старая запись с тем же ключом (если есть) удаляется из индекса
  void index_add(entry_t& e) noexcept;
  // удаляет 'e' из хеш-индексов, если она там есть (то есть если это самая новая запись со своим ключом)
  void index_remove(entry_t& e) noexcept;
  // precondition: entry now in table
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
