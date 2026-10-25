#pragma once

#include <algorithm>
#include <cstdint>

#include "hpack/basic_types.hpp"
#include "hpack/integers.hpp"

namespace hpack {

namespace noexport {

struct huffman_code_t {
  uint32_t code;  // most significant bit first
  uint8_t len;    // in bits
};

// index is symbol, 256 - EOS
inline constexpr huffman_code_t huffman_codes[257] = {
#define HUFFMAN_TABLE(index, bits, bitcount) {0b##bits, bitcount},
#include "hpack/huffman_table.def"
};

}  // namespace noexport

template <Out O>
O encode_string_huffman(std::string_view str, O _out) {
  auto out = noexport::adapt_output_iterator(_out);
  // precalculate size
  // (size should be before string and len in bits depends on 'len' value)
  size_type len_after_encode = 0;
  for (char c : str)
    len_after_encode += noexport::huffman_codes[uint8_t(c)].len;
  *out = 0b1000'0000;  // set H bit
  // padding is always < 8 bits
  // https://datatracker.ietf.org/doc/html/rfc7541#section-5.2
  out = encode_integer((len_after_encode + 7) / 8, 7, out);
  // codes are appended to the low bits of 'acc', 'bits' - count of low bits not written yet (< 8 between
  // symbols). Max code len is 30, so 'acc' never loses unwritten bits
  uint64_t acc = 0;
  int bits = 0;
  for (char c : str) {
    noexport::huffman_code_t sym = noexport::huffman_codes[uint8_t(c)];
    acc = (acc << sym.len) | sym.code;
    bits += sym.len;
    while (bits >= 8) {
      bits -= 8;
      *out = byte_t(acc >> bits);
      ++out;
    }
  }
  if (bits > 0) {
    // padding MUST be formed from most significant bits of EOS (all ones)
    *out = byte_t((acc << (8 - bits)) | (0xFF >> bits));
    ++out;
  }
  return noexport::unadapt<O>(out);
}

template <bool Huffman = false, Out O>
O encode_string(std::string_view str, O _out) {
  auto out = noexport::adapt_output_iterator(_out);
  /*
       0   1   2   3   4   5   6   7
     +---+---+---+---+---+---+---+---+
     | H |    String Length (7+)     |
     +---+---------------------------+
     |  String Data (Length octets)  |
     +-------------------------------+
  */
  if constexpr (!Huffman) {
    *out = 0;  // set H bit to 0
    out = encode_integer(str.size(), 7, out);
    out = noexport::copy_n_fast(str.data(), str.size(), out);
  } else {
    out = encode_string_huffman(str, out);
  }
  return noexport::unadapt<O>(out);
}

// in decoder.hpp
struct decoded_string;

void decode_string(In& in, In e, decoded_string& out);

}  // namespace hpack
