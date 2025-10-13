#pragma once

#include "hpack/basic_types.hpp"
#include "hpack/dynamic_table.hpp"

#include <span>
#include <utility>

namespace hpack {

// helper for `decode_string`. Temporal storage for decoded strings
//
// may store string_view (AND NOT OWN IT!), may store huffman decoded string
// and in this case memory will be allocated and owned
// tries to reuse memory when new huffman string setted and memory already allocated
struct decoded_string {
 private:
  const char* data = nullptr;
  size_type sz = 0;
  // != -1 after decoding huffman str
  // default -1 for removing ambiguity between 'not allocated' and 'allocated 1 byte' (log2(1) == 0)
  int8_t allocated_sz_log2 = -1;

 public:
  decoded_string() = default;

  decoded_string(const char* ptr, size_type len, bool is_huffman_encoded) : data(ptr), sz(len) {
    if (!is_huffman_encoded)
      return;
    set_huffman(ptr, len);
  }

  // precondition: str.size() less then max of size_type
  decoded_string(std::string_view str, bool is_huffman_encoded)
      : decoded_string(str.data(), str.size(), is_huffman_encoded) {
    assert(std::in_range<size_type>(str.size()));
  }

  decoded_string(decoded_string&& other) noexcept {
    swap(other);
  }

  decoded_string& operator=(decoded_string&& other) noexcept {
    swap(other);
    return *this;
  }

  void set_huffman(const char* ptr, size_type len);
  // Note: *this will not own `ptr` memory, only contain a view
  void set_not_huffman(const char* ptr, size_type len) {
    reset();
    data = ptr;
    sz = len;
  }

  // not huffman encoded string
  decoded_string& operator=(std::string_view str) noexcept {
    assert(std::in_range<size_type>(str.size()));
    set_not_huffman(str.data(), str.size());
    return *this;
  }

  void swap(decoded_string& other) noexcept {
    std::swap(data, other.data);
    std::swap(sz, other.sz);
    std::swap(allocated_sz_log2, other.allocated_sz_log2);
  }

  friend void swap(decoded_string& l, decoded_string& r) noexcept {
    l.swap(r);
  }

  ~decoded_string() {
    reset();
  }

  void reset() noexcept {
    if (allocated_sz_log2 != -1)
      free((void*)data);
    data = nullptr;
    sz = 0;
    allocated_sz_log2 = -1;
  }

  [[nodiscard]] size_t bytes_allocated() const noexcept {
    if (allocated_sz_log2 == -1)
      return 0;
    return 1 << allocated_sz_log2;
  }

  [[nodiscard]] std::string_view str() const noexcept {
    return std::string_view(data, sz);
  }

  // true if not empty
  explicit operator bool() const noexcept {
    return sz != 0;
  }

  bool operator==(const decoded_string& other) const noexcept {
    return str() == other.str();
  }
  bool operator==(std::string_view other) const noexcept {
    return str() == other;
  }

  std::strong_ordering operator<=>(const decoded_string& other) const noexcept {
    return str() <=> other.str();
  }

  std::strong_ordering operator<=>(std::string_view other) const noexcept {
    return str() <=> other;
  }
};

// note: decoding next header invalidates previous header
struct header_view {
  decoded_string name;
  decoded_string value;

  // header may be not present if default contructed or table_size_update happen instead of header
  explicit operator bool() const noexcept {
    return !!name;
  }

  header_view& operator=(header_view&&) = default;
  header_view& operator=(table_entry entry) {
    name = entry.name;
    value = entry.value;
    return *this;
  }
};

void decode_string(In& in, In e, decoded_string& out);

struct decoder {
  dynamic_table_t dyntab;

  // 4096 - default size in HTTP/2
  explicit decoder(size_type max_dyntab_size = 4096,
                   std::pmr::memory_resource* resource = std::pmr::get_default_resource())
      : dyntab(max_dyntab_size, resource) {
  }

  decoder(decoder&&) = default;
  decoder& operator=(decoder&&) noexcept = default;

  /*
   Note: this function ignores special 'cookie' header case
   https://www.rfc-editor.org/rfc/rfc7540#section-8.1.2.5
   and protocol error if decoded header name is not lowercase
  */
  void decode_header(In& in, In e, header_view& out);

  // returns status code
  // its always first header of response, so 'in' must point to first byte of headers block
  int decode_response_status(In& in, In e);
};

// eats parts of headers fragment, allowing to parse CONTINUATIONS in HTTP/2 part by part
struct stream_decoder {
 private:
  decoder& dec;
  std::vector<byte_t> incomplete;

  // returns where first unparsed byte starts
  template <typename V>
  In do_feed(std::span<byte_t> chunk, bool last_chunk, V&& visitor, size_t& approx) {
    In in = chunk.data();
    In e = in + chunk.size();
    assert(in != e);
    In in_just_before_fail;
    approx = 0;
    try {
      header_view header;
      while (in != e) {
        in_just_before_fail = in;

        dec.decode_header(in, e, header);

        if (header) [[likely]]  // dynamic size update decoded without error
          visitor(header.name.str(), header.value.str());
      }
      // successfully parsed all headers
      return e;
    } catch (hpack::incomplete_data_error& e) {
      approx = e.required_bytes;
      if (last_chunk)
        throw;
      return in_just_before_fail;
    }
  }

 public:
  stream_decoder(decoder& d) noexcept : dec(d) {
  }

  stream_decoder(stream_decoder&&) = delete;
  void operator=(stream_decoder&&) = delete;

  // `visitor` should accept two string_views, name and value
  // optimized for case when each `chunk` >> 1 header
  // returns approx count of bytes required for receiving next part of header
  // or 0 if there are no unhandled data in chunk
  // e.g. may be used to detect too big string before receiving it
  template <typename V>
  size_t feed(std::span<byte_t> chunk, bool last_chunk, V&& visitor) {
    if (chunk.empty()) [[unlikely]]
      return 0;
    size_t approx;
    if (!incomplete.empty()) {
      incomplete.insert(incomplete.end(), chunk.begin(), chunk.end());
      In i = do_feed(incomplete, last_chunk, std::forward<V>(visitor), approx);
      In e = incomplete.data() + incomplete.size();
      auto sz = e - i;
      // avoid UB on .assign (iterators into vector itself)
      memmove(incomplete.data(), i, sz);
      incomplete.resize(sz);
    } else {
      In i = do_feed(chunk, last_chunk, std::forward<V>(visitor), approx);
      incomplete.assign(i, In(chunk.data()) + chunk.size());
    }
    return approx;
  }

  // returns such value, that `pending_data_size` + `feed` result == almost exact value of bytes which will be
  // stored until next part of header (not header itself!) will be parsed.
  // Note, there are only 2 parts of header - name and value
  [[nodiscard]] size_t pending_data_size() const noexcept {
    return incomplete.size();
  }

  // makes possible start from beginning, forgetting previous `feed` calls
  void clear() noexcept {
    incomplete.clear();
  }
};

}  // namespace hpack
