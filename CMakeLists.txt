cmake_minimum_required(VERSION 3.14)

PROJECT(aqualink-control C)
ADD_DEFINITIONS(-O1 -Wall -Werror --std=gnu99 -g3)

include_directories(${CMAKE_SOURCE_DIR})

option(LOCAL_LIBUBOX "build a local copy of libubox" ON)

if(LOCAL_LIBUBOX)
	set(BUILD_LUA False)
	set(BUILD_EXAMPLES False)
	add_subdirectory(libubox)
else()
	find_path(ubox_include_dir NAMES libubox/ulog.h)
	include_directories(${ubox_include_dir})
endif()


add_executable(aquamasterd
	src/aqualink_frame.c
	src/main.c
	src/jxi_heater.c
)
target_link_libraries(aquamasterd ubox-static)

# Current tests are small, so they can be left enabled.
enable_testing()
add_executable(test-protocol tests/test-protocol.c src/aqualink_frame.c)
target_include_directories(test-protocol PRIVATE src)
add_test(NAME test-protocol COMMAND test-protocol)
