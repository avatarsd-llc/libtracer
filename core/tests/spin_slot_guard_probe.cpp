/**
 * @file
 * @brief The probe translation unit for `spin_slot_guard` (#1618): it includes the header that
 *        carries the slot assertions and odr-uses the bound slot.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Which configuration it sees is decided by the include path `cmake/spin_slot_guard.cmake`
 * hands the compiler, one arm at a time.
 */
#include "libtracer/vertex.hpp"

/** @brief Keeps the bound slot odr-used so no toolchain can skip the assertions' subject. */
int main() {
    tr::graph::lkv_slot_t slot;
    return slot.load() == nullptr ? 0 : 1;
}
