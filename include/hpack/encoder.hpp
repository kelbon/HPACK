#pragma once

#include "hpack/dynamic_table.hpp"
#include "hpack/strings.hpp"
#include "hpack/integers.hpp"

#include <charconv>

namespace hpack {

struct encoder {
  dynamic_table_t dyntab;

  // 4096 - default size in HTTP/2
  explicit encoder(size_type max_dyntab_size = 4096,
                   std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : dyntab(max_dyntab_size, resource) {
  }

  encoder(encoder&&) = default;
  encoder& operator=(encoder&&) noexcept = default;

  // indexed name and value, for example ":path" "/index.html" from static table
  // or some index from dynamic table
  template <Out O>
  O encode_header_fully_indexed(index_type header_index, O _out) {
    /*
          0   1   2   3   4   5   6   7
        +---+---+---+---+---+---+---+---+
        | 1 |        Index (7+)         |
        +---+---------------------------+
    */
    assert(header_index <= dyntab.current_max_index());
    auto out = noexport::adapt_output_iterator(_out);
    // indexed name and value 0b1...
    *out = 0b1000'0000;
    return noexport::unadapt<O>(encode_integer(header_index, 7, out));
  }

  // only name indexed
  // precondition: header_index present in static or dynamic table
  //
  // Note: will encode to the cached header, but calling this function again will not result in more efficient
  // encoding.
  // Instead, it will send requests to cache the header and this will result in evicting from dynamic table.
  // Its likely you want to use encode_with_cache instead
  template <bool Huffman = false, Out O>
  O encode_header_and_cache(index_type header_index, std::string_view value, O _out) {
    assert(header_index <= dyntab.current_max_index() && header_index != 0);
    auto out = noexport::adapt_output_iterator(_out);
    // indexed name, new value 0b01...
    *out = 0b0100'0000;
    out = encode_integer(header_index, 6, out);
    std::string_view str = get_by_index(header_index, &dyntab).name;
    dyntab.add_entry(str, value);
    return noexport::unadapt<O>(encode_string<Huffman>(value, out));
  }

  // indexes value for future use
  // 'out_index' contains index of 'name' + 'value' pair after encode
  //
  // Note: will encode to the cached header, but calling this function again will not result in more efficient
  // encoding.
  // Instead, it will send requests to cache the header and this will result in evicting from dynamic table.
  // Its likely you want to use encode_with_cache instead
  template <bool Huffman = false, Out O>
  O encode_header_and_cache(std::string_view name, std::string_view value, O _out) {
    /*
    new name
       0   1   2   3   4   5   6   7
     +---+---+---+---+---+---+---+---+
     | 0 | 1 |           0           |
     +---+---+-----------------------+
     | H |     Name Length (7+)      |
     +---+---------------------------+
     |  Name String (Length octets)  |
     +---+---------------------------+
     | H |     Value Length (7+)     |
     +---+---------------------------+
     | Value String (Length octets)  |
     +-------------------------------+
    */
    auto out = noexport::adapt_output_iterator(_out);
    *out = 0b0100'0000;
    ++out;
    out = encode_string<Huffman>(name, out);
    dyntab.add_entry(name, value);
    return noexport::unadapt<O>(encode_string<Huffman>(value, out));
  }

  // encodes header like 'encode_header_and_cache', but uses created cache, so next calls much more efficient
  // than first call
  //
  // Note: does not use static_table. In this case its better to use `encode_header_fully_indexed`
  template <bool Huffman = false, Out O>
  O encode_with_cache(std::string_view name, std::string_view value, O out) {
    find_result_t r = dyntab.find(name, value);
    if (r.value_indexed) [[likely]] {
      // its likely, because only first call will be not cached
      return encode_header_fully_indexed(r.header_name_index, out);
    }
    return encode_header_and_cache<Huffman>(name, value, out);
  }

  // encodes header like 'encode_header_and_cache', but uses created cache, so next calls much more efficient
  // than first call
  //
  // Note: does not use static_table. In this case its better to use `encode_header_fully_indexed`
  template <bool Huffman = false, Out O>
  O encode_with_cache(index_type name, std::string_view value, O out) {
    find_result_t r = dyntab.find(name, value);
    if (r.value_indexed) [[likely]] {
      // its likely, because only first call will be not cached
      return encode_header_fully_indexed(r.header_name_index, out);
    }
    return encode_header_and_cache<Huffman>(name, value, out);
  }

  template <bool Huffman = false, Out O>
  O encode_header_without_indexing(index_type name, std::string_view value, O _out) {
    /*
        0   1   2   3   4   5   6   7
      +---+---+---+---+---+---+---+---+
      | 0 | 0 | 0 | 0 |  Index (4+)   |
      +---+---+-----------------------+
      | H |     Value Length (7+)     |
      +---+---------------------------+
      | Value String (Length octets)  |
      +-------------------------------+
    */
    assert(name <= dyntab.current_max_index());
    auto out = noexport::adapt_output_iterator(_out);
    *out = 0;
    out = encode_integer(name, 4, out);
    return noexport::unadapt<O>(encode_string<Huffman>(value, out));
  }

  template <bool Huffman = false, Out O>
  O encode_header_without_indexing(std::string_view name, std::string_view value, O _out) {
    /*
        0   1   2   3   4   5   6   7
      +---+---+---+---+---+---+---+---+
      | 0 | 0 | 0 | 0 |       0       |
      +---+---+-----------------------+
      | H |     Name Length (7+)      |
      +---+---------------------------+
      |  Name String (Length octets)  |
      +---+---------------------------+
      | H |     Value Length (7+)     |
      +---+---------------------------+
      | Value String (Length octets)  |
      +-------------------------------+
    */
    auto out = noexport::adapt_output_iterator(_out);
    *out = 0;
    ++out;
    out = encode_string<Huffman>(name, out);
    return noexport::unadapt<O>(encode_string<Huffman>(value, out));
  }

  // same as without_indexing, but should not be stored in any proxy memory etc
  template <bool Huffman = false, Out O>
  O encode_header_never_indexing(index_type name, std::string_view value, O _out) {
    /*
        0   1   2   3   4   5   6   7
      +---+---+---+---+---+---+---+---+
      | 0 | 0 | 0 | 1 |  Index (4+)   |
      +---+---+-----------------------+
      | H |     Value Length (7+)     |
      +---+---------------------------+
      | Value String (Length octets)  |
      +-------------------------------+
    */
    assert(name <= dyntab.current_max_index());
    auto out = noexport::adapt_output_iterator(_out);
    *out = 0b0001'0000;
    out = encode_integer(name, 4, out);
    return noexport::unadapt<O>(encode_string<Huffman>(value, out));
  }

  template <bool Huffman = false, Out O>
  O encode_header_never_indexing(std::string_view name, std::string_view value, O _out) {
    /*
        0   1   2   3   4   5   6   7
      +---+---+---+---+---+---+---+---+
      | 0 | 0 | 0 | 1 |       0       |
      +---+---+-----------------------+
      | H |     Name Length (7+)      |
      +---+---------------------------+
      |  Name String (Length octets)  |
      +---+---------------------------+
      | H |     Value Length (7+)     |
      +---+---------------------------+
      | Value String (Length octets)  |
      +-------------------------------+
    */
    auto out = noexport::adapt_output_iterator(_out);
    *out = 0b0001'0000;
    ++out;
    out = encode_string<Huffman>(name, out);
    return noexport::unadapt<O>(encode_string<Huffman>(value, out));
  }

  /*
   default encode, more calculations, less memory
   minimizes size of encoded

   usually its better to encode headers manually, but may be used as "okay somehow encode"

   'Cache' - if true, then will cache headers if they are not in cache yet
   'Huffman' - use Huffman encoding for strings or no (prefer no)

  Note: static table has priority over dynamic table
   (eg indexed name which is indexed in both tables uses index from static,
  same for name + value pairs)
  */
  template <bool Cache = false, bool Huffman = false, Out O>
  O encode(std::string_view name, std::string_view value, O out) {
    find_result_t r2 = static_table_t::find(name, value);
    if (r2.value_indexed)
      return encode_header_fully_indexed(r2.header_name_index, out);
    find_result_t r1 = dyntab.find(name, value);
    if (r1.value_indexed)
      return encode_header_fully_indexed(r1.header_name_index, out);
    if (r2) {
      if constexpr (Cache)
        return encode_header_and_cache<Huffman>(r2.header_name_index, value, out);
      else
        return encode_header_without_indexing<Huffman>(r2.header_name_index, value, out);
    }
    if (r1) {
      if constexpr (Cache)
        return encode_header_and_cache<Huffman>(r1.header_name_index, value, out);
      else
        return encode_header_without_indexing<Huffman>(r1.header_name_index, value, out);
    }
    if constexpr (Cache)
      return encode_header_and_cache<Huffman>(name, value, out);
    else
      return encode_header_without_indexing<Huffman>(name, value, out);
  }

  template <bool Cache = false, bool Huffman = false, Out O>
  O encode(index_type name, std::string_view value, O out) {
    find_result_t r2 = static_table_t::find(name, value);
    if (r2.value_indexed)
      return encode_header_fully_indexed(r2.header_name_index, out);
    find_result_t r1 = dyntab.find(name, value);
    if (r1.value_indexed)
      return encode_header_fully_indexed(r1.header_name_index, out);
    if (r2) {
      if constexpr (Cache)
        return encode_header_and_cache<Huffman>(r2.header_name_index, value, out);
      else
        return encode_header_without_indexing<Huffman>(r2.header_name_index, value, out);
    }
    if (r1) {
      if constexpr (Cache)
        return encode_header_and_cache<Huffman>(r1.header_name_index, value, out);
      else
        return encode_header_without_indexing<Huffman>(r1.header_name_index, value, out);
    }
    if constexpr (Cache)
      return encode_header_and_cache<Huffman>(name, value, out);
    else
      return encode_header_without_indexing<Huffman>(name, value, out);
  }

  /*
  An encoder can choose to use less capacity than this maximum size
     (see Section 6.3), but the chosen size MUST stay lower than or equal
     to the maximum set by the protocol.

     A change in the maximum size of the dynamic table is signaled via a
     dynamic table size update (see Section 6.3).  This dynamic table size
     update MUST occur at the beginning of the first header block
     following the change to the dynamic table size.  In HTTP/2, this
     follows a settings acknowledgment (see Section 6.5.3 of [HTTP2]).
  */
  template <Out O>
  O encode_dynamic_table_size_update(size_type new_size, O _out) noexcept {
    /*
         0   1   2   3   4   5   6   7
       +---+---+---+---+---+---+---+---+
       | 0 | 0 | 1 |   Max size (5+)   |
       +---+---------------------------+
    */
    auto out = noexport::adapt_output_iterator(_out);
    *out = 0b0010'0000;
    auto it = noexport::unadapt<O>(encode_integer(new_size, 5, out));
    dyntab.update_size(new_size);
    return it;
  }

  // encodes :status pseudoheader for server
  // precondition:
  template <Out O>
  O encode_status(int status, O out) {
    using enum static_table_t::values;
    switch (status) {
      case 200:
        return encode_header_fully_indexed(status_200, out);
      case 204:
        return encode_header_fully_indexed(status_204, out);
      case 206:
        return encode_header_fully_indexed(status_206, out);
      case 304:
        return encode_header_fully_indexed(status_304, out);
      case 400:
        return encode_header_fully_indexed(status_400, out);
      case 404:
        return encode_header_fully_indexed(status_404, out);
      case 500:
        return encode_header_fully_indexed(status_500, out);
      default:
        char data[32];
        auto [ptr, ec] = std::to_chars(data, data + 32, status);
        assert(ec == std::errc{});
        // its likely, that server will send this status again, so cache it
        return encode_with_cache(status_200, std::string_view(+data, ptr), out);
    }
  }
};

}  // namespace hpack
