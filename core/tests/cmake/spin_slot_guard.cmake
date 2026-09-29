# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The #1618 guard check, run by ctest in script mode (`cmake -P`) — the LKV slot's twin of
# spin_pool_guard.cmake, and for the same reason: what is under test is a build REJECTION, which
# no compiled test binary can observe. It drives the compiler over one probe TU
# (spin_slot_guard_probe.cpp) under four configurations:
#
#   DEFAULT   (no fragment: the host default)          -> must compile.
#   ALLOWED   (kSpinWaitSafe = false, single-writer)   -> must compile. The configuration the
#                                                         guard steers an RTOS build towards.
#   SPINNING  (kSpinWaitSafe = false, may_spin = true) -> must FAIL on the spin assertion.
#   UNMARKED  (a policy that declares no may_spin)     -> must FAIL on the declaration assertion.
#
# A forbidden arm that fails for any other reason (a typo, a missing header) is reported as a
# failure rather than accepted as a pass — the difference between this and a bare WILL_FAIL.
#
# Required -D arguments: LT_CXX, LT_CORE_INCLUDE, LT_FIXTURES, LT_PROBE.

foreach(_arg LT_CXX LT_CORE_INCLUDE LT_FIXTURES LT_PROBE)
    if(NOT DEFINED ${_arg})
        message(FATAL_ERROR "spin_slot_guard.cmake: -D${_arg}=... is required")
    endif()
endforeach()

function(_lt_compile out_rc out_log)
    execute_process(
        COMMAND "${LT_CXX}" -std=c++23 -fsyntax-only ${ARGN} "-I${LT_CORE_INCLUDE}" "${LT_PROBE}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    set(${out_rc} ${_rc} PARENT_SCOPE)
    set(${out_log} "${_out}${_err}" PARENT_SCOPE)
endfunction()

_lt_compile(_rc _log)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "DEFAULT arm did not compile — the host default slot must satisfy the "
                        "slot assertions.\n${_log}")
endif()

_lt_compile(_rc _log "-I${LT_FIXTURES}/allowed")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "ALLOWED arm (kSpinWaitSafe = false, single_writer_slot_t) did not "
                        "compile — the RTOS configuration must stay buildable.\n${_log}")
endif()

# Each forbidden arm must be refused by ITS OWN assertion, matched on that assertion's wording.
set(_want_spinning "declares may_spin = true")
set(_want_unmarked "does not declare")
foreach(_arm spinning unmarked)
    _lt_compile(_rc _log "-I${LT_FIXTURES}/${_arm}")
    if(_rc EQUAL 0)
        message(FATAL_ERROR "${_arm} arm COMPILED. The slot assertions in vertex.hpp are inert: "
                            "a spin-refusing build can bind a slot that spins, or one that never "
                            "says whether it does.")
    endif()
    if(NOT "${_log}" MATCHES "${_want_${_arm}}")
        message(FATAL_ERROR "${_arm} arm failed, but NOT on its slot assertion — its diagnostic "
                            "never says \"${_want_${_arm}}\", so this check is passing for the "
                            "wrong reason.\n${_log}")
    endif()
endforeach()

message(STATUS "spin_slot_guard: OK — default and allowed arms compile, spinning and unmarked "
               "arms are rejected by name")
