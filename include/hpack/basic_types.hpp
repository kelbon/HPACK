#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string_view>
#include <exception>

namespace hpack {

struct protocol_error : std::exception {
  const char* msg = "";

  explicit protocol_error(const char* m) : msg(m) {
  }

  const char* what() const noexcept override {
    return msg;
  }
};

// thrown if there are not enough data for reading header
struct incomplete_data_error : hpack::protocol_error {
  // approx value - how many bytes need to be readen for receiving next part (int or string)
  size_t required_bytes = 0;

  explicit incomplete_data_error(size_t required_bytes_approx)
      : hpack::protocol_error("incomplete data"), required_bytes(required_bytes_approx) {
  }
};

struct sym_info_t {
  uint32_t bits;
  uint8_t bit_count;
};
sym_info_t huffman_table_get(uint8_t index) noexcept;

// uint16_t(-1) if not found
uint16_t huffman_decode_table_find(sym_info_t info);

// integer/string len
using size_type = uint32_t;
// header index
using index_type = uint32_t;
using byte_t = unsigned char;
using In = const byte_t*;

template <typename T>
concept Out = std::output_iterator<T, byte_t>;

#define HPACK_PROTOCOL_ERROR(...) ::hpack::protocol_error("hpack protocol error: " #__VA_ARGS__)

namespace noexport {

// caches first byte for avoiding *it = x == push_back,
// so *it | mask will be into next byte (may be with back_inserter)
template <typename T>
struct adapted_output_iterator {
  T base_it;
  mutable byte_t byte = 0;

  using iterator_category = std::output_iterator_tag;
  using value_type = byte_t;
  using difference_type = std::ptrdiff_t;

  constexpr value_type& operator*() const noexcept {
    return byte;
  }
  constexpr adapted_output_iterator& operator++() {
    *base_it = byte;
    ++base_it;
    return *this;
  }
  constexpr adapted_output_iterator operator++(int) {
    auto cpy = *this;
    ++(*this);
    return cpy;
  }
};

template <Out O>
auto adapt_output_iterator(O it) {
  return adapted_output_iterator<O>{it};
}
template <typename T>
auto adapt_output_iterator(adapted_output_iterator<T> it) {
  return it;
}

inline byte_t* adapt_output_iterator(byte_t* ptr) {
  return ptr;
}
inline byte_t* adapt_output_iterator(std::byte* ptr) {
  return reinterpret_cast<byte_t*>(ptr);
}
inline byte_t* adapt_output_iterator(char* ptr) {
  return reinterpret_cast<byte_t*>(ptr);
}

template <typename Original, typename T>
Original unadapt(adapted_output_iterator<T> it) {
  return Original{std::move(it.base_it)};
}

template <typename Original>
Original unadapt(byte_t* ptr) {
  static_assert(std::is_pointer_v<Original>);
  return reinterpret_cast<Original>(ptr);
}

// standard interface (e.g. for vector) for inserting many values at back
// Note: ignores fact, that someone can make super-bad type with push_back + insert making something wrong
template <typename T>
constexpr inline bool can_insert_many =
    requires(T& value, const char* p) { value.insert(value.end(), p, p); };

// standard back_insert_iterator rly uses protected field exactly for such accessing
template <typename C>
C& access_protected_container(std::back_insert_iterator<C> c) {
  static_assert(std::is_trivially_copyable_v<decltype(c)>);
  struct accessor : std::back_insert_iterator<C> {
    C* get() noexcept {
      return this->container;
    }
  };
  return *accessor{c}.get();
}

template <typename C>
  requires(can_insert_many<C>)
std::back_insert_iterator<C> do_copy_n_fast(const char* ptr, size_t sz, std::back_insert_iterator<C> it) {
  auto& c = access_protected_container(it);
  c.insert(c.end(), ptr, ptr + sz);
  return it;  // back insert iterator does not change on ++/* etc
}

template <typename C>
  requires(can_insert_many<C>)
adapted_output_iterator<std::back_insert_iterator<C>> do_copy_n_fast(
    const char* ptr, size_t sz, adapted_output_iterator<std::back_insert_iterator<C>> it) {
  auto& c = access_protected_container(it.base_it);
  c.insert(c.end(), ptr, ptr + sz);
  return it;  // adapted iterator must be unchanged, since base_it unchanged (its back inserter)
}

// fallback
template <typename It>
It do_copy_n_fast(const char* ptr, size_t sz, It it) {
  return std::copy_n(ptr, sz, std::move(it));
}

// makes copy_n, but for back_insert iterator makes insert(end, It, It + n)
// this converts many push_backs into one uninitialized_copy_n
template <typename It>
It copy_n_fast(const char* ptr, size_t sz, It it) {
  return do_copy_n_fast(ptr, sz, std::move(it));
}

}  // namespace noexport

struct table_entry {
  std::string_view name;   // empty if not found
  std::string_view value;  // empty if no

  constexpr explicit operator bool() const noexcept {
    return !name.empty();
  }
  auto operator<=>(const table_entry&) const = default;
};

struct find_result_t {
  // not found by default
  index_type header_name_index = 0;
  bool value_indexed = false;

  constexpr explicit operator bool() const noexcept {
    return header_name_index != 0;
  }
};

}  // namespace hpack
