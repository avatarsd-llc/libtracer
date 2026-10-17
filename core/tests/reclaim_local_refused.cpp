/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Compiled, never linked, by the `reclaim_local_refused` test (#2127): against a fragment
 *        that binds the withdrawn `reclaim_local_t`, this TU must fail to compile, and the
 *        first diagnostic must be the migration note.
 */

#include "libtracer/tracer.hpp"
