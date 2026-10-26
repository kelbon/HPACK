// Simple benchmarks without dependencies, build in Release:
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DHPACK_ENABLE_BENCHMARKS=ON && cmake --build build
//   ./build/benchmarks/bench_hpack

#include <hpack/hpack.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using bytes_t = std::vector<hpack::byte_t>;
using hpack::byte_t;

// prevents compiler from removing benchmarked code
size_t sink = 0;

// runs 'f' 'iterations' times, prints time per 'units_per_iteration' units
template <typename F>
void bench(const char* name, int iterations, double units_per_iteration, const char* unit, F f) {
  f();  // warm up
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i)
    f();
  auto end = std::chrono::steady_clock::now();
  double ns = std::chrono::duration<double, std::nano>(end - start).count();
  std::printf("%-40s %8.2f ns/%s\n", name, ns / (iterations * units_per_iteration), unit);
}

struct header {
  std::string name;
  std::string value;
};

// typical browser request
std::vector<header> request_headers(int n) {
  return {
      {":method", "GET"},
      {":scheme", "https"},
      {":authority", "api.example.com"},
      {":path",
       "/api/v1/users/" + std::to_string(12345 + n) + "/orders?limit=50&offset=" + std::to_string(n * 50)},
      {"user-agent",
       "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 "
       "Safari/537.36"},
      {"accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8"},
      {"accept-encoding", "gzip, deflate, br, zstd"},
      {"accept-language", "en-US,en;q=0.9"},
      {"cookie", "session=4f2a9c7e1b3d5f6a8c0e2b4d6f8a0c2e; theme=dark"},
      {"x-request-id", "b7e3c1a9-" + std::to_string(1000000 + n)},
  };
}

// typical response
std::vector<header> response_headers(int n) {
  return {
      {"content-type", "application/json"},
      {"content-length", std::to_string(1000 + n % 5000)},
      {"date", "Sun, 25 Oct 2026 18:" + std::to_string(10 + n % 50) + ":00 GMT"},
      {"server", "hidi"},
      {"cache-control", "no-cache"},
      {"x-request-id", "b7e3c1a9-" + std::to_string(1000000 + n)},
  };
}

constexpr int blocks_count = 100;

// header blocks of consecutive requests, encoded like browsers do (Huffman + incremental indexing)
std::vector<bytes_t> encoded_request_blocks() {
  hpack::encoder enc;
  std::vector<bytes_t> blocks;
  for (int i = 0; i < blocks_count; ++i) {
    bytes_t& block = blocks.emplace_back();
    for (auto& h : request_headers(i))
      enc.encode</*Cache=*/true, /*Huffman=*/true>(h.name, h.value, std::back_inserter(block));
  }
  return blocks;
}

void bench_huffman() {
  std::vector<header> hdrs = request_headers(0);
  std::vector<bytes_t> encoded;
  size_t total = 0;
  for (auto& h : hdrs) {
    hpack::encode_string<true>(h.value, std::back_inserter(encoded.emplace_back()));
    total += h.value.size();
  }
  hpack::decoded_string out;
  bench("huffman decode", 100000, total, "byte", [&] {
    for (bytes_t& e : encoded) {
      const byte_t* in = e.data();
      hpack::decode_string(in, e.data() + e.size(), out);
      sink += out.str().size();
    }
  });
  bytes_t buf;
  bench("huffman encode", 100000, total, "byte", [&] {
    buf.clear();
    for (auto& h : hdrs)
      hpack::encode_string<true>(h.value, std::back_inserter(buf));
    sink += buf.size();
  });
}

void bench_decode_blocks() {
  std::vector<bytes_t> blocks = encoded_request_blocks();
  size_t headers_count = blocks_count * request_headers(0).size();
  hpack::header_view h;
  bench("decode request blocks (huffman, cached)", 1000, headers_count, "header", [&] {
    hpack::decoder dec;
    for (bytes_t& b : blocks) {
      const byte_t* in = b.data();
      const byte_t* e = b.data() + b.size();
      while (in != e) {
        dec.decode_header(in, e, h);
        sink += h.value.str().size();
      }
    }
  });
}

void bench_encode_blocks() {
  std::vector<std::vector<header>> responses;
  for (int i = 0; i < blocks_count; ++i)
    responses.push_back(response_headers(i));
  size_t headers_count = blocks_count * responses[0].size();
  bytes_t buf;
  bench("encode response blocks (encode_auto)", 1000, headers_count, "header", [&] {
    hpack::encoder enc;
    for (auto& hdrs : responses) {
      buf.clear();
      for (auto& h : hdrs)
        enc.encode_auto(h.name, h.value, std::back_inserter(buf));
      sink += buf.size();
    }
  });
  bench("encode response blocks (encode_with_cache)", 1000, headers_count, "header", [&] {
    hpack::encoder enc;
    for (auto& hdrs : responses) {
      buf.clear();
      for (auto& h : hdrs)
        enc.encode_with_cache(h.name, h.value, std::back_inserter(buf));
      sink += buf.size();
    }
  });
}

void bench_dynamic_table() {
  std::vector<header> entries;
  for (int i = 0; i < 1000; ++i)
    for (auto& h : request_headers(i))
      entries.push_back(h);
  hpack::dynamic_table_t table(4096);
  for (auto& h : entries)  // fill table
    table.add_entry(h.name, h.value);
  bench("dynamic table add_entry (full table)", 100, entries.size(), "op", [&] {
    for (auto& h : entries)
      sink += (size_t)table.add_entry(h.name, h.value);
  });
  bench("dynamic table find", 100, entries.size(), "op", [&] {
    for (auto& h : entries)
      sink += table.find(h.name, h.value).header_name_index;
  });
}

}  // namespace

int main() {
  bench_huffman();
  bench_decode_blocks();
  bench_encode_blocks();
  bench_dynamic_table();
  std::printf("(%zu)\n", sink);
}
