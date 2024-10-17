
Complete implementation of HPACK (Header Compression for HTTP/2, fully compliant RFC 7541)

encode:

```cpp
#include <hpack/hpack.hpp>

void encode_my_headers(hpack::encoder& enc, std::vector<hpack::bytes>& bytes;) {
  // memory effective by default
  enc.encode("name", "value", std::back_inserter(bytes));
  // or by hands
  enc.encode_header_fully_indexed(hpack::static_table_t::status_200, std::back_inserter(bytes));
}

```

decode

```cpp
#include <hpack/hpack.hpp>

void decode_my_headers(hpack::decoder& d, std::span<const hpack::byte_t> bytes) {
  hpack::decode_headers_block(e, bytes, [&](std::string_view name, std::string_view value) {
    // use name/value somehow
  });
}

```

adding with cmake:

Preferred way with [CPM](https://github.com/cpm-cmake/CPM.cmake)

```cmake

CPMAddPackage(
  NAME HPACK
  GIT_REPOSITORY https://github.com/kelbon/HPACK
  GIT_TAG        v1.0.0
  OPTIONS "HPACK_ENABLE_TESTING ON"
)

target_link_libraries(MyTargetName hpacklib)

```

simple way with fetch content:

```cmake

include(FetchContent)
FetchContent_Declare(
  HPACK
  GIT_REPOSITORY https://github.com/kelbon/HPACK
  GIT_TAG        origin/master
)
FetchContent_MakeAvailable(HPACK)
target_link_libraries(MyTargetName hpacklib)

```
