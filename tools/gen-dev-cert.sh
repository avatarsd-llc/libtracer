#!/usr/bin/env sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# gen-dev-cert.sh — emit a self-signed DEV certificate pair for the QUIC
# transport's LISTEN mode (ADR-0043 Phase A): <out-dir>/cert.pem + <out-dir>/key.pem.
#
# DEV ONLY: a self-signed certificate cannot chain to any CA, so the dialing
# side must use quic_dial_tls_t{.insecure_no_verify = true} (or trust the cert
# file directly as its CA bundle). Deployments use a real certificate.
#
# Usage: scripts/gen-dev-cert.sh [out-dir]   (default: current directory)
set -eu

out="${1:-.}"
mkdir -p "$out"

openssl req -x509 -newkey rsa:2048 -sha256 -days 365 -nodes \
    -keyout "$out/key.pem" -out "$out/cert.pem" \
    -subj "/CN=libtracer-dev" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1

echo "dev cert written: $out/cert.pem + $out/key.pem (self-signed — DEV ONLY)"
