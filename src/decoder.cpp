
#include <charconv>
#include <bit>

#include "hpack/integers.hpp"
#include "hpack/decoder.hpp"

namespace {

template <typename F>
struct scope_exit {
  F foo;

  ~scope_exit() {
    foo();
  }
};
template <typename T>
scope_exit(T) -> scope_exit<T>;

}  // namespace

namespace hpack {

[[nodiscard]] constexpr static size_t max_huffman_string_size_after_decode(
    size_type huffman_str_len) noexcept {
  // minimal symbol in table is 5 bit len, so worst case is only 5 bit symbols
  return size_t(huffman_str_len) * 8 / 5;
}

// precondition: in != e
// note: 'len' must be decoded before calling this function
template <Out O>
static O decode_string_huffman(In in, size_type len, O out) {
  In e = in + len;
  sym_info_t info{0, 0};
  int bit_nmb = 0;
  auto next_bit = [&] {
    bool bit = *in & (0b1000'0000 >> bit_nmb);
    if (bit_nmb == 7) {
      bit_nmb = 0;
      ++in;
    } else {
      ++bit_nmb;
    }
    return bit;
  };
  for (;;) {
    // min symbol len in Huffman table is 5 bits
    for (int i = 0; in != e && i < 5; ++i, ++info.bit_count) {
      info.bits <<= 1;
      info.bits += next_bit();
    }
    uint16_t sym;
    while ((sym = huffman_decode_table_find(info)) == uint16_t(-1) && in != e) {
      info.bits <<= 1;
      info.bits += next_bit();
      ++info.bit_count;
    }
    if (sym == 256) [[unlikely]] {
      // EOS
      while (bit_nmb != 0)  // skip padding
        next_bit();
      // https://datatracker.ietf.org/doc/html/rfc7541#section-5.2
      // A Huffman-encoded string literal containing the EOS symbol MUST be treated as a decoding error.
      throw HPACK_PROTOCOL_ERROR(Huffman encoded string literal containing the EOS symbol);
    }
    if (sym != uint16_t(-1)) {
      info = {};
      *out = byte_t(sym);
      ++out;
    } else if (info.bit_count > 7) {
      // symbol is not resolved and its > 7 bit,
      // https://datatracker.ietf.org/doc/html/rfc7541#section-5.2
      // A padding strictly longer than 7 bits MUST be treated as a decoding error.
      throw HPACK_PROTOCOL_ERROR(padding strictly longer than 7 bits);
    }
    if (in == e) {
      if (std::countr_one(info.bits) != info.bit_count)
        throw HPACK_PROTOCOL_ERROR(invalid padding);
      return out;
    }
  }
  return out;
}

void decoded_string::set_huffman(const char* ptr, size_type len) {
  // also handles case when len == 0
  if (bytes_allocated() >= max_huffman_string_size_after_decode(len)) {
    const byte_t* in = (const byte_t*)ptr;
    // const cast because im owner of pointer (its allocated by malloc)
    char* end = decode_string_huffman(in, len, const_cast<char*>(data));
    sz = end - data;
  } else {
    size_t sz_to_allocate = std::bit_ceil(max_huffman_string_size_after_decode(len));
    auto was_allocated = allocated_sz_log2;
    allocated_sz_log2 = std::bit_width(sz_to_allocate) - 1;
    const char* old_data = data;
    data = (char*)malloc(sz_to_allocate);
    bool failed = true;
    scope_exit free_mem{[&] {
      if (failed) {
        free((void*)data);
        data = old_data;
        allocated_sz_log2 = was_allocated;
      } else {
        if (was_allocated != -1)
          free((void*)old_data);
      }
    }};
    // recursive call into branch where we have enough memory
    set_huffman(ptr, len);

    failed = false;
  }
}

// decodes partly indexed / new-name pairs
static void decode_header_impl(In& in, In e, uint8_t N, dynamic_table_t& dyntab, header_view& out) {
  index_type index = decode_integer(in, e, N);
  if (index == 0)
    decode_string(in, e, out.name);
  else
    out.name = get_by_index(index, &dyntab).name;
  decode_string(in, e, out.value);
}

static void decode_header_fully_indexed(In& in, In e, dynamic_table_t& dyntab, header_view& out) {
  assert(*in & 0b1000'0000);
  index_type index = decode_integer(in, e, 7);
  table_entry entry = get_by_index(index, &dyntab);
  out = entry;
}

static void decode_header_incremental_indexing(In& in, In e, dynamic_table_t& dyntab, header_view& out) {
  assert(in != e && *in & 0b0100'0000);
  decode_header_impl(in, e, 6, dyntab, out);
  dyntab.add_entry(out.name.str(), out.value.str());
}

static void decode_header_without_indexing(In& in, In e, dynamic_table_t& dyntab, header_view& out) {
  assert(in != e && (*in & 0x1111'0000) == 0);
  return decode_header_impl(in, e, 4, dyntab, out);
}

static void decode_header_never_indexing(In& in, In e, dynamic_table_t& dyntab, header_view& out) {
  assert(in != e && *in & 0b0001'0000);
  return decode_header_impl(in, e, 4, dyntab, out);
}

// returns requested new size of dynamic table
static size_type decode_dynamic_table_size_update(In& in, In e) {
  assert(*in & 0b0010'0000);  // marker of dynamic table size update
  if (*in & 0b1100'0000)
    throw HPACK_PROTOCOL_ERROR(invalid dynamic table size update);
  return decode_integer(in, e, 5);
}

void decode_string(In& in, In e, decoded_string& out) {
  if (in == e)
    throw incomplete_data_error(1);
  bool is_huffman = *in & 0b1000'0000;
  size_type str_len = decode_integer(in, e, 7);
  if (str_len > std::distance(in, e))
    throw incomplete_data_error(str_len - std::distance(in, e));
  if (is_huffman)
    out.set_huffman((const char*)in, str_len);
  else
    out.set_not_huffman((const char*)in, str_len);
  in += str_len;
}

void decoder::decode_header(In& in, In e, header_view& out) {
  assert(in != e);
  if (*in & 0b1000'0000)
    return decode_header_fully_indexed(in, e, dyntab, out);
  if (*in & 0b0100'0000)
    return decode_header_incremental_indexing(in, e, dyntab, out);
  if (*in & 0b0010'0000) {
    dyntab.update_size(decode_dynamic_table_size_update(in, e));
    out.name.reset();
    out.value.reset();
    return;
  }
  if (*in & 0b0001'0000)
    return decode_header_never_indexing(in, e, dyntab, out);
  if ((*in & 0b1111'0000) == 0)
    return decode_header_without_indexing(in, e, dyntab, out);
  throw HPACK_PROTOCOL_ERROR(invalid field representation);
}

int decoder::decode_response_status(In& in, In e) {
  if (in == e)
    // empty headers block, while atleast ':status' required
    throw HPACK_PROTOCOL_ERROR(required ":status" pseudoheader not present);
  if (*in & 0b1000'0000) {
    // fast path, fully indexed
    auto in_before = in;
    index_type index = decode_integer(in, e, 7);
    switch (index) {
      case static_table_t::status_200:
        return 200;
      case static_table_t::status_204:
        return 204;
      case static_table_t::status_206:
        return 206;
      case static_table_t::status_304:
        return 304;
      case static_table_t::status_400:
        return 400;
      case static_table_t::status_404:
        return 404;
      case static_table_t::status_500:
        return 500;
    }
    in = in_before;
  }
  // first header of response must be required pseudoheader,
  // which is (for response) only one - ":status"
  header_view header;
  do {
    decode_header(in, e, header);
  } while (!header && in != e);
  if (!header)
    throw HPACK_PROTOCOL_ERROR(header block only with dynamic_table_size_update);
  std::string_view code = header.value.str();
  if (header.name.str() != ":status" || code.size() != 3)
    throw HPACK_PROTOCOL_ERROR(invalid first header, expected ":status" with 3 digit code);
  int status_code;
  auto [_, err] = std::from_chars(code.data(), code.data() + 3, status_code);
  if (err != std::errc{})
    throw HPACK_PROTOCOL_ERROR(invalid ":status" value, not a number);
  return status_code;
}

}  // namespace hpack
