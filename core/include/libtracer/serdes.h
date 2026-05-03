/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
 *
 * @file serdes.h
 * @brief Serialization/deserialization interface for TLV-backed types.
 */

#ifndef SERDES_I
#define SERDES_I

/* will convert tlv_t to internal type and vice versa */
class serdes_i {
    public:
    virtual void serialize() = 0;
    virtual void deserialize() = 0;
};

#endif
