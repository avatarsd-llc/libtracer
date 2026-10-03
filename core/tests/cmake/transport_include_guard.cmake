# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The #1720 include-graph guard, run by ctest in script mode (`cmake -P`).
#
# A transport header only has to NAME a factory type and the settings record it receives, so it
# includes the `transport_factory.hpp` leaf and never parses `graph.hpp` or `vertex.hpp`. What is
# under test is the preprocessor's include closure, which no compiled test binary can observe,
# so this asks the compiler for each header's dependency list (`-M`) and fails if either graph
# header is in it.
#
#   LEAN      (the transport headers)  -> the closure must NOT name graph.hpp or vertex.hpp.
#   CONTROL   (transport_vertex.hpp)   -> the closure MUST name graph.hpp. Without this arm the
#                                         check would pass just as happily if the dependency
#                                         listing stopped reporting libtracer headers at all.
#
# Required -D arguments: LT_CXX, LT_CORE_INCLUDE, LT_WORK_DIR.

foreach(_arg LT_CXX LT_CORE_INCLUDE LT_WORK_DIR)
    if(NOT DEFINED ${_arg})
        message(FATAL_ERROR "transport_include_guard.cmake: -D${_arg}=... is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${LT_WORK_DIR}")

# Writes a TU that includes only libtracer/<header>.hpp and returns its dependency list.
function(lt_include_closure header out_var)
    set(_probe "${LT_WORK_DIR}/${header}_probe.cpp")
    file(WRITE "${_probe}" "#include \"libtracer/${header}.hpp\"\n")
    execute_process(
        COMMAND "${LT_CXX}" -std=c++23 -M "-I${LT_CORE_INCLUDE}" "${_probe}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "libtracer/${header}.hpp does not preprocess on its own:\n${_err}")
    endif()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

set(_lean_headers transport_factory transport_can transport_quic transport_webtransport
                  self_heal_link)
foreach(_h IN LISTS _lean_headers)
    lt_include_closure(${_h} _deps)
    foreach(_heavy graph vertex)
        if(_deps MATCHES "libtracer/${_heavy}\\.hpp")
            message(FATAL_ERROR
                    "libtracer/${_h}.hpp pulls in libtracer/${_heavy}.hpp (#1720). A transport "
                    "header names its factory through transport_factory.hpp, not the graph.")
        endif()
    endforeach()
endforeach()

lt_include_closure(transport_vertex _control)
if(NOT _control MATCHES "libtracer/graph\\.hpp")
    message(FATAL_ERROR
            "CONTROL arm: transport_vertex.hpp's dependency list does not name graph.hpp, so the "
            "listing is not reporting libtracer headers and the guard above proves nothing.")
endif()
