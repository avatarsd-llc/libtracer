/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * rmw_tracer identity — the implementation-identifier and serialization-format
 * entry points (the minimal real RMW surface). The remaining ~198 rmw_* entry
 * points (publish/subscribe/loaned/wait/qos/graph/service) are the staged work
 * documented in ../../README.md and docs/adr/0023, mapped onto the libtracer
 * graph. This TU exists so the ament package + libtracer + the ROS rmw headers
 * build together (scripts/build-ros.sh) before that work lands.
 */
#include "rmw/rmw.h"

const char *rmw_get_implementation_identifier(void) { return "rmw_tracer"; }

/* ROS messages are CDR-serialized by rosidl; libtracer carries the bytes opaque
 * (VALUE payload), so the format ROS sees on the wire is CDR. */
const char *rmw_get_serialization_format(void) { return "cdr"; }
