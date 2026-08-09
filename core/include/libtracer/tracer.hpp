/**
 * @file
 * @brief Umbrella header for the libtracer protocol-v1 reference implementation — the L0
 *        substrate, L1 views, the L2/L3 wire codec, the L4 graph runtime and its forwarding
 *        plane, and the UDP/TCP/loopback transports.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Including this header pulls in every layer. A translation unit that needs one layer includes
 * that layer's header directly. The opt-in pieces are deliberately absent and are included by
 * name: the WebSocket, CAN, QUIC and WebTransport transports, the CUDA backend, the lazy
 * decode tier, and the ACL policy seam — each carries dependencies a minimal node should not
 * pay for.
 */
#pragma once

#include "libtracer/backend.hpp"
#include "libtracer/child_registry.hpp"
#include "libtracer/conn_spec.hpp"
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
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/view.hpp"
