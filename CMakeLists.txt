cmake_minimum_required(VERSION 3.05)
project(HPACK LANGUAGES CXX)

### options ###

option(HPACK_ENABLE_TESTING "enables testing" OFF)
option(HPACK_USE_CPM "turn off to not download boost" ON)

### dependecies ###

if(HPACK_USE_CPM)

include(cmake/get_cpm.cmake)

set(BOOST_INCLUDE_LIBRARIES intrusive)
CPMAddPackage(
  NAME Boost
  VERSION 1.84.0
  URL https://github.com/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.tar.xz
  OPTIONS "BOOST_ENABLE_CMAKE ON"
)
unset(BOOST_INCLUDE_LIBRARIES)
find_package(Boost 1.84 COMPONENTS intrusive REQUIRED)

endif()

### hpacklib ###

add_library(hpacklib STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/decoder.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/dynamic_table.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/huffman.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/static_table.cpp")

target_include_directories(hpacklib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(hpacklib PUBLIC Boost::intrusive)

set_target_properties(hpacklib PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CMAKE_CXX_STANDARD_REQUIRED ON
	CXX_STANDARD 20)

if(HPACK_ENABLE_TESTING)
	include(CTest)
	add_subdirectory(tests)
endif()
