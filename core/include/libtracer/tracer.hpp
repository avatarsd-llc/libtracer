/**
 * @file
 * @brief Umbrella header for the libtracer protocol-v1 reference implementation — includes the
 *        whole default surface: L0 substrate, L1 views, the L2/L3 wire codec, the L4 graph
 *        runtime, and the built-in transports.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Including this header pulls in every layer. A translation unit that needs one layer includes
 * that layer's header directly; the opt-in transports (QUIC, WebTransport, CAN, WebSocket) and
 * the CUDA backend are deliberately absent — they carry their own dependencies and are included
 * by name.
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
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/vertex.hpp"
#include "libtracer/view.hpp"
