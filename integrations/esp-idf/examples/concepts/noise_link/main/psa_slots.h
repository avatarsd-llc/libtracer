/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief mbedTLS user config for this example (MBEDTLS_USER_CONFIG_FILE): static PSA key slots,
 *        sized for two Noise links and nothing else.
 *
 * Without static slots every key import allocates, a first message's handshake key included,
 * and security_noise_psa.hpp refuses to build. The rule (docs/modules/security-noise.md): at
 * least 6 slots per Noise link (two sessions of two keys during a rekey, the handshake cipher
 * and the ephemeral key), plus the peak of the image's other PSA users, each slot as large as
 * the largest key imported. This image has two links and no other PSA user: 12 slots of 32 B.
 *
 * With IDF v6.0's mbedTLS 4 the two sizes must skip mbedTLS's own config-check unit, which
 * refuses PSA values from this file; the guard is a no-op on mbedTLS 3.6.
 */
#pragma once

#define MBEDTLS_PSA_STATIC_KEY_SLOTS
#undef MBEDTLS_PSA_KEY_STORE_DYNAMIC
#if !defined(MBEDTLS_CONFIG_CHECK_WASSET_MBEDTLS_PSA_KEY_SLOT_COUNT)
#define MBEDTLS_PSA_KEY_SLOT_COUNT 12
#define MBEDTLS_PSA_STATIC_KEY_SLOT_BUFFER_SIZE 32
#endif
