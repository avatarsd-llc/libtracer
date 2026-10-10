# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# The ESP-IDF half of the Noise link harness's crypto arm (#2065): the same NNpsk0 handshake and
# ChaCha20-Poly1305 frame code as bench_noise_crypto, over the mbedTLS PSA backend ESP-IDF
# ships, so the flash and static RAM a backend costs an image come from `idf.py size`. The
# backend is a Kconfig choice (NOISE_BENCH_BACKEND); the `none` build is the same app with
# every crypto call compiled out, and the difference between the two images is the backend.
# See bench/README.md §Noise link harness. Not built by CI; a diagnostic, not a gate.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_noise_crypto)
