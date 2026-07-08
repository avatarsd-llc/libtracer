/**
 * @file
 * @brief POSIX `<poll.h>` compatibility shim for ESP-IDF chip targets.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * IDF's libc ships the declarations only under `<sys/poll.h>`. Core sources use
 * the POSIX spelling (`<poll.h>`) and stay platform-neutral; this
 * component-private include dir absorbs the quirk, exactly like the platform TU
 * selection in CMakeLists.txt.
 */
#pragma once

#include <sys/poll.h>
