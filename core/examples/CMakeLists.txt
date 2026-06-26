# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

add_executable(in_process_pubsub in_process_pubsub.cpp)
target_link_libraries(in_process_pubsub PRIVATE libtracer)
find_package(Threads REQUIRED)
target_link_libraries(in_process_pubsub PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(in_process_pubsub PRIVATE /W4)
else()
    target_compile_options(in_process_pubsub PRIVATE -Wall -Wextra)
endif()

add_executable(two_node_loopback two_node_loopback.cpp)
target_link_libraries(two_node_loopback PRIVATE libtracer Threads::Threads)
if(MSVC)
    target_compile_options(two_node_loopback PRIVATE /W4)
else()
    target_compile_options(two_node_loopback PRIVATE -Wall -Wextra)
endif()

# The same two-node program as two_node_loopback, over a real localhost UDP socket.
add_executable(udp_two_node udp_two_node.cpp)
target_link_libraries(udp_two_node PRIVATE libtracer Threads::Threads)
if(MSVC)
    target_compile_options(udp_two_node PRIVATE /W4)
else()
    target_compile_options(udp_two_node PRIVATE -Wall -Wextra)
endif()

# Run the examples as smoke tests when testing is enabled (they exit 0).
if(BUILD_TESTING)
    add_test(NAME example_in_process_pubsub COMMAND in_process_pubsub)
    add_test(NAME example_two_node_loopback COMMAND two_node_loopback)
    add_test(NAME example_udp_two_node COMMAND udp_two_node)
endif()
