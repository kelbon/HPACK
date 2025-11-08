#pragma once

#include <span>

#include "hpack/encoder.hpp"
#include "hpack/decoder.hpp"

namespace hpack {

// if 'Cache' is true, then tries to use dynamic table for indexing header
// and reduce size in next decoding
// if 'HUffman' is true, then strings will be encoded with huffman encoding
template <bool Cache = false, bool Huffman = false, Out O>
O encode_headers_block(encoder& enc, auto&& range_of_headers, O out) {
  for (auto&& [name, value] : range_of_headers)
    out = enc.template encode<Cache, Huffman>(name, value, out);
  return out;
}

// visitor should accept two string_views, name and value
// ignores (may be handled by caller side):
//  * special case Cookie header separated by key-value pairs
//  * possibly not lowercase headers (it should be protocol error,
//    but nothing breaks anyway, useless check)
template <typename V>
V decode_headers_block(decoder& dec, std::span<const byte_t> bytes, V visitor) {
  const auto* in = bytes.data();
  const auto* e = in + bytes.size();
  header_view header;
  while (in != e) {
    dec.decode_header(in, e, header);
    if (header) [[likely]]  // dynamic size update decoded without error
      visitor(header.name.str(), header.value.str());
  }
  return visitor;
}

// should be used when endpoint wants to skip headers without breaking decoder state
inline void ignore_headers_block(decoder& dec, std::span<const byte_t> bytes) {
  // entry size calculated as name.size() + value.size() + 32,
  // so minimal possible header to fit into dynamic table - "" "" with size 32
  if (dec.dyntab.max_size() < 32)
    return;
  // maintains dynamic table in decoder
  decode_headers_block(dec, bytes, [](std::string_view, std::string_view) {});
}

// should be used when endpoint wants to skip headers without breaking decoder state
inline void ignore_headers_block(decoder& dec, In b, In e) {
  ignore_headers_block(dec, std::span<const byte_t>(b, e));
}

}  // namespace hpack
