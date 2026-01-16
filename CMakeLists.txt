cmake_minimum_required(VERSION 3.14)

PROJECT(aqualink-control C)
ADD_DEFINITIONS(-O1 -Wall -Werror --std=gnu99 -g3)

# Current tests are small, so they can be left enabled.
enable_testing()
add_executable(test-protocol tests/test-protocol.c src/aqualink_frame.c)
target_include_directories(test-protocol PRIVATE src)
add_test(NAME test-protocol COMMAND test-protocol)
