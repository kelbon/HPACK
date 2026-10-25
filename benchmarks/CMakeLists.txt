cmake_minimum_required(VERSION 3.05)

add_executable(bench_hpack ${CMAKE_CURRENT_SOURCE_DIR}/bench_hpack.cpp)

target_link_libraries(bench_hpack PUBLIC hpacklib)

set_target_properties(bench_hpack PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CXX_STANDARD 20
	CMAKE_CXX_STANDARD_REQUIRED ON
)
