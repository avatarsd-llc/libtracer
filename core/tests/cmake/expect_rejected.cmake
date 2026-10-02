# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# One harness for every "this spelling is gone" tripwire (#1703, #1722), run by ctest in script
# mode (`cmake -P`): what is under test is a build REJECTION, which no compiled test binary can
# observe. Each test passes two commands, as `|`-separated argument lists (a `;` would be split
# by add_test):
#
#   LT_ACCEPT_CMD (optional) -> must SUCCEED: the current spelling, or the default, still builds.
#   LT_REJECT_CMD            -> must FAIL, and its output must match LT_WANT (a regex) — the
#                               tripwire's own message. A failure for any other reason (a typo,
#                               a missing header) is reported as a failure, not accepted.
#
# Required -D arguments: LT_NAME, LT_REJECT_CMD, LT_WANT. Optional: LT_ACCEPT_CMD.

foreach(_arg LT_NAME LT_REJECT_CMD LT_WANT)
    if(NOT DEFINED ${_arg})
        message(FATAL_ERROR "expect_rejected.cmake: -D${_arg}=... is required")
    endif()
endforeach()

function(_lt_run cmd out_rc out_log)
    string(REPLACE "|" ";" _argv "${cmd}")
    execute_process(COMMAND ${_argv} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(${out_rc} ${_rc} PARENT_SCOPE)
    set(${out_log} "${_out}${_err}" PARENT_SCOPE)
endfunction()

if(DEFINED LT_ACCEPT_CMD)
    _lt_run("${LT_ACCEPT_CMD}" _rc _log)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${LT_NAME}: the ACCEPT arm failed — the current spelling must "
                            "still build.\n${_log}")
    endif()
endif()

_lt_run("${LT_REJECT_CMD}" _rc _log)
if(_rc EQUAL 0)
    message(FATAL_ERROR "${LT_NAME}: the REJECT arm SUCCEEDED. The tripwire is inert: a build "
                        "that still uses the old spelling would go on without a word.")
endif()
if(NOT "${_log}" MATCHES "${LT_WANT}")
    message(FATAL_ERROR "${LT_NAME}: the REJECT arm failed, but NOT on the tripwire — its "
                        "output never matches \"${LT_WANT}\", so this check would pass for the "
                        "wrong reason.\n${_log}")
endif()

message(STATUS "${LT_NAME}: OK — refused by the tripwire's own message")
