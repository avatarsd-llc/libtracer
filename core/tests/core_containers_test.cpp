/**
 * @file
 * @brief Unit tests for the core container set (#1776, ADR-0083 Decision 2 and 9): the vector
 *        (`block_array_t`), the name/string store (`string_t`), the sorted map
 *        (`sorted_map_t`, and the leaf-chunked `chunked_map_t`) and the non-owning
 *        `function_ref_t`, and the polymorphic owner `poly_ptr_t` (#1780, #2022).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every growing call is driven against a source that refuses its Nth request, for each N
 * the operation can reach, and the test asserts three things: the refusal comes back as a
 * value, the container is exactly as it was (contents, and the caller's moved-in argument
 * still intact), and the source gets every byte back (sized release matches). The target is
 * compiled with `-fno-exceptions`, which is the ticket's "compiles in a no-exceptions build"
 * criterion: nothing here may need a throw to report a refusal.
 */

#include <cstddef>
#include <cstdio>
#include <map>
#include <string_view>
#include <type_traits>
#include <utility>

#include "libtracer/function_ref.hpp"
#include "libtracer/mem_chunked_map.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/mem_sorted_map.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_string.hpp"
#include "test_support.hpp"

namespace {

using tr::testing::check;

/**
 * @brief A heap-backed source that refuses exactly its Nth request (1-based; 0 = never) and
 *        counts the bytes and blocks still out, so a test can prove the sized release.
 */
class refuse_nth_source_t final : public tr::mem::block_source_t {
   public:
    /** @brief Refuse request number @p nth; 0 never refuses. */
    explicit refuse_nth_source_t(int nth) noexcept
        : tr::mem::block_source_t("refuse-nth"), nth_(nth) {}

    /** @brief Serve from the heap, except the Nth request. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (++calls_ == nth_) {
            ++refused_;
            return nullptr;
        }
        void* p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) {
            bytes_out_ += bytes;
            ++blocks_out_;
        }
        return p;
    }
    /** @brief Return to the heap and settle the account. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        bytes_out_ -= bytes;
        --blocks_out_;
        tr::mem::heap_source().release(p, bytes, align);
    }

    int calls_ = 0;              /**< @brief Requests seen. */
    int refused_ = 0;            /**< @brief Requests refused. */
    std::size_t bytes_out_ = 0;  /**< @brief Bytes handed out and not returned. */
    std::size_t blocks_out_ = 0; /**< @brief Blocks handed out and not returned. */

   private:
    int nth_; /**< @brief The request number to refuse. */
};

/** @brief Live `tracked_t` objects, so a test sees every constructor matched by a destructor. */
int g_live = 0;

/** @brief A non-trivial element: counts its lives and marks a moved-from husk. */
struct tracked_t {
    /** @brief Hold @p v. */
    explicit tracked_t(int v) noexcept : value(v) { ++g_live; }
    /** @brief Copy. */
    tracked_t(const tracked_t& o) noexcept : value(o.value) { ++g_live; }
    /** @brief Move; the source keeps -1. */
    tracked_t(tracked_t&& o) noexcept : value(o.value) {
        o.value = -1;
        ++g_live;
    }
    /** @brief Copy-assign. */
    tracked_t& operator=(const tracked_t& o) noexcept = default;
    /** @brief Move-assign; the source keeps -1. */
    tracked_t& operator=(tracked_t&& o) noexcept {
        value = o.value;
        o.value = -1;
        return *this;
    }
    /** @brief One life ends. */
    ~tracked_t() { --g_live; }

    int value; /**< @brief The payload, -1 once moved from. */
};
static_assert(!std::is_trivially_copyable_v<tracked_t>);

/** @brief The vector over a trivially copyable element: refusal at every growth step. */
void test_array_trivial() {
    for (int nth = 1; nth <= 4; ++nth) {
        refuse_nth_source_t src(nth);
        {
            tr::mem::block_array_t<int> a(src);
            bool refused_once = false;
            for (int i = 0; i < 40; ++i) {
                const std::size_t before = a.size();
                if (!a.push_back(i)) {
                    refused_once = true;
                    check(a.size() == before, "trivial: refused push_back leaves the size");
                    bool same = true;
                    for (std::size_t k = 0; k < a.size(); ++k) same = same && a[k] == int(k);
                    check(same, "trivial: refused push_back leaves the contents");
                    check(a.push_back(i), "trivial: the next push_back succeeds");
                }
            }
            check(refused_once, "trivial: the Nth request was refused");
            check(a.size() == 40 && a.back() == 39, "trivial: all 40 elements present");
            a.erase_at(0);
            check(a.size() == 39 && a.front() == 1, "trivial: erase_at shifts the tail down");
            check(a.emplace_at(0, 0) != nullptr && a[0] == 0 && a[1] == 1,
                  "trivial: emplace_at shifts the tail up");
        }
        check(src.bytes_out_ == 0 && src.blocks_out_ == 0, "trivial: every byte returned");
    }
}

/** @brief The vector over a non-trivial element: lives balance, refusal consumes nothing. */
void test_array_nontrivial() {
    for (int nth = 1; nth <= 3; ++nth) {
        refuse_nth_source_t src(nth);
        {
            tr::mem::block_array_t<tracked_t> a(src);
            for (int i = 0; i < 30; ++i) {
                tracked_t v(i);
                const std::size_t before = a.size();
                if (!a.push_back(std::move(v))) {
                    check(a.size() == before, "nontrivial: refused push_back leaves the size");
                    check(v.value == i, "nontrivial: refused push_back does not move from v");
                    check(a.push_back(std::move(v)), "nontrivial: the retry succeeds");
                }
            }
            bool in_order = a.size() == 30;
            for (std::size_t k = 0; k < a.size(); ++k) in_order = in_order && a[k].value == int(k);
            check(in_order, "nontrivial: relocation kept every value in order");
            check(g_live == 30, "nontrivial: one live object per element");
            // Insert an alias of an element at the front: the copy must be taken before the
            // shift moves that element.
            check(a.emplace_at(0, a[5]) != nullptr && a[0].value == 5 && a[6].value == 5,
                  "nontrivial: emplace_at copies an aliased argument before shifting");
            a.erase_at(0);
            a.pop_back();
            check(a.size() == 29 && a.back().value == 28 && g_live == 29,
                  "nontrivial: erase_at and pop_back destroy what they remove");
            a.clear();
            check(a.empty() && g_live == 0 && a.capacity() > 0,
                  "nontrivial: clear destroys every element and keeps the block");
            check(a.emplace_back(7) != nullptr && g_live == 1, "nontrivial: reuse after clear");
        }
        check(g_live == 0, "nontrivial: the destructor ended every life");
        check(src.bytes_out_ == 0 && src.blocks_out_ == 0, "nontrivial: every byte returned");
    }
    // Growth with an argument that aliases an element of the full array.
    refuse_nth_source_t src(0);
    {
        tr::mem::block_array_t<tracked_t> a(src);
        while (a.size() < 8) check(a.emplace_back(int(a.size())) != nullptr, "fill to capacity");
        check(a.size() == a.capacity(), "array is full");
        check(a.push_back(a[3]) && a.back().value == 3,
              "nontrivial: growth copies an aliased argument before releasing the old block");
    }
    check(g_live == 0 && src.bytes_out_ == 0, "nontrivial alias: balanced");
}

/** @brief The name/string store: exact assign, doubling append, aliasing, refusal by value. */
void test_string() {
    for (int nth = 1; nth <= 3; ++nth) {
        refuse_nth_source_t src(nth);
        {
            tr::mem::string_t s(src);
            check(s.empty() && s.c_str()[0] == '\0' && src.calls_ == 0,
                  "string: empty costs no allocation and still has a terminator");
            bool ok = s.assign("sensors/temperature");
            if (!ok) {
                check(s.empty(), "string: refused assign leaves it unchanged");
                ok = s.assign("sensors/temperature");
            }
            check(ok && s == "sensors/temperature" && s.capacity() == s.size(),
                  "string: assign sizes the block exactly");
            const std::string_view tail = "/celsius";
            if (!s.append(tail)) {
                check(s == "sensors/temperature", "string: refused append leaves it unchanged");
                check(s.append(tail), "string: the retry succeeds");
            }
            check(s == "sensors/temperature/celsius" && s.c_str()[s.size()] == '\0',
                  "string: append keeps the terminator");
            if (!s.append(s.view())) check(s.append(s.view()), "string: self-append retry");
            check(s == "sensors/temperature/celsiussensors/temperature/celsius",
                  "string: appending its own view is safe across growth");
            check(s.assign(s.view().substr(8, 11)) && s == "temperature",
                  "string: assigning its own sub-view is safe in place");
            tr::mem::string_t moved(std::move(s));
            check(moved == "temperature" && s.empty(), "string: move steals the block");
            check(moved < std::string_view("zz") && moved > std::string_view("abc"),
                  "string: byte-wise ordering against a view");
        }
        check(src.bytes_out_ == 0 && src.blocks_out_ == 0, "string: every byte returned");
    }
}

/** @brief The sorted map: order, heterogeneous lookup, refused insert keeps the caller's key. */
void test_sorted_map() {
    for (int nth = 1; nth <= 6; ++nth) {
        refuse_nth_source_t src(nth);
        {
            tr::mem::sorted_map_t<tr::mem::string_t, tracked_t> m(src);
            const std::string_view names[] = {"m", "c", "x", "a", "q", "e", "z", "b", "k"};
            int refusals = 0;
            int value = 0;
            for (std::string_view n : names) {
                tr::mem::string_t key(src);
                while (!key.assign(n)) ++refusals;
                auto r = m.try_emplace(std::move(key), value);
                if (r.value == nullptr) {
                    ++refusals;
                    check(!r.inserted, "map: a refused insert reports not inserted");
                    check(key == n, "map: a refused insert does not move from the key");
                    check(!m.contains(n), "map: a refused insert adds nothing");
                    r = m.try_emplace(std::move(key), value);
                }
                check(r.value != nullptr && r.inserted && r.value->value == value,
                      "map: insert adds the entry");
                ++value;
            }
            check(refusals == 1, "map: exactly the Nth request was refused");
            check(m.size() == 9 && g_live == 9, "map: nine entries, nine live values");
            bool sorted = true;
            for (auto it = m.begin(); it + 1 != m.end(); ++it)
                sorted = sorted && it->key < it[1].key;
            check(sorted, "map: entries iterate in key order");
            const tracked_t* x = m.find(std::string_view("x"));
            check(x != nullptr && x->value == 2, "map: heterogeneous find by string_view");
            check(m.find(std::string_view("nope")) == nullptr, "map: absent key finds null");
            tr::mem::string_t dup(src);
            check(dup.assign("x"), "dup key built");
            const auto again = m.try_emplace(std::move(dup), 99);
            check(!again.inserted && again.value == x && x->value == 2,
                  "map: a present key is not replaced");
            check(m.erase(std::string_view("c")) && !m.contains(std::string_view("c")) &&
                      m.size() == 8 && g_live == 8,
                  "map: erase removes the entry and destroys its value");
            check(!m.erase(std::string_view("c")), "map: erasing an absent key answers false");
        }
        check(g_live == 0, "map: every value destroyed");
        check(src.bytes_out_ == 0 && src.blocks_out_ == 0, "map: every byte returned");
    }
}

/** @brief A map with 8-entry leaves, so a few dozen keys split, drain and empty leaves. */
using chunked_t = tr::mem::chunked_map_t<int, tracked_t, std::less<>, 8>;

/** @brief Whether @p m holds exactly @p want's keys and values, in key order. */
bool same_as(const chunked_t& m, const std::map<int, int>& want) {
    if (m.size() != want.size()) return false;
    chunked_t::pos_t p{0, 0};
    for (const auto& [k, v] : want) {
        if (p == m.end_pos() || m.at(p).key != k || m.at(p).value.value != v) return false;
        p = m.next(p);
    }
    return p == m.end_pos();
}

/**
 * @brief The chunked map (#1886) against a `std::map` oracle: inserts in a scrambled order split
 *        leaves, a refused insert at every request number changes nothing, range erases cross
 *        leaves and give emptied ones back, and every byte returns.
 */
void test_chunked_map() {
    constexpr int kKeys = 96;
    const auto scrambled = [](int i) { return (i * 37) % kKeys; };  // 37 is coprime to 96
    int inserts_requests = 0;
    for (int nth = 0; nth <= 24; ++nth) {
        refuse_nth_source_t src(nth);
        {
            chunked_t m(src);
            std::map<int, int> want;
            int refusals = 0;
            for (int i = 0; i < kKeys; ++i) {
                const int k = scrambled(i);
                auto r = m.try_emplace(k, k * 10);
                if (r.value == nullptr) {
                    ++refusals;
                    check(!r.inserted && same_as(m, want),
                          "chunked: a refused insert changes nothing");
                    r = m.try_emplace(k, k * 10);
                }
                check(r.inserted && r.value != nullptr && r.value->value == k * 10,
                      "chunked: insert adds the entry");
                want.emplace(k, k * 10);
            }
            // The refusal-free pass (nth == 0, first) counts the requests the inserts make.
            if (nth == 0) inserts_requests = src.calls_;
            check(refusals == (nth != 0 && nth <= inserts_requests ? 1 : 0),
                  "chunked: exactly the Nth request was refused");
            check(same_as(m, want) && g_live == kKeys, "chunked: every key, in key order");
            const auto again = m.try_emplace(5, -1);
            check(!again.inserted && again.value != nullptr && again.value->value == 50,
                  "chunked: a present key is not replaced");
            check(m.find(95) != nullptr && m.find(kKeys) == nullptr && m.find(-1) == nullptr,
                  "chunked: find at both ends and past them");
            // Erase every third key one at a time, then the odd keys of [20, 70) as one range.
            for (int k = 0; k < kKeys; k += 3) {
                check(m.erase(k), "chunked: erase a present key");
                want.erase(k);
            }
            check(!m.erase(0), "chunked: erasing an absent key answers false");
            const std::size_t gone =
                m.erase_if(m.lower_bound(20), m.lower_bound(70),
                           [](const chunked_t::entry_t& e) { return e.key % 2 != 0; });
            std::size_t odd = 0;
            for (auto it = want.lower_bound(20); it != want.lower_bound(70);)
                it = it->first % 2 != 0 ? (++odd, want.erase(it)) : std::next(it);
            check(gone == odd && same_as(m, want) && g_live == static_cast<int>(want.size()),
                  "chunked: a range erase across leaves keeps the rest in order");
            // Drain the whole map as one run: every leaf but the first is given back.
            check(m.erase_if(m.lower_bound(0), m.end_pos(),
                             [](const chunked_t::entry_t&) { return true; }) == want.size() &&
                      m.empty() && m.lower_bound(0) == m.end_pos(),
                  "chunked: draining every entry empties the map");
            check(src.blocks_out_ == 2, "chunked: a drained map keeps its index and one leaf");
            const int kept = src.calls_;
            check(m.try_emplace(1, 10).inserted && src.calls_ == kept && m.erase(1),
                  "chunked: the next insert into a drained map allocates nothing");
            check(m.try_emplace(7, 70).inserted && m.size() == 1, "chunked: reusable once empty");
        }
        check(g_live == 0, "chunked: every value destroyed");
        check(src.bytes_out_ == 0 && src.blocks_out_ == 0, "chunked: every byte returned");
    }
}

/** @brief A free function for the function-pointer form of `function_ref_t`. */
int twice(int x) { return 2 * x; }

/** @brief The synchronous-callback parameter shape `function_ref_t` serves. */
int apply(tr::function_ref_t<int(int)> f, int x) { return f(x); }

/** @brief `function_ref_t`: binds lambdas, temporaries, functions; two words; never allocates. */
void test_function_ref() {
    static_assert(std::is_trivially_copyable_v<tr::function_ref_t<int(int)>>);
    static_assert(sizeof(tr::function_ref_t<int(int)>) == 2 * sizeof(void*));
    static_assert(!std::is_default_constructible_v<tr::function_ref_t<int(int)>>);
    int base = 10;
    check(apply([&](int x) { return x + base; }, 5) == 15, "fref: a capturing temporary");
    check(apply(twice, 4) == 8 && apply(&twice, 5) == 10, "fref: a function and its pointer");
    int calls = 0;
    auto counter = [&calls](int x) mutable {
        ++calls;
        return x;
    };
    tr::function_ref_t<int(int)> r = counter;
    tr::function_ref_t<int(int)> copy = r;
    check(r(1) == 1 && copy(2) == 2 && calls == 2, "fref: a copy refers to the same callable");
    const auto konst = [](int x) { return -x; };
    check(apply(konst, 3) == -3, "fref: a const callable");
    tr::function_ref_t<void(int&)> bump = [](int& v) { ++v; };
    int v = 0;
    bump(v);
    check(v == 1, "fref: void return and a reference parameter");
    tr::function_ref_t<long(int)> widen = twice;
    check(widen(21) == 42, "fref: the result converts to R");
}

/** @brief A base with no virtual destructor, like the seam bases (#2022). */
class poly_base_t {
   public:
    /** @brief The one virtual, so the class is polymorphic but has no deleting destructor. */
    [[nodiscard]] virtual int id() const noexcept = 0;

   protected:
    /** @brief Destroyed only as the derived class it is. */
    ~poly_base_t() = default;
};
static_assert(!std::has_virtual_destructor_v<poly_base_t>);

/** @brief A first base that puts `poly_base_t` at a non-zero offset in `poly_derived_t`. */
struct poly_pad_t {
    long pad[3] = {}; /**< @brief Room before the second base. */
};

/** @brief Derived through a second base; counts its lives in `g_live`. */
class poly_derived_t final : public poly_pad_t, public poly_base_t {
   public:
    /** @brief Hold @p v. */
    explicit poly_derived_t(int v) noexcept : v_(v) { ++g_live; }
    /** @brief One life ends. */
    ~poly_derived_t() { --g_live; }
    /** @brief The held value. */
    [[nodiscard]] int id() const noexcept override { return v_; }

   private:
    int v_; /**< @brief The payload. */
};

/** @brief `poly_ptr_t`: owned through a base with a protected, non-virtual destructor, the
 *         object is destroyed as the class `make_poly` built and its exact block returns. */
void test_poly_ptr() {
    g_live = 0;
    refuse_nth_source_t src(0);
    {
        tr::mem::poly_ptr_t<poly_base_t> p = tr::mem::make_poly<poly_derived_t>(src, 7);
        check(p && p->id() == 7 && g_live == 1, "poly: built and converted to the base");
        tr::mem::poly_ptr_t<poly_base_t> q = std::move(p);
        check(!p && q->id() == 7 && g_live == 1, "poly: a move transfers the object");
        check(src.blocks_out_ == 1 && src.bytes_out_ == sizeof(poly_derived_t),
              "poly: one block of the derived size");
    }
    check(g_live == 0, "poly: the derived destructor ran through the base owner");
    check(src.blocks_out_ == 0 && src.bytes_out_ == 0, "poly: the exact block came back");
    refuse_nth_source_t refusing(1);
    tr::mem::poly_ptr_t<poly_base_t> r = tr::mem::make_poly<poly_derived_t>(refusing, 1);
    check(!r && g_live == 0, "poly: a refusal is an empty owner and builds nothing");
}

}  // namespace

/** @brief Run every section; the exit code is the failure count. */
int main() {
    test_array_trivial();
    test_array_nontrivial();
    test_string();
    test_sorted_map();
    test_chunked_map();
    test_function_ref();
    test_poly_ptr();
    return tr::testing::summary("core_containers");
}
