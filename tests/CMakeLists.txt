cmake_minimum_required(VERSION 3.05)

add_executable(test_hpack ${CMAKE_CURRENT_SOURCE_DIR}/test_hpack.cpp)

target_link_libraries(test_hpack PUBLIC hpacklib)

add_test(NAME test_hpack COMMAND test_hpack)

set_target_properties(test_hpack PROPERTIES
	CMAKE_CXX_EXTENSIONS OFF
	LINKER_LANGUAGE CXX
	CXX_STANDARD 20
	CMAKE_CXX_STANDARD_REQUIRED ON
)
