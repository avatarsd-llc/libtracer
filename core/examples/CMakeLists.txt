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

# Run the example as a smoke test when testing is enabled (it exits 0).
if(BUILD_TESTING)
    add_test(NAME example_in_process_pubsub COMMAND in_process_pubsub)
endif()
