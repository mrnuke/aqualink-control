cmake_minimum_required(VERSION 3.14)

PROJECT(aqualink-control C)
ADD_DEFINITIONS(-O1 -Wall -Werror --std=gnu99 -g3)

INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/..)
# LINK_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/..)
option(LOCAL_LIBUBOX "build a local copy of libubox" ON)

if(LOCAL_LIBUBOX)
	set(BUILD_LUA False)
	set(BUILD_EXAMPLES False)
	add_subdirectory(libubox)
else()
	find_path(ubox_include_dir NAMES libubox/ulog.h)
	include_directories(${ubox_include_dir})
endif()


add_executable(aquamasterd main.c)
target_link_libraries(aquamasterd ubox-static)
