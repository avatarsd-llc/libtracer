// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

/**
 * @brief The stream framing shared by every libtracer byte-stream transport
 * (M6): each libtracer TLV frame travels as `u32-LE length ++ frame bytes`.
 *
 * This module is the pure, socket-free codec — byte-identical to the C++
 * `tcp_transport_t` / `quic_transport_t` / `webtransport_transport_t` framing
 * (the prefix is TRANSPORT framing, not part of the TLV).
 */

/** @brief The length prefix: 4 bytes, u32 little-endian. */
export const PREFIX_BYTES = 4;

/**
 * @brief The largest frame the prefix may announce (16 MiB — the C++
 * `kMaxFrame` cap).
 *
 * A larger prefix is malformed: the stream has lost framing sync and cannot be
 * trusted again.
 */
export const MAX_FRAME = 16 * 1024 * 1024;

/**
 * @brief Thrown by {@link FrameReassembler.push} on a prefix announcing more
 * than {@link MAX_FRAME} bytes — the stream is desynced and must be torn down.
 */
export class MalformedPrefixError extends Error {
  /** @brief The length the malformed prefix announced. */
  readonly announced: number;

  constructor(announced: number) {
    super(`malformed length prefix: ${announced} > MAX_FRAME (${MAX_FRAME})`);
    this.name = 'MalformedPrefixError';
    this.announced = announced;
  }
}

/** @brief Encode one frame as a length-prefixed record: `u32-LE length ++ frame`. */
export function encodeRecord(frame: Uint8Array): Uint8Array {
  if (frame.byteLength > MAX_FRAME) {
    throw new RangeError(`frame of ${frame.byteLength} bytes exceeds MAX_FRAME (${MAX_FRAME})`);
  }
  const out = new Uint8Array(PREFIX_BYTES + frame.byteLength);
  new DataView(out.buffer).setUint32(0, frame.byteLength, true);
  out.set(frame, PREFIX_BYTES);
  return out;
}

/**
 * @brief Reassemble length-prefixed records from arbitrary stream chunks (a
 * QUIC stream delivers reads at transport-chosen boundaries: prefixes split
 * across chunks, records coalesced into one chunk — both are normal).
 *
 * `push(chunk)` returns every COMPLETE frame the chunk finished; partial bytes
 * are buffered for the next push. A zero-length record is a no-op (skipped,
 * matching the C++ reassembler); an oversize prefix throws
 * {@link MalformedPrefixError} — after that the reassembler must be discarded
 * with its stream.
 */
export class FrameReassembler {
  private pending: Uint8Array = new Uint8Array(0);

  /** @brief Feed one stream chunk; returns the frames it completed (possibly none). */
  push(chunk: Uint8Array): Uint8Array[] {
    if (this.pending.byteLength === 0) {
      this.pending = chunk;
    } else {
      const merged = new Uint8Array(this.pending.byteLength + chunk.byteLength);
      merged.set(this.pending, 0);
      merged.set(chunk, this.pending.byteLength);
      this.pending = merged;
    }

    const frames: Uint8Array[] = [];
    let off = 0;
    while (this.pending.byteLength - off >= PREFIX_BYTES) {
      const view = new DataView(this.pending.buffer, this.pending.byteOffset + off);
      const len = view.getUint32(0, true);
      if (len > MAX_FRAME) {
        this.pending = new Uint8Array(0);
        throw new MalformedPrefixError(len);
      }
      if (this.pending.byteLength - off - PREFIX_BYTES < len) break; // await the body
      if (len > 0) {
        // Copy out so callers own their bytes independent of the buffer.
        frames.push(this.pending.slice(off + PREFIX_BYTES, off + PREFIX_BYTES + len));
      }
      off += PREFIX_BYTES + len;
    }
    this.pending = off === 0 ? this.pending : this.pending.slice(off);
    return frames;
  }

  /** @brief Bytes buffered awaiting the rest of a prefix/body (diagnostics). */
  get buffered(): number {
    return this.pending.byteLength;
  }
}
