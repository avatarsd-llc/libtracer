# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The #375-deliverable-3 CAN guard, run by ctest in script mode (`cmake -P`).
#
# `transport_can.cpp` carries a `static_assert(kBusLinks, ...)`: a CAN link is peer-named by
# construction (ADR-0030), so a build that closed the ADR-0044 bus module out and then compiles
# the CAN module is a contradiction — the transport would come up and none of its peers would be
# addressable. What is under test is therefore a build REJECTION, which no compiled test binary
# can observe, so this drives the compiler itself over the REAL translation unit (not a proxy
# probe) under two renderings of libtracer/config.hpp:
#
#   ALLOWED   (kBusLinks = true)  -> must compile. Without this arm the check would pass just as
#                                    happily if transport_can.cpp stopped compiling for any
#                                    unrelated reason.
#   FORBIDDEN (kBusLinks = false) -> must FAIL, *and* the diagnostic must name `kBusLinks`.
#                                    Failing for some other reason is reported as a failure,
#                                    not accepted as a pass — the difference between this and a
#                                    bare WILL_FAIL, which any broken build satisfies.
#
# -fsyntax-only: the assert fires in the front end, so codegen and linking would buy nothing.
#
# Required -D arguments: LT_CXX, LT_CORE_INCLUDE, LT_BUS_CLOSED_INCLUDE, LT_PROBE.

foreach(_arg LT_CXX LT_CORE_INCLUDE LT_BUS_CLOSED_INCLUDE LT_PROBE)
    if(NOT DEFINED ${_arg})
        message(FATAL_ERROR "bus_can_guard.cmake: -D${_arg}=... is required")
    endif()
endforeach()

execute_process(
    COMMAND "${LT_CXX}" -std=c++23 -fsyntax-only "-I${LT_CORE_INCLUDE}" "${LT_PROBE}"
    RESULT_VARIABLE _allowed_rc
    OUTPUT_VARIABLE _allowed_out
    ERROR_VARIABLE _allowed_err)
if(NOT _allowed_rc EQUAL 0)
    message(FATAL_ERROR
            "ALLOWED arm (kBusLinks = true) did not compile — the CAN module must stay buildable "
            "on a target that carries the bus module.\n${_allowed_out}${_allowed_err}")
endif()

execute_process(
    COMMAND "${LT_CXX}" -std=c++23 -fsyntax-only "-I${LT_BUS_CLOSED_INCLUDE}"
            "-I${LT_CORE_INCLUDE}" "${LT_PROBE}"
    RESULT_VARIABLE _forbidden_rc
    OUTPUT_VARIABLE _forbidden_out
    ERROR_VARIABLE _forbidden_err)
if(_forbidden_rc EQUAL 0)
    message(FATAL_ERROR
            "FORBIDDEN arm (kBusLinks = false) COMPILED. The guard in transport_can.cpp is "
            "inert: a bus-less build can still link a CAN transport whose peers nothing can "
            "address.")
endif()
if(NOT "${_forbidden_out}${_forbidden_err}" MATCHES "kBusLinks")
    message(FATAL_ERROR
            "FORBIDDEN arm failed, but NOT on the guard — its diagnostic never names "
            "kBusLinks, so this check is passing for the wrong "
            "reason.\n${_forbidden_out}${_forbidden_err}")
endif()

message(STATUS "bus_can_guard: OK — allowed arm compiles, forbidden arm is rejected by name")
