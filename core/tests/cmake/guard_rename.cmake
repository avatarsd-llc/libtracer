# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The #1703 rename tripwire, run by ctest in script mode (`cmake -P`), modelled on
# spin_slot_guard.cmake: what is under test is a build REJECTION, which no compiled test binary
# can observe. It drives the compiler over one probe TU under two configurations:
#
#   DEFAULT (no fragment: the host default)              -> must compile.
#   STALE   (a fragment that still binds reader_guard_t) -> must FAIL on the rename tripwire, with
#                                                           a message that names guard_t.
#
# The renamed spelling (`using guard_t = ...;`) is the spin_slot_guard ALLOWED arm, which must
# compile; it is not repeated here. A STALE arm that fails for any other reason (a typo, a
# missing header) is reported as a failure rather than accepted as a pass.
#
# Required -D arguments: LT_CXX, LT_CORE_INCLUDE, LT_FIXTURES, LT_PROBE.

foreach(_arg LT_CXX LT_CORE_INCLUDE LT_FIXTURES LT_PROBE)
    if(NOT DEFINED ${_arg})
        message(FATAL_ERROR "guard_rename.cmake: -D${_arg}=... is required")
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
    message(FATAL_ERROR "DEFAULT arm did not compile — the host default must not trip the "
                        "rename tripwire.\n${_log}")
endif()

_lt_compile(_rc _log "-I${LT_FIXTURES}/stale")
if(_rc EQUAL 0)
    message(FATAL_ERROR "STALE arm COMPILED. The rename tripwire in config.hpp is inert: a "
                        "fragment that binds reader_guard_t would silently keep the host "
                        "mutex_guard_t.")
endif()
set(_want "renamed to guard_t")
if(NOT "${_log}" MATCHES "${_want}")
    message(FATAL_ERROR "STALE arm failed, but NOT on the rename tripwire — its diagnostic never "
                        "says \"${_want}\", so this check is passing for the wrong reason.\n"
                        "${_log}")
endif()

message(STATUS "guard_rename: OK — the default compiles, a stale reader_guard_t fragment is "
               "refused by name")
