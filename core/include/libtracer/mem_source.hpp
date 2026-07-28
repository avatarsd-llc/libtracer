/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source — the L0 nothrow block seam every FAILABLE allocation draws from
 * (#551). Raw bytes, failure by value, no refcount.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>

/**
 * @file
 * @brief The nothrow failable-block seam (`tr::mem::block_source_t`), the
 *        process-wide platform-heap source that backs it by default, and the two
 *        companions the migrated call sites need: a bump source over a caller
 *        buffer and a nothrow growable array.
 */

namespace tr::mem {

/**
 * @brief The nothrow block seam every FAILABLE allocation draws from — the ones a
 *        PEER can provoke (#551, ADR-0065; ADR-0039 erratum 5/6).
 *
 * @note "Failable", not "control-plane": CONTEXT.md already binds *control plane* to the
 *       `:` field-write addressing plane, and this seam is orthogonal to that axis — a
 *       DATA-plane branch write is one of its first consumers.
 *
 * RFC-0014 made vertex registration a **runtime, wire-driven** operation: a peer's
 * CREATE frame reaches `register_vertex_key`. Every allocation on that path is an
 * unguarded throwing one, and ESP-IDF link-wraps `__cxa_throw` /
 * `__cxa_allocate_exception` to `abort()` stubs — so on the shipping profile a peer
 * can reboot the node by exhausting the heap. This seam is the failure-by-value
 * answer: exhaustion returns `nullptr` and the operation answers BACKPRESSURE.
 *
 * @note Deliberately NOT a `std::pmr::memory_resource`, and not derived from one.
 *       That type's `allocate` is annotated `__attribute__((__returns_nonnull__))`
 *       (libstdc++ `bits/memory_resource.h`), so a caller's `if (p == nullptr)` is
 *       undefined-behaviour-deletable. Measured on riscv32-esp-elf-g++ 15.2.0 with
 *       the deployment flags: the soft-fail branch survives at `-O0`/`-O1`/`-O2`/`-O3`
 *       and is GONE at `-Os`/`-Oz` — the level the reference node ships at
 *       (`CONFIG_COMPILER_OPTIMIZATION_SIZE`), and the level at which no job exercises an
 *       allocation-failure path (see ADR-0065 §1).
 *       Inheriting would keep that `allocate()` publicly callable on this object,
 *       one token away from every correct `try_alloc` call site, with no diagnostic
 *       at any warning level. A separate type makes the slip a compile error.
 *
 * @note Also distinct from @ref mem_backend_t, which vends a refcounted
 *       @ref view::segment_t. Control-plane blocks have a single owner and no
 *       header; a refcount on them is pure overhead (a `segment_t` measures 20 B on
 *       rv32 / 40 B on x86-64 against an 80 B `vertex_t`).
 *
 * @note Blocks are host-owned storage: the source MUST outlive the `graph_t` and
 *       every object built in its blocks. Teardown is driven by whoever holds the
 *       source, never by the object itself — a `vertex_t` has no room for the
 *       pointer (`core/tests/vertex_size_test.cpp`).
 *
 * @note Each source declares its own concurrency contract, exactly as
 *       @ref mem_backend_t does (ADR-0012). The RFC-0014 wire-driven registration
 *       path runs on a transport thread, so an injected source must be thread-safe
 *       on that target. @ref heap_source_t is.
 */
class block_source_t {
   public:
    /** @brief Construct a source with a stable, human-readable @p name (e.g. `"heap"`). */
    explicit constexpr block_source_t(const char* name) noexcept : name_(name) {}
    /** @brief Sources are held by pointer and outlive their users; virtual teardown. */
    virtual ~block_source_t() = default;

    /** @brief Non-copyable — a source is an identity, not a value. */
    block_source_t(const block_source_t&) = delete;
    /** @brief Non-assignable. */
    block_source_t& operator=(const block_source_t&) = delete;

    /**
     * @brief Obtain @p bytes of storage aligned to at least @p align — NOTHROW.
     *
     * @param bytes Size of the block; a zero-sized request is implementation-defined
     *              and callers do not make one.
     * @param align Minimum alignment, a power of two.
     * @retval nullptr Exhaustion. The caller answers BACKPRESSURE; it never falls back
     *                 to the global heap and never aborts.
     */
    [[nodiscard]] virtual void* try_alloc(
        std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept = 0;

    /**
     * @brief Return a block previously handed out by @ref try_alloc.
     *
     * @warning @p bytes and @p align MUST match the originating @ref try_alloc call
     *          (sized reclaim), so a bump or pool source needs no per-block header.
     */
    virtual void release(void* p, std::size_t bytes,
                         std::size_t align = alignof(std::max_align_t)) noexcept = 0;

    /** @brief The source's stable name, for census and diagnostics. */
    [[nodiscard]] const char* name() const noexcept { return name_; }

   private:
    const char* name_; /**< @brief Borrowed literal; never owned. */
};

/**
 * @brief The default source: the platform heap, nothrow.
 *
 * Behaviour is byte-identical to today for a host that injects nothing, EXCEPT that
 * exhaustion returns `nullptr` instead of reaching the ESP-IDF `__cxa_throw` abort
 * stub. Thread-safe: the global nothrow `operator new` is.
 */
class heap_source_t final : public block_source_t {
   public:
    /** @brief Constant-initializable, so the process-wide default costs no dynamic init. */
    constexpr heap_source_t() noexcept : block_source_t("heap") {}

    /** @brief Nothrow aligned heap allocation; `nullptr` on exhaustion. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }

    /** @brief Sized, aligned reclaim matching @ref try_alloc. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ::operator delete(p, bytes, std::align_val_t{align});
    }
};

/**
 * @brief The process-wide default @ref block_source_t (the platform heap).
 *
 * A namespace-scope `constinit` object behind a function, NOT a function-local static:
 * the latter costs a `__cxa_guard` word in `.bss` and an acquire fence on every call.
 */
[[nodiscard]] block_source_t& heap_source() noexcept;

/**
 * @brief The source that serves nothing — every request is exhaustion.
 *
 * The upstream to give a @ref bump_source_t when its buffer must be the HARD bound, so a
 * frame that outgrows it is rejected rather than reaching the global heap. This is the
 * bounded-node composition, and the honest replacement for
 * `std::pmr::null_memory_resource()`, which signals the same thing by throwing.
 */
class null_source_t final : public block_source_t {
   public:
    /** @brief Constant-initializable, like @ref heap_source_t. */
    constexpr null_source_t() noexcept : block_source_t("null") {}
    /** @brief Always `nullptr`. */
    [[nodiscard]] void* try_alloc(std::size_t, std::size_t) noexcept override { return nullptr; }
    /** @brief Unreachable — this source hands out nothing to return. */
    void release(void*, std::size_t, std::size_t) noexcept override {}
};

/** @brief The process-wide @ref null_source_t (serves nothing; see the class docs). */
[[nodiscard]] block_source_t& null_source() noexcept;

/**
 * @brief A caller-owned buffer handed out by bump, falling back to @p upstream once full.
 *
 * The nothrow twin of `std::pmr::monotonic_buffer_resource` over a fixed span. Blocks
 * carved from the span are never individually reclaimed (@ref release is a no-op for
 * them, exactly as a monotonic resource behaves); blocks that came from the upstream are
 * returned to it, so a decode that outgrows the buffer still frees what it borrowed.
 *
 * @note The upstream is what keeps this a capability-preserving substitution: a
 *       `monotonic_buffer_resource` also spills past its buffer, but it spills to a
 *       THROWING default resource, which on `-fno-exceptions` is the `abort()` this whole
 *       seam exists to remove. Pass a bounded source (or a null-serving one) to make the
 *       buffer the hard limit instead.
 *
 * @warning SCOPE-LIFETIME USE ONLY. A bump block is never reclaimed, so a source that
 *          outlives one burst of work monotonically fills and then refuses everything.
 *          Construct it per operation (as the branch-write decode does), or @ref reset it
 *          between operations. It is NOT a long-lived seam: an 8 KiB bump source wired as
 *          a router's `rx` decoded 6 frames and rejected the next 194 — measured. A
 *          long-lived bounded seam wants @ref pool_source_t, which recycles.
 * @note Single-threaded by contract — a bump cursor is not synchronized. Its intended use
 *       is a function-scoped buffer on the calling thread's stack.
 */
class bump_source_t final : public block_source_t {
   public:
    /** @brief Carve from @p buffer; once it cannot serve a request, draw from @p upstream. */
    explicit bump_source_t(std::span<std::byte> buffer,
                           block_source_t& upstream = heap_source()) noexcept
        : block_source_t("bump"), buf_(buffer), upstream_(&upstream) {}

    /** @brief Bump-allocate, aligned; falls back to the upstream when the buffer cannot fit. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(buf_.data()) + used_;
        // Padding to the next `align` boundary. Mask, not modulo: `align` is a power of
        // two by contract, and two integer divisions here are worth ~20 % of a terminus
        // decode — measured, on the way in.
        const auto pad = static_cast<std::size_t>((~cur + 1U) & (align - 1U));
        if (pad <= buf_.size() - used_ && bytes <= buf_.size() - used_ - pad) {
            used_ += pad + bytes;
            return buf_.data() + (used_ - bytes);
        }
        return upstream_->try_alloc(bytes, align);
    }

    /** @brief No-op for a bump block; a sized return to the upstream otherwise. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        auto* b = static_cast<std::byte*>(p);
        if (b >= buf_.data() && b < buf_.data() + buf_.size()) return;  // bump: never reclaimed
        upstream_->release(p, bytes, align);
    }

    /**
     * @brief Hand the whole buffer back for reuse — the next @ref try_alloc starts at 0.
     *
     * For a source reused across scopes (a terminus decoding frame after frame into the
     * same slab). Deliberately NOT called `release`: on a
     * `std::pmr::monotonic_buffer_resource` that name means exactly this, while on
     * @ref block_source_t it means "return one block", and the two must not be confusable
     * at a call site.
     *
     * @warning Every block previously carved from the buffer dangles afterwards. Blocks
     *          that came from the upstream are NOT reclaimed by this — return those first.
     */
    void reset() noexcept { used_ = 0; }

    /** @brief Bytes carved from the buffer so far (diagnostics; excludes upstream blocks). */
    [[nodiscard]] std::size_t used() const noexcept { return used_; }

   private:
    std::span<std::byte> buf_;
    block_source_t* upstream_;
    std::size_t used_ = 0;
};

/**
 * @brief The no-op synchronization policy — the default, and the one the hot seam wants.
 *
 * A @ref pool_source_t owned by exactly one thread needs no synchronization at all, and
 * that is the intended shape for a per-receiver source: ownership removes the race
 * instead of guarding it. See @ref pool_source_t's threading note for why this matters
 * more than the choice of free-list algorithm.
 *
 * A policy is anything with `lock()`/`unlock()`; a target supplies its own where it needs
 * one (an interrupt-disable critical section on single-core FreeRTOS,
 * `tr::mem::sync_mutex_t` from `mem_source_sync.hpp` on a host). This header stays
 * freestanding-clean, so it pulls in no threading facility of its own.
 */
struct sync_none_t {
    /** @brief No-op — this policy exists to compile to nothing. */
    static void lock() noexcept {}
    /** @brief No-op. */
    static void unlock() noexcept {}
};

/**
 * @brief One recycling free-list, keyed by the exact `(bytes, align)` pair it serves.
 *
 * Caller-supplied storage: a @ref pool_source_t is handed a span of these, so the number
 * of classes is a deployment property rather than a constant in this header (ADR-0065's
 * injected-bounds rule). Sizing it is measurable — see @ref pool_source_t::classes_used.
 */
struct size_class_t {
    std::size_t bytes = 0; /**< @brief Normalized payload size; 0 marks an unused slot. */
    std::size_t align = 0; /**< @brief Normalized alignment this class's blocks satisfy. */
    void* head = nullptr;  /**< @brief Intrusive free-list head; the link lives in the block. */
};

/**
 * @brief A BOUNDED, RECYCLING source: segregated exact-size free lists over a caller slab.
 *
 * The long-lived counterpart to @ref bump_source_t, and the source a node with a RAM
 * ceiling injects. Exhaustion is `nullptr` — never the platform heap, never an abort.
 *
 * ### Why exact-size classes, measured
 *
 * The demand at this seam is nearly degenerate. Recording every `try_alloc`/`release`
 * across the host suite (70,937 events) found **12 distinct sizes, three of which cover
 * 99.8 % of all allocations** — they are the arena's geometrically growing arrays. So
 * exact classes cost **zero internal fragmentation**, and a first-fit-with-coalescing
 * allocator's header buys nothing back. Replaying that trace against both policies:
 *
 * | policy | slab to serve the trace | vs peak-live floor |
 * | --- | ---: | ---: |
 * | this one | 26,176 B | +11.1 % |
 * | first-fit + coalescing | 27,448 B | +16.5 % |
 *
 * The gap is **1,088 B of external fragmentation and only 184 B of header** — splitting a
 * remainder under geometric growth rarely produces the size of the next request. Note what
 * that says about the usual argument for a header-free pool: here it is worth 0.7 % of the
 * difference, so it is not the reason to choose this shape.
 *
 * Code size of what actually ships, `riscv32-esp-elf-g++ -Os -fno-exceptions -fno-rtti`:
 * **322 B** of text (`try_alloc` 120, `release` 142, `find` 46, teardown 14) plus a 24 B
 * vtable. The two 256 B / 380 B figures quoted while choosing between the policies were
 * feature-matched *prototypes* — neither carried the alignment key, the overflow counter
 * or the foreign-pointer check this one does — so they compare the shapes to each other
 * and are not the shipped cost of either.
 *
 * ### Threading — read this before sharing one
 *
 * @warning A shared pool is the WRONG shape for a hot multi-core path, and this is
 *          measured, not feared. ADR-0060 erratum 1 recorded a shared free-list pool
 *          collapsing to roughly **a fifteenth of its own single-thread rate** on a
 *          12-core host (8.3 M → 1.36 M ops/s; p50 60 ns → 3587 ns) while the platform
 *          heap *scaled* — a cacheline storm, not serialization. A lock-free CAS on the
 *          list head does not fix it: it replaces one contended word with the same word.
 *          The shape that works is per-thread free lists, and the cheapest way to get
 *          them is to give each receiver its **own** source rather than to guard a shared
 *          one. Reach for a locking @p Sync only where the seam is wiring-frequency (a
 *          graph's control source), never per-frame.
 *
 * @tparam Sync Synchronization policy (`lock()`/`unlock()`); @ref sync_none_t by default,
 *              which compiles to nothing.
 */
template <class Sync = sync_none_t>
class pool_source_t final : public block_source_t {
   public:
    /**
     * @brief Serve allocations from @p slab, recycling through @p classes.
     *
     * @param slab    Caller-owned storage; must outlive every block carved from it.
     * @param classes Caller-owned free-list slots. Running out is safe but lossy — see
     *                @ref overflowed.
     */
    pool_source_t(std::span<std::byte> slab, std::span<size_class_t> classes) noexcept
        : block_source_t("pool"), buf_(slab), cls_(classes) {}

    /** @brief Pop a recycled block of this exact shape, else carve a fresh one; `nullptr` when
     * full. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        normalize(bytes, align);
        const guard_t g{sync_};
        if (size_class_t* c = find(bytes, align); c != nullptr && c->head != nullptr) {
            void* const p = c->head;
            std::memcpy(&c->head, p, sizeof(void*));  // pop the intrusive link
            return p;
        }
        // Padding to the next `align` boundary. Mask, not modulo: `align` is a power of two
        // by contract, and the division showed up as ~20 % of a terminus decode.
        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(buf_.data()) + used_;
        const auto pad = static_cast<std::size_t>((~cur + 1U) & (align - 1U));
        if (pad > buf_.size() - used_ || bytes > buf_.size() - used_ - pad) return nullptr;
        used_ += pad + bytes;
        return buf_.data() + (used_ - bytes);
    }

    /**
     * @brief Return a block to its class's free list.
     *
     * @p bytes and @p align MUST match the originating @ref try_alloc, per the seam's sized
     * contract — that is what lets a block carry no header. A pointer from outside the slab
     * is ignored rather than trusted, mirroring `bump_source_t` — two compares are cheaper
     * than the corruption a foreign pointer would cause.
     */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        auto* const b = static_cast<std::byte*>(p);
        if (b < buf_.data() || b >= buf_.data() + buf_.size()) return;
        normalize(bytes, align);
        const guard_t g{sync_};
        size_class_t* c = find(bytes, align);
        if (c == nullptr) c = claim(bytes, align);
        if (c == nullptr) {
            // The class table is full. The block stays carved — bounded and safe, never
            // corrupt — and the loss is counted so a deployment can size the span.
            ++overflow_;
            return;
        }
        std::memcpy(p, &c->head, sizeof(void*));  // push the intrusive link
        c->head = p;
    }

    /** @brief Bytes carved from the slab so far, recycled blocks included (diagnostics). */
    [[nodiscard]] std::size_t used() const noexcept { return used_; }

    /** @brief Class slots in use — the number to size the injected span against. */
    [[nodiscard]] std::size_t classes_used() const noexcept { return n_; }

    /** @brief Blocks lost because the class table was full; non-zero means the span is too small.
     */
    [[nodiscard]] std::size_t overflowed() const noexcept { return overflow_; }

   private:
    /** @brief RAII lock over the policy; empty and free when @p Sync is @ref sync_none_t. */
    struct guard_t {
        explicit guard_t(Sync& s) noexcept : s_(s) { s_.lock(); }
        ~guard_t() { s_.unlock(); }
        guard_t(const guard_t&) = delete;
        guard_t& operator=(const guard_t&) = delete;
        Sync& s_;
    };

    /**
     * @brief Widen a request so a freed block can host the intrusive link.
     *
     * Applied identically on both sides, so the `(bytes, align)` a `release` computes is
     * the one its `try_alloc` computed — the whole header-free scheme rests on that.
     * Alignment is part of the key rather than folded away: a block carved for `align=4`
     * cannot be handed back out for an `align=8` request.
     */
    static void normalize(std::size_t& bytes, std::size_t& align) noexcept {
        if (bytes < sizeof(void*)) bytes = sizeof(void*);
        if (align < alignof(void*)) align = alignof(void*);
    }

    /** @brief The slot serving this exact shape, or `nullptr`. */
    [[nodiscard]] size_class_t* find(std::size_t bytes, std::size_t align) noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (cls_[i].bytes == bytes && cls_[i].align == align) return &cls_[i];
        }
        return nullptr;
    }

    /** @brief Take the next free slot for this shape, or `nullptr` when the span is full. */
    [[nodiscard]] size_class_t* claim(std::size_t bytes, std::size_t align) noexcept {
        if (n_ == cls_.size()) return nullptr;
        cls_[n_] = size_class_t{bytes, align, nullptr};
        return &cls_[n_++];
    }

    std::span<std::byte> buf_;
    std::span<size_class_t> cls_;
    std::size_t used_ = 0;
    std::size_t n_ = 0;
    std::size_t overflow_ = 0;
    [[no_unique_address]] Sync sync_{};
};

/**
 * @brief A nothrow growable array of trivially-copyable @p T drawn from a @ref block_source_t.
 *
 * The container a failable path uses where a `std::pmr::vector` would otherwise sit
 * (#551 Q2, #588). Two differences carry the whole point:
 *
 * 1. **Growth returns `false` instead of throwing.** `std::pmr::vector::push_back` on an
 *    exhausted resource throws, which on ESP-IDF reaches the link-wrapped `__cxa_throw`
 *    `abort()` stub — a peer-reachable reboot when the container sits on the RX decode path.
 * 2. **Relocation is a `memcpy`.** `T` is required trivially copyable, so growth needs no
 *    move loop and the vacated block needs no destruction. Both current users
 *    (`wire::arena_tlv_t`, the walk's open-node record) are span/enum aggregates.
 *
 * Same footprint as `std::pmr::vector` (four words), one virtual call per growth instead of
 * the allocator's two.
 */
template <class T>
class block_array_t {
    static_assert(std::is_trivially_copyable_v<T>, "block_array_t relocates by memcpy");
    static_assert(std::is_trivially_destructible_v<T>, "block_array_t never runs destructors");

   public:
    /** @brief An empty array that will draw its storage from @p src. */
    explicit block_array_t(block_source_t& src) noexcept : src_(&src) {}
    /** @brief Returns the block, if one was taken. */
    ~block_array_t() { give_back(); }

    /** @brief Non-copyable — one array, one block. */
    block_array_t(const block_array_t&) = delete;
    /** @brief Non-assignable. */
    block_array_t& operator=(const block_array_t&) = delete;
    /** @brief Move-constructible so a decode can return its arena by value. */
    block_array_t(block_array_t&& o) noexcept
        : src_(o.src_), data_(o.data_), end_(o.end_), cap_(o.cap_) {
        o.data_ = o.end_ = o.cap_ = nullptr;
    }
    /** @brief Move-assignable (releases this array's block first). */
    block_array_t& operator=(block_array_t&& o) noexcept {
        if (this != &o) {
            give_back();
            src_ = o.src_;
            data_ = o.data_;
            end_ = o.end_;
            cap_ = o.cap_;
            o.data_ = o.end_ = o.cap_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Ensure room for @p n elements without growing again.
     * @retval false The source is exhausted — the array is unchanged.
     */
    [[nodiscard]] bool reserve(std::size_t n) noexcept {
        return n <= static_cast<std::size_t>(cap_ - data_) || regrow(n);
    }

    /**
     * @brief Append @p v.
     * @retval false The source is exhausted — the array is unchanged (BACKPRESSURE).
     */
    [[nodiscard]] bool push_back(const T& v) noexcept {
        if (end_ == cap_ && !grow()) return false;
        *end_++ = v;
        return true;
    }

    /**
     * @brief Claim one uninitialized slot at the end and return it — fill it IN PLACE.
     * @retval nullptr The source is exhausted — the array is unchanged (BACKPRESSURE).
     *
     * The form the hot paths use. `push_back(T{...})` has to materialize the aggregate on
     * the stack and copy it in, and for a 48-byte `T` written field-by-field then read back
     * as wide loads that is a store-forwarding stall on every element: measured on the
     * terminus decode, ~45 % slower with FEWER instructions executed. Writing through this
     * slot removes the temporary entirely.
     */
    [[nodiscard]] T* push_slot() noexcept {
        if (end_ == cap_ && !grow()) return nullptr;
        return end_++;
    }

    /** @brief Drop the last element. Precondition: not empty. */
    void pop_back() noexcept { --end_; }
    /** @brief The last element. Precondition: not empty. */
    [[nodiscard]] T& back() noexcept { return end_[-1]; }
    /** @brief The first element. Precondition: not empty. */
    [[nodiscard]] const T& front() const noexcept { return *data_; }
    /** @brief Element @p i, unchecked. */
    [[nodiscard]] T& operator[](std::size_t i) noexcept { return data_[i]; }
    /** @brief Element @p i, unchecked (const). */
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return data_[i]; }
    /** @brief Element count. */
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(end_ - data_);
    }
    /** @brief True when no elements are held. */
    [[nodiscard]] bool empty() const noexcept { return end_ == data_; }
    /**
     * @brief First element, or `nullptr` when empty — the contiguous block.
     *
     * For handing the array to an API that takes a pointer/length pair, e.g. building a
     * `std::span` over an egress iov table. The pointer is invalidated by any growth.
     */
    [[nodiscard]] T* data() noexcept { return data_; }
    /** @brief First element (const), or `nullptr` when empty. */
    [[nodiscard]] const T* data() const noexcept { return data_; }

   private:
    void give_back() noexcept {
        if (data_ != nullptr)
            src_->release(data_, static_cast<std::size_t>(cap_ - data_) * sizeof(T), alignof(T));
    }

    // The cold half of push_back, kept OUT OF LINE: growth happens once or twice per
    // terminus decode, and inlining it into the caller doubles the hot loop's live range.
    [[gnu::noinline]] [[nodiscard]] bool grow() noexcept {
        const std::size_t have = static_cast<std::size_t>(cap_ - data_);
        return regrow(have < 4 ? 8 : have * 2);
    }

    // Relocate into a block of `want` elements. `false` leaves the array untouched, which
    // is what lets every caller treat exhaustion as a clean reject.
    [[gnu::noinline]] [[nodiscard]] bool regrow(std::size_t want) noexcept {
        void* fresh = src_->try_alloc(want * sizeof(T), alignof(T));
        if (fresh == nullptr) return false;
        const std::size_t n = static_cast<std::size_t>(end_ - data_);
        if (n > 0) std::memcpy(fresh, data_, n * sizeof(T));
        give_back();
        data_ = static_cast<T*>(fresh);
        end_ = data_ + n;
        cap_ = data_ + want;
        return true;
    }

    block_source_t* src_;
    T* data_ = nullptr;
    T* end_ = nullptr;
    T* cap_ = nullptr;
};

}  // namespace tr::mem
