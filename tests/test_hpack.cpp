#include "hpack/hpack.hpp"

#include <random>
#include <deque>
#include <bit>

#define TEST(name) static void test_##name()
#define error_if(...)    \
  if (!!(__VA_ARGS__)) { \
    exit(__LINE__);      \
  }

using hpack::decode_integer;
using hpack::encode_integer;

static void test_number(uint32_t value_to_encode, uint8_t prefix_length, uint32_t expected_bytes_filled = 0) {
  hpack::encoder enc;

  uint8_t chars[10] = {};
  uint8_t* encoded_end = encode_integer(value_to_encode, prefix_length, chars);
  if (expected_bytes_filled != 0)
    error_if(encoded_end - chars != expected_bytes_filled);
  const uint8_t* in = chars;
  uint32_t x = decode_integer(in, encoded_end, prefix_length);
  error_if(x != value_to_encode);
  error_if(in != encoded_end);  // decoded all what encoded
}

TEST(encode_decode_integers) {
  test_number(1337, 5, 3);
  test_number(10, 5, 1);
  test_number(31, 5, 2);
  test_number(32, 5, 2);
  test_number(127, 5, 2);
  test_number(128, 5, 2);
  test_number(255, 8, 2);
  test_number(256, 8, 2);
  test_number(16383, 5, 3);
  test_number(100000, 5, 4);
  test_number(1048576, 5, 4);
  test_number(0, 5, 1);
  test_number(1, 5, 1);
  test_number(std::numeric_limits<hpack::size_type>::max(), 5, 6);
  hpack::encoder enc;

  uint8_t chars[10] = {};
  uint8_t* encoded_end =
      encode_integer<uint64_t>(uint64_t(std::numeric_limits<uint32_t>::max()) + 1, 6, chars);
  const uint8_t* in = chars;
  try {
    (void)decode_integer<uint32_t>(in, encoded_end, 6);
  } catch (...) {
    // must throw overflow
    return;
  }
  error_if(true);
}

// vector, ordering matters!
using headers_t = std::vector<std::pair<std::string, std::string>>;
using bytes_t = std::vector<uint8_t>;

// encoder by ref, because HPACK it statefull (between requests/responses)
template <bool Huffman = false>
static void test_encode(hpack::encoder& enc, hpack::size_type expected_dyntab_size,
                        headers_t headers_to_encode, bytes_t expected_encoded_bytes,
                        headers_t expected_dyntab_content) {
  std::vector<uint8_t> bytes(expected_encoded_bytes.size(), ~0);
  auto* out = bytes.data();
  for (auto&& [name, value] : headers_to_encode)
    out = enc.template encode</*Cache=*/true, Huffman>(name, value, out);
  error_if(bytes != expected_encoded_bytes);
  error_if(enc.dyntab.current_size() != expected_dyntab_size);
  for (auto&& [name, value] : expected_dyntab_content) {
    auto res = enc.dyntab.find(name, value);
    error_if(!res);
  }
}

static void test_decode(hpack::decoder& enc, hpack::size_type expected_dyntab_size,
                        headers_t expected_decoded_headers, bytes_t bytes_to_decode,
                        headers_t expected_dyntab_content) {
  assert(!bytes_to_decode.empty());
  hpack::header_view hdr;
  headers_t decoded;
  const uint8_t* in = bytes_to_decode.data();
  auto* e = in + bytes_to_decode.size();
  while (in != e) {
    enc.decode_header(in, e, hdr);
    decoded.emplace_back(hdr.name.str(), hdr.value.str());
  }
  error_if(decoded != expected_decoded_headers);
  error_if(enc.dyntab.current_size() != expected_dyntab_size);
  error_if(in - 1 != &bytes_to_decode.back());
  size_t i = hpack::static_table_t::first_unused_index;
  size_t imax = i + expected_dyntab_content.size();
  for (auto&& [name, value] : expected_dyntab_content) {
    error_if(!enc.dyntab.find(name, value));
    ++i;
  }
}

// https://www.rfc-editor.org/rfc/rfc7541#appendix-C.3.1
TEST(encode_decode1) {
  hpack::encoder sender(164);
  hpack::decoder receiver(164);
  // first request
  {
    headers_t headers{
        {":method", "GET"},
        {":scheme", "http"},
        {":path", "/"},
        {":authority", "www.example.com"},
    };
    headers_t cached_headers{
        {":authority", "www.example.com"},
    };
    bytes_t bytes_expected{
        0x82, 0x86, 0x84, 0x41, 0x0f, 0x77, 0x77, 0x77, 0x2e, 0x65,
        0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d,
    };

    test_encode(sender, 57, headers, bytes_expected, cached_headers);

    test_decode(receiver, 57, headers, bytes_expected, cached_headers);
  }
  // second request
  {
    headers_t headers{
        {":method", "GET"},
        {":scheme", "http"},
        {":path", "/"},
        {":authority", "www.example.com"},
        {"cache-control", "no-cache"},
    };
    bytes_t bytes_expected{
        0x82, 0x86, 0x84, 0xbe, 0x58, 0x08, 0x6e, 0x6f, 0x2d, 0x63, 0x61, 0x63, 0x68, 0x65,
    };
    headers_t cached_headers{
        {"cache-control", "no-cache"},
        {":authority", "www.example.com"},
    };
    test_encode(sender, 110, headers, bytes_expected, cached_headers);
    test_decode(receiver, 110, headers, bytes_expected, cached_headers);
  }
  // third request
  {
    headers_t headers{
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/index.html"},
        {":authority", "www.example.com"},
        {"custom-key", "custom-value"},
    };
    bytes_t bytes_expected{0x82, 0x87, 0x85, 0xbf, 0x40, 0x0a, 0x63, 0x75, 0x73, 0x74,
                           0x6f, 0x6d, 0x2d, 0x6b, 0x65, 0x79, 0x0c, 0x63, 0x75, 0x73,
                           0x74, 0x6f, 0x6d, 0x2d, 0x76, 0x61, 0x6c, 0x75, 0x65};
    headers_t cached_headers{
        {"custom-key", "custom-value"},
        {"cache-control", "no-cache"},
        {":authority", "www.example.com"},
    };
    test_encode(sender, 164, headers, bytes_expected, cached_headers);
    test_decode(receiver, 164, headers, bytes_expected, cached_headers);
  }
}

TEST(encode_decode_huffman1) {
  hpack::encoder sender(164);
  hpack::decoder receiver(164);
  // first request
  {
    headers_t headers{
        {":method", "GET"},
        {":scheme", "http"},
        {":path", "/"},
        {":authority", "www.example.com"},
    };
    headers_t cached_headers{
        {":authority", "www.example.com"},
    };
    bytes_t bytes_expected{
        0x82, 0x86, 0x84, 0x41, 0x8c, 0xf1, 0xe3, 0xc2, 0xe5, 0xf2, 0x3a, 0x6b, 0xa0, 0xab, 0x90, 0xf4, 0xff,
    };

    test_encode<true>(sender, 57, headers, bytes_expected, cached_headers);

    test_decode(receiver, 57, headers, bytes_expected, cached_headers);
  }
  // second request
  {
    headers_t headers{
        {":method", "GET"},
        {":scheme", "http"},
        {":path", "/"},
        {":authority", "www.example.com"},
        {"cache-control", "no-cache"},
    };
    bytes_t bytes_expected{
        0x82, 0x86, 0x84, 0xbe, 0x58, 0x86, 0xa8, 0xeb, 0x10, 0x64, 0x9c, 0xbf,
    };
    headers_t cached_headers{
        {"cache-control", "no-cache"},
        {":authority", "www.example.com"},
    };
    test_encode<true>(sender, 110, headers, bytes_expected, cached_headers);
    test_decode(receiver, 110, headers, bytes_expected, cached_headers);
  }
  // third request
  {
    headers_t headers{
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/index.html"},
        {":authority", "www.example.com"},
        {"custom-key", "custom-value"},
    };
    bytes_t bytes_expected{
        0x82, 0x87, 0x85, 0xbf, 0x40, 0x88, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xa9,
        0x7d, 0x7f, 0x89, 0x25, 0xa8, 0x49, 0xe9, 0x5b, 0xb8, 0xe8, 0xb4, 0xbf,
    };
    headers_t cached_headers{
        {"custom-key", "custom-value"},
        {"cache-control", "no-cache"},
        {":authority", "www.example.com"},
    };
    test_encode<true>(sender, 164, headers, bytes_expected, cached_headers);
    test_decode(receiver, 164, headers, bytes_expected, cached_headers);
  }
}

// similar to first example, but forces eviction of dynamic table entries
TEST(encode_decode_with_eviction) {
  hpack::encoder sender(256);
  hpack::decoder receiver(256);
  // first response
  {
    headers_t headers{
        {":status", "302"},
        {"cache-control", "private"},
        {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"location", "https://www.example.com"},
    };
    headers_t cached_headers{
        {"location", "https://www.example.com"},
        {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"cache-control", "private"},
        {":status", "302"},
    };
    bytes_t bytes_expected = {
        0x48, 0x03, 0x33, 0x30, 0x32, 0x58, 0x07, 0x70, 0x72, 0x69, 0x76, 0x61, 0x74, 0x65,
        0x61, 0x1d, 0x4d, 0x6f, 0x6e, 0x2c, 0x20, 0x32, 0x31, 0x20, 0x4f, 0x63, 0x74, 0x20,
        0x32, 0x30, 0x31, 0x33, 0x20, 0x32, 0x30, 0x3a, 0x31, 0x33, 0x3a, 0x32, 0x31, 0x20,
        0x47, 0x4d, 0x54, 0x6e, 0x17, 0x68, 0x74, 0x74, 0x70, 0x73, 0x3a, 0x2f, 0x2f, 0x77,
        0x77, 0x77, 0x2e, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x2e, 0x63, 0x6f, 0x6d,
    };

    test_encode(sender, 222, headers, bytes_expected, cached_headers);

    test_decode(receiver, 222, headers, bytes_expected, cached_headers);
  }
  // second response
  {
    headers_t headers{
        {":status", "307"},
        {"cache-control", "private"},
        {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"location", "https://www.example.com"},
    };
    headers_t cached_headers{
        {":status", "307"},
        {"location", "https://www.example.com"},
        {"date", "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"cache-control", "private"},
    };
    bytes_t bytes_expected = {0x48, 0x03, 0x33, 0x30, 0x37, 0xc1, 0xc0, 0xbf};

    test_encode(sender, 222, headers, bytes_expected, cached_headers);

    test_decode(receiver, 222, headers, bytes_expected, cached_headers);
  }
  // third response
  {
    headers_t headers{
        {":status", "200"},
        {"cache-control", "private"},
        {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
        {"location", "https://www.example.com"},
        {"content-encoding", "gzip"},
        {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
    };
    headers_t cached_headers{
        {"set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
        {"content-encoding", "gzip"},
        {"date", "Mon, 21 Oct 2013 20:13:22 GMT"},
    };
    bytes_t bytes_expected = {
        0x88, 0xc1, 0x61, 0x1d, 0x4d, 0x6f, 0x6e, 0x2c, 0x20, 0x32, 0x31, 0x20, 0x4f, 0x63, 0x74, 0x20, 0x32,
        0x30, 0x31, 0x33, 0x20, 0x32, 0x30, 0x3a, 0x31, 0x33, 0x3a, 0x32, 0x32, 0x20, 0x47, 0x4d, 0x54, 0xc0,
        0x5a, 0x04, 0x67, 0x7a, 0x69, 0x70, 0x77, 0x38, 0x66, 0x6f, 0x6f, 0x3d, 0x41, 0x53, 0x44, 0x4a, 0x4b,
        0x48, 0x51, 0x4b, 0x42, 0x5a, 0x58, 0x4f, 0x51, 0x57, 0x45, 0x4f, 0x50, 0x49, 0x55, 0x41, 0x58, 0x51,
        0x57, 0x45, 0x4f, 0x49, 0x55, 0x3b, 0x20, 0x6d, 0x61, 0x78, 0x2d, 0x61, 0x67, 0x65, 0x3d, 0x33, 0x36,
        0x30, 0x30, 0x3b, 0x20, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x3d, 0x31};

    test_encode(sender, 215, headers, bytes_expected, cached_headers);

    test_decode(receiver, 215, headers, bytes_expected, cached_headers);
  }
}

TEST(huffman) {
  std::string_view str = "hello world";
  uint8_t buf[20];
  auto* encoded_end = hpack::encode_string<true>(str, +buf);
  const uint8_t* in = +buf;
  hpack::decoded_string out;
  hpack::decode_string(in, encoded_end, out);
  error_if(out.str() != str);
}

TEST(huffman_rand) {
  std::mt19937 gen(155);
  bytes_t bytes;
  for (int i = 0; i < 1000; ++i)
    bytes.push_back(std::uniform_int_distribution<int>(0, 255)(gen));
  bytes_t encoded;
  hpack::encode_string_huffman(std::string_view((char*)bytes.data(), (char*)bytes.data() + bytes.size()),
                               std::back_inserter(encoded));
  const uint8_t* in = encoded.data();
  hpack::decoded_string out;
  hpack::decode_string(in, in + encoded.size(), out);
  error_if(in != encoded.data() + encoded.size());
  error_if(out.str() != std::string_view((char*)bytes.data(), bytes.size()));
}

TEST(huffman_table_itself) {
#define HUFFMAN_TABLE(index, bits, bitcount)                                                              \
  {                                                                                                       \
    error_if(hpack::huffman_decode_table_find(hpack::sym_info_t{0b##bits, bitcount}) != uint16_t(index)); \
  }
#include "hpack/huffman_table.def"
}

TEST(huffman_encode_eos) {
  // encoded string ("!") and EOS
  bytes_t bytes{
      0x85, 0xfe, 0x3f, 0xff, 0xff, 0xff,
  };
  const uint8_t* in = bytes.data();
  hpack::decoded_string decoded;
  hpack::decode_string(in, in + bytes.size(), decoded);
  error_if(in != bytes.data() + bytes.size());
  error_if(decoded.str() != "!");
}

TEST(static_table_find) {
  using st = hpack::static_table_t;
  using namespace hpack;
#define STATIC_TABLE_ENTRY(cppname, header_name, ...)                       \
  {                                                                         \
    auto res = st::find(header_name, "" __VA_OPT__(__VA_ARGS__));           \
    error_if(res.header_name_index != (index_type)static_table_t::cppname); \
    error_if(res.value_indexed != (0 __VA_OPT__(+1)));                      \
  }
#include "hpack/static_table.def"
}

struct test_dyntab_t {
  std::deque<std::pair<std::string, std::string>> d;
  size_t cur_size;
  size_t max_size;

  test_dyntab_t(size_t max_sz) : cur_size(0), max_size(max_sz) {
  }
  void add_entry(std::string name, std::string value) {
    size_t entry_sz = name.size() + value.size() + 32;  // RFC
    while (!d.empty() && entry_sz > max_size - cur_size) {
      cur_size -= (d.back().first.size() + d.back().second.size() + 32);
      d.pop_back();
    }
    if (entry_sz > max_size)
      return;
    d.push_front({std::move(name), std::move(value)});
    cur_size += entry_sz;
  }
};

static int64_t rand_int(int64_t min, int64_t max, std::mt19937& gen) {
  return std::uniform_int_distribution(min, max)(gen);
}
static std::string generate_random_string(size_t length, std::mt19937& gen) {
  constexpr std::string_view characters = "abcdefghijklmnopqrstuvwxyz";
  std::string random_str;
  random_str.reserve(length);
  for (size_t i = 0; i < length; ++i)
    random_str += characters[rand_int(0, characters.size() - 1, gen)];
  return random_str;
}

TEST(dynamic_table_indexes) {
  enum { MAX_SZ = 512 };
  hpack::dynamic_table_t table(MAX_SZ);

  error_if(table.current_size() != 0);
  table.add_entry("name1", "hello world");
  table.add_entry("name2", "header2");
  table.add_entry(std::string(1000, 'a'), "");
  error_if(table.current_size() != 0);
  std::mt19937 gen(213214);

  test_dyntab_t test_table(MAX_SZ);

  for (int i = 0; i < 1000; ++i) {
    std::string random_name = generate_random_string(rand_int(1, 300, gen), gen);
    std::string random_value = generate_random_string(rand_int(0, 300, gen), gen);
    table.add_entry(random_name, random_value);
    auto r = table.find(random_name, random_value);
    if (random_name.size() + random_value.size() + 32 <= MAX_SZ) {
      // firstly inserted value always have first index
      error_if(r.header_name_index != 62);
      error_if(!r.value_indexed);
    } else {
      error_if(r.header_name_index != 0);
      error_if(r.value_indexed);
    }
    test_table.add_entry(random_name, random_value);
    error_if(table.current_size() != test_table.cur_size);
    // 62 is min index of HPACK dynamic table
    for (size_t j = 62; j < 62 + test_table.d.size(); ++j) {
      const auto& real_entry = table.get_entry(j);
      const auto& test_entry = test_table.d[j - 62];
      error_if(real_entry.name != test_entry.first);
      error_if(real_entry.value != test_entry.second);
    }
  }
}

TEST(tg_answer) {
  std::vector<hpack::byte_t> bytes = {
      0x88, 0x76, 0x89, 0xaa, 0x63, 0x55, 0xe5, 0x80, 0xae, 0x17, 0x97, 0x7,  0x61, 0x96, 0xc3, 0x61, 0xbe,
      0x94, 0x3,  0x8a, 0x6e, 0x2d, 0x6a, 0x8,  0x2,  0x69, 0x40, 0x3b, 0x70, 0xf,  0x5c, 0x13, 0x4a, 0x62,
      0xd1, 0xbf, 0x5f, 0x8b, 0x1d, 0x75, 0xd0, 0x62, 0xd,  0x26, 0x3d, 0x4c, 0x74, 0x41, 0xea, 0x5c, 0x4,
      0x31, 0x39, 0x32, 0x36, 0x0,  0x91, 0x42, 0x6c, 0x31, 0x12, 0xb2, 0x6c, 0x1d, 0x48, 0xac, 0xf6, 0x25,
      0x64, 0x14, 0x96, 0xd8, 0x64, 0xfa, 0xa0, 0xa4, 0x7e, 0x56, 0x1c, 0xc5, 0x81, 0x90, 0xb6, 0xcb, 0x80,
      0x0,  0x3e, 0xd4, 0x35, 0x44, 0xa2, 0xd9, 0xb,  0xba, 0xd8, 0xef, 0x9e, 0x91, 0x9a, 0xa4, 0x7d, 0xa9,
      0x5d, 0x85, 0xa0, 0xe3, 0x93, 0x0,  0x93, 0x19, 0x8,  0x54, 0x21, 0x62, 0x1e, 0xa4, 0xd8, 0x7a, 0x16,
      0x1d, 0x14, 0x1f, 0xc2, 0xc7, 0xb0, 0xd3, 0x1a, 0xaf, 0x1,  0x2a, 0x0,  0x94, 0x19, 0x8,  0x54, 0x21,
      0x62, 0x1e, 0xa4, 0xd8, 0x7a, 0x16, 0x1d, 0x14, 0x1f, 0xc2, 0xd4, 0x95, 0x33, 0x9e, 0x44, 0x7f, 0x90,
      0xc5, 0x83, 0x7f, 0xd2, 0x9a, 0xf5, 0x6e, 0xdf, 0xf4, 0xa6, 0xad, 0x7b, 0xf2, 0x6a, 0xd3, 0xbb, 0x0,
      0x94, 0x19, 0x8,  0x54, 0x21, 0x62, 0x1e, 0xa4, 0xd8, 0x7a, 0x16, 0x2f, 0x9a, 0xce, 0x82, 0xad, 0x39,
      0x47, 0x21, 0x6c, 0x47, 0xa5, 0xbc, 0x7a, 0x92, 0x5a, 0x92, 0xb6, 0x72, 0xd5, 0x32, 0x67, 0xfa, 0xbc,
      0x7a, 0x92, 0x5a, 0x92, 0xb6, 0xff, 0x55, 0x97, 0xea, 0xf8, 0xd2, 0x5f, 0xad, 0xc5, 0xb3, 0xb9, 0x6c,
      0xfa, 0xbc, 0x7a, 0xaa, 0x29, 0x12, 0x63, 0xd5,
  };
  hpack::decoder e;
  headers_t expected{
      {":status", "200"},
      {"server", "nginx/1.18.0"},
      {"date", "Fri, 06 Sep 2024 07:08:24 GMT"},
      {"content-type", "application/json"},
      {"content-length", "1926"},
      {"strict-transport-security", "max-age=31536000; includeSubDomains; preload"},
      {"access-control-allow-origin", "*"},
      {"access-control-allow-methods", "GET, POST, OPTIONS"},
      {"access-control-expose-headers", "Content-Length,Content-Type,Date,Server,Connection"},
  };
  headers_t result;
  const hpack::byte_t* in = bytes.data();
  error_if(200 != e.decode_response_status(in, bytes.data() + bytes.size()));
  hpack::decode_headers_block(e, bytes, [&](std::string_view name, std::string_view value) {
    result.emplace_back(std::string(name), std::string(value));
  });
  error_if(result != expected);
}

TEST(decode_status) {
  hpack::encoder e;
  hpack::decoder de;
  bytes_t rsp;

  e.encode_header_fully_indexed(hpack::static_table_t::status_304, std::back_inserter(rsp));
  const auto* in = rsp.data();
  error_if(304 != de.decode_response_status(in, rsp.data() + rsp.size()));
  error_if(in != rsp.data() + rsp.size());
  rsp.clear();

  // use status as name and valid status code
  e.encode_header_without_indexing(hpack::static_table_t::status_200, "200", std::back_inserter(rsp));
  in = rsp.data();
  error_if(200 != de.decode_response_status(in, rsp.data() + rsp.size()));
  error_if(in != rsp.data() + rsp.size());
  rsp.clear();

  e.encode_header_without_indexing(hpack::static_table_t::status_200, "fds", std::back_inserter(rsp));
  in = rsp.data();
  try {
    de.decode_response_status(in, rsp.data() + rsp.size());
    error_if(true);
  } catch (...) {
  }
  rsp.clear();

  e.encode_header_without_indexing(hpack::static_table_t::status_200, "2000", std::back_inserter(rsp));
  in = rsp.data();
  try {
    de.decode_response_status(in, rsp.data() + rsp.size());
    error_if(true);
  } catch (...) {
  }
  rsp.clear();

  e.encode_header_never_indexing(hpack::static_table_t::status_200, "2 0 0", std::back_inserter(rsp));
  in = rsp.data();
  try {
    de.decode_response_status(in, rsp.data() + rsp.size());
    error_if(true);
  } catch (...) {
  }
  rsp.clear();

  e.encode_header_and_cache(hpack::static_table_t::status_200, "555", std::back_inserter(rsp));
  in = rsp.data();
  error_if(555 != de.decode_response_status(in, rsp.data() + rsp.size()));
  error_if(in != rsp.data() + rsp.size());
  rsp.clear();
}

TEST(dynamic_table_size_update) {
  hpack::encoder e;
  hpack::decoder de;
  bytes_t bytes;

  e.encode_dynamic_table_size_update(144, std::back_inserter(bytes));
  const auto* in = bytes.data();
  const auto* end = bytes.data() + bytes.size();
  // decode dynamic table size (repeated because its in .cpp)
  auto decode_size_update = hpack::decode_integer(in, end, 5);
  error_if(144 != decode_size_update);
  error_if(in != end);  // all parsed
}

TEST(static_table_find_by_index) {
  using namespace hpack;
  {
    find_result_t res;
    res = static_table_t::find(0, "");
    error_if(res.value_indexed || res.header_name_index);
    res = static_table_t::find(static_table_t::first_unused_index, "abc");
    error_if(res.value_indexed || res.header_name_index);
  }
  std::string possible_values[]{
#define STATIC_TABLE_ENTRY(cppname, header_name, ...) __VA_OPT__(__VA_ARGS__, )
#include "hpack/static_table.def"
  };
  std::string impossible_values[]{
      "",
      "fdsgwrg",
      "hello world",
  };
  auto test_index = [&](index_type i) {
    for (std::string_view val : possible_values) {
      auto myentry = static_table_t::get_entry(i);
      find_result_t res = static_table_t::find(i, val);
      find_result_t res2 = static_table_t::find(myentry.name, val);
      error_if(res.value_indexed != res2.value_indexed);
      error_if(static_table_t::get_entry(res.header_name_index).name != myentry.name);
      error_if(static_table_t::get_entry(res.header_name_index).name !=
               static_table_t::get_entry(res2.header_name_index).name);
      if (val == myentry.value)
        error_if(res.header_name_index != i);
    }
    for (std::string_view val : impossible_values) {
      auto res = static_table_t::find(i, val);
      error_if(res.value_indexed);
      error_if(res.header_name_index != i);
    }
  };
  for (index_type i = 1; i < static_table_t::first_unused_index; ++i)
    test_index(i);
  {
    auto res1 = static_table_t::find(static_table_t::path, "/");
    auto res2 = static_table_t::find(static_table_t::path_index_html, "/");
    error_if(res1.value_indexed != res2.value_indexed);
    error_if(res1.header_name_index != res2.header_name_index);
    error_if(res1.header_name_index != static_table_t::path);
  }
}

TEST(decoded_string) {
  // empry str

  hpack::decoded_string str;
  error_if(str);
  error_if(str.bytes_allocated());
  str = std::move(str);
  error_if(str);
  error_if(str.bytes_allocated());
  error_if(str.str() != "");

  // non huffman

  std::string_view test = "hello";
  str = test;
  error_if(str.bytes_allocated());
  error_if(str.str() != test);
  str.reset();
  error_if(str);
  error_if(str.str() != "");
  error_if(str.bytes_allocated());

  // huffman

  bytes_t out;
  hpack::encode_string_huffman(test, std::back_inserter(out));
  const auto* in = out.data();
  hpack::decode_string(in, in + out.size(), str);
  error_if(!str);
  error_if(str.str() != test);
  error_if(str.bytes_allocated() != std::bit_ceil(test.size()));

  // memory reuse

  std::string_view before = str.str();
  in = out.data();
  hpack::decode_string(in, in + out.size(), str);
  error_if(!str);
  error_if(str.str() != test);
  error_if(str.bytes_allocated() != std::bit_ceil(test.size()));
  error_if(before.data() != str.str().data());

  // memory reuse for smaller string

  std::string_view test2 = "ab";
  bytes_t out2;
  hpack::encode_string_huffman(test2, std::back_inserter(out2));
  in = out2.data();

  hpack::decode_string(in, in + out2.size(), str);
  error_if(!str);
  error_if(str != test2);
  error_if(before.data() != str.str().data());
  error_if(str.bytes_allocated() != std::bit_ceil(test.size()));

  // reallocate after bigger string

  std::string_view test3 = "hello world big string";
  bytes_t out3;
  hpack::encode_string_huffman(test3, std::back_inserter(out3));
  in = out3.data();
  hpack::decode_string(in, in + out3.size(), str);

  error_if(!str);
  error_if(str != test3);
  error_if(str.bytes_allocated() != std::bit_ceil(test3.size()));

  // zero-len str huffman

  bytes_t out_empty;
  hpack::encode_string_huffman("", std::back_inserter(out_empty));
  in = out_empty.data();
  hpack::decode_string(in, in + out_empty.size(), str);
  error_if(str);
  error_if(str.bytes_allocated() != std::bit_ceil(test3.size()));
  error_if(str != "");

  // reseting

  str.reset();

  error_if(str);
  error_if(str.bytes_allocated());
  error_if(str != "");

  str.reset();

  error_if(str != str);
}

TEST(dyntab2) {
  const unsigned testarr[] = {
      72,  130, 16,  3,   95,  139, 29,  117, 208, 98,  13,  38,  61,  76,  116, 65,  234, 15,  31,  187, 157,
      41,  174, 227, 12,  127, 238, 229, 192, 255, 242, 227, 207, 0,   12,  85,  85,  146, 173, 84,  180, 177,
      220, 44,  85,  42,  198, 169, 9,   29,  68,  42,  24,  100, 46,  20,  49,  178, 250, 192, 126, 89,  86,
      104, 82,  58,  179, 210, 17,  245, 153, 121, 247, 7,   32,  72,  219, 206, 63,  162, 64,  140, 102, 106,
      235, 89,  17,  153, 104, 205, 84,  134, 170, 111, 175, 142, 136, 42,  149, 100, 21,  63,  106, 85,  42,
      10,  152, 16,  84,  133, 122, 172, 149, 5,   74,  237, 204, 69,  233, 168, 128, 108, 11,  210, 66,  9,
      176, 125, 168, 130, 217, 222, 161, 210, 88,  42,  170, 201, 86,  170, 90,  127, 15,  13,  130, 11,  130,
  };
  hpack::decoder d(4096);
  error_if(d.dyntab.current_max_index() != hpack::static_table_t::first_unused_index - 1);
  d.dyntab.add_entry(":status", "201");
  error_if(d.dyntab.current_max_index() != hpack::static_table_t::first_unused_index);
  hpack::table_entry e;
  e.name = ":status";
  e.value = "201";
  error_if(d.dyntab.get_entry(d.dyntab.current_max_index()) != e);
  d.dyntab.add_entry("content-type", "application/json");
  error_if(d.dyntab.get_entry(d.dyntab.current_max_index()) != e);
  e.name = "content-type";
  e.value = "application/json";
  error_if(d.dyntab.get_entry(d.dyntab.current_max_index() - 1) != e);
  bytes_t bytes;
  for (unsigned i : testarr) {
    bytes.push_back(hpack::byte_t(i));
  }
  headers_t expected = {
      {":status", "201"},
      {"content-type", "application/json"},
      {"location", "http://[::1]:8800/nnrf-nfm/v1/nf-instances/316e1b39-09ff-42d7-8dc9-3896ad1c5869"},
      {"etag", ""},
      {"3gpp-sbi-binding", "bl=nf-set; nfset=set1.nrfset.5gc.mnc050.mcc250; servname=nnrf-nfm"},
      {"content-length", "162"},
  };
  headers_t decoded;
  hpack::decode_headers_block(d, bytes, [&](std::string_view name, std::string_view value) {
    decoded.push_back({std::string(name), std::string(value)});
  });
  error_if(expected != decoded);
}

int main() {
  test_decoded_string();
  test_tg_answer();
  test_encode_decode_with_eviction();
  test_encode_decode_huffman1();
  test_encode_decode_integers();
  test_encode_decode1();
  test_huffman_table_itself();
  test_huffman();
  test_huffman_rand();
  test_huffman_encode_eos();
  test_static_table_find();
  test_dynamic_table_indexes();
  test_decode_status();
  test_dynamic_table_size_update();
  test_static_table_find_by_index();
  test_dyntab2();
}
