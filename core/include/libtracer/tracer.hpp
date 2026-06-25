// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// Umbrella header for the libtracer protocol-v1 reference implementation.
// M1 (this milestone) ships the L2/L3 wire codec; the L0/L1 substrate and the
// L4 in-process graph runtime follow (see core/README.md).
#pragma once

#include "libtracer/crc.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/tlv.hpp"
