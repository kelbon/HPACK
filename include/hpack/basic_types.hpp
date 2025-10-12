#pragma once

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
  incomplete_data_error() : hpack::protocol_error("incomplete data") {
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
