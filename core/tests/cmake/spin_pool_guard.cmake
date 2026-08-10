# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The #1158 guard check, run by ctest in script mode (`cmake -P`).
#
# What is under test is a build REJECTION, which no compiled test binary can observe: by the
# time a test runs, the translation unit it would have to reject has already compiled. So this
# drives the compiler itself, twice, over one probe TU (spin_pool_guard_probe.cpp) and two
# renderings of libtracer/config.hpp:
#
#   ALLOWED   (kSpinWaitSafe = true)  -> must compile. This is the "a correct build is
#                                        unaffected" half; without it the check would pass
#                                        just as happily if mem_pool.hpp stopped compiling.
#   FORBIDDEN (kSpinWaitSafe = false) -> must FAIL, *and* the diagnostic must name
#                                        critical_pool_t. Failing for any other reason (a
#                                        typo, a missing header, a stale include path) is
#                                        reported as a failure, not silently accepted as a
#                                        pass — that is the difference between this and a
#                                        bare WILL_FAIL, which any broken build satisfies.
#
# -fsyntax-only: the guard fires in the front end, so there is nothing to gain from codegen
# or a link step, and skipping them keeps the check well under a second.
#
# Required -D arguments: LT_CXX, LT_CORE_INCLUDE, LT_FORBIDDEN_INCLUDE, LT_PROBE.

foreach(_arg LT_CXX LT_CORE_INCLUDE LT_FORBIDDEN_INCLUDE LT_PROBE)
    if(NOT DEFINED ${_arg})
        message(FATAL_ERROR "spin_pool_guard.cmake: -D${_arg}=... is required")
    endif()
endforeach()

execute_process(
    COMMAND "${LT_CXX}" -std=c++23 -fsyntax-only "-I${LT_CORE_INCLUDE}" "${LT_PROBE}"
    RESULT_VARIABLE _allowed_rc
    OUTPUT_VARIABLE _allowed_out
    ERROR_VARIABLE _allowed_err)
if(NOT _allowed_rc EQUAL 0)
    message(FATAL_ERROR
            "ALLOWED arm (kSpinWaitSafe = true) did not compile — sync_pool_t must stay usable "
            "on a target where spin-waiting is safe.\n${_allowed_out}${_allowed_err}")
endif()

execute_process(
    COMMAND "${LT_CXX}" -std=c++23 -fsyntax-only "-I${LT_FORBIDDEN_INCLUDE}"
            "-I${LT_CORE_INCLUDE}" "${LT_PROBE}"
    RESULT_VARIABLE _forbidden_rc
    OUTPUT_VARIABLE _forbidden_out
    ERROR_VARIABLE _forbidden_err)
if(_forbidden_rc EQUAL 0)
    message(FATAL_ERROR
            "FORBIDDEN arm (kSpinWaitSafe = false) COMPILED. The guard in mem_pool.hpp is "
            "inert: a chip build can still instantiate synchronized_pool_t<spin_sync_t>.")
endif()
if(NOT "${_forbidden_out}${_forbidden_err}" MATCHES "critical_pool_t")
    message(FATAL_ERROR
            "FORBIDDEN arm failed, but NOT on the guard — its diagnostic never names "
            "critical_pool_t, so this check is passing for the wrong "
            "reason.\n${_forbidden_out}${_forbidden_err}")
endif()

message(STATUS "spin_pool_guard: OK — allowed arm compiles, forbidden arm is rejected by name")
