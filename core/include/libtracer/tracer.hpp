/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Umbrella header for the libtracer protocol-v1 reference implementation.
 * M1 (this milestone) ships the L2/L3 wire codec; the L0/L1 substrate and the
 * L4 in-process graph runtime follow (see core/README.md).
 */
#pragma once

#include "libtracer/backend.hpp"
#include "libtracer/child_registry.hpp"
#include "libtracer/crc.hpp"
#include "libtracer/error.hpp"
#include "libtracer/frame.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/mem_borrowed.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/path.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/status.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_arena.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/view.hpp"
