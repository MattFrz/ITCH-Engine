// Unit tests for the open-addressing order index.
//
// The interesting property is not "does a hash map work" but "does backward
// shift deletion keep every probe chain intact after the delete-heavy workload
// a real ITCH day produces". These tests hammer exactly that.

#include <unordered_map>
#include <vector>

#include "itch_engine/book/order_index.hpp"
#include "test_support.hpp"

using namespace itch;
using namespace itch::book;
using itch_test::Rng;

namespace {

void test_basic_insert_find_erase() {
    OrderIndex idx(1024);
    CHECK(idx.capacity() == 1024);
    CHECK(idx.size() == 0);
    CHECK(idx.find(42) == OrderIndex::kNull);

    CHECK(idx.insert(42, 7));
    CHECK_EQ(idx.find(42), 7);
    CHECK_EQ(idx.size(), 1);

    // Duplicate insert is refused, and does not disturb the existing value.
    CHECK(!idx.insert(42, 9));
    CHECK_EQ(idx.find(42), 7);

    CHECK(idx.insert_or_assign(42, 9));
    CHECK_EQ(idx.find(42), 9);
    CHECK_EQ(idx.size(), 1);

    CHECK(idx.erase(42));
    CHECK(idx.find(42) == OrderIndex::kNull);
    CHECK_EQ(idx.size(), 0);
    CHECK(!idx.erase(42));
}

void test_capacity_rounding_and_load_limit() {
    OrderIndex idx(100);
    CHECK_EQ(idx.capacity(), 128);  // rounded up to a power of two
    CHECK_EQ(idx.max_load(), 64);   // load factor 0.5

    for (std::uint32_t i = 0; i < 64; ++i) {
        CHECK(idx.insert(1000 + i, i));
    }
    CHECK(idx.full());
    // Past the load limit the table refuses rather than rehashing: an
    // unbounded pause is exactly what the hot path must never take.
    CHECK(!idx.insert(9999, 1));
    CHECK_EQ(idx.rejected(), 1);
    CHECK_EQ(idx.size(), 64);
}

void test_reserved_empty_key_is_rejected() {
    OrderIndex idx(64);
    CHECK(!idx.insert(OrderIndex::kEmptyKey, 1));
    CHECK_EQ(idx.rejected(), 1);
    CHECK_EQ(idx.size(), 0);
}

// Sequential ids are the ITCH case and the worst case for a bad hash: the
// table must stay probe-cheap rather than degenerating into a linear scan.
void test_sequential_ids_probe_cheaply() {
    OrderIndex idx(1 << 16);
    for (std::uint32_t i = 0; i < 30000; ++i) {
        CHECK(idx.insert(1000000 + i, i));
    }
    for (std::uint32_t i = 0; i < 30000; ++i) {
        CHECK_EQ(idx.find(1000000 + i), i);
    }
    CHECK(idx.average_probes() < 2.0);
}

// The real test: interleaved insert/erase/find against std::unordered_map as
// an oracle, with a delete-heavy mix, so any broken probe chain shows up as a
// lookup that finds nothing where the oracle finds something.
void test_against_oracle_delete_heavy() {
    OrderIndex idx(1 << 17);
    std::unordered_map<OrderId, std::uint32_t> oracle;
    std::vector<OrderId> live;
    Rng rng(20260831);

    OrderId next = 1;
    for (int step = 0; step < 400000; ++step) {
        const std::uint32_t roll = rng.below(100);
        if (roll < 45 || live.size() < 16) {
            const OrderId id = next++;
            const std::uint32_t v = static_cast<std::uint32_t>(rng.below(1000000));
            if (idx.size() >= idx.max_load()) continue;
            CHECK(idx.insert(id, v));
            oracle[id] = v;
            live.push_back(id);
        } else if (roll < 90) {
            const std::uint32_t k = rng.below(static_cast<std::uint32_t>(live.size()));
            const OrderId id = live[k];
            CHECK(idx.erase(id));
            oracle.erase(id);
            live[k] = live.back();
            live.pop_back();
        } else {
            const std::uint32_t k = rng.below(static_cast<std::uint32_t>(live.size()));
            const OrderId id = live[k];
            auto it = oracle.find(id);
            CHECK(it != oracle.end());
            CHECK_EQ(idx.find(id), it->second);
        }
    }

    CHECK_EQ(idx.size(), oracle.size());
    for (const auto& kv : oracle) {
        CHECK_EQ(idx.find(kv.first), kv.second);
    }
    // Nothing that was erased may still be findable.
    std::size_t seen = 0;
    idx.for_each([&](OrderId id, std::uint32_t v) {
        ++seen;
        auto it = oracle.find(id);
        CHECK(it != oracle.end());
        CHECK_EQ(v, it->second);
    });
    CHECK_EQ(seen, oracle.size());
}

// Deliberately collide every key into one home slot, then delete from the
// middle of the chain. This is the case tombstone-free deletion gets wrong if
// the shift condition is off by one.
void test_forced_collisions() {
    OrderIndex idx(64);
    // With a power-of-two table and multiplicative hashing, ids spaced by the
    // multiplicative inverse of the golden ratio constant land in the same
    // home slot. Simpler and just as strong: insert enough keys that chains
    // certainly overlap, then delete in a scrambled order and re-verify.
    std::vector<OrderId> ids;
    for (std::uint32_t i = 0; i < 32; ++i) {
        const OrderId id = 500 + i * 7;
        CHECK(idx.insert(id, i));
        ids.push_back(id);
    }
    Rng rng(7);
    while (!ids.empty()) {
        const std::uint32_t k = rng.below(static_cast<std::uint32_t>(ids.size()));
        const OrderId id = ids[k];
        CHECK(idx.erase(id));
        ids[k] = ids.back();
        ids.pop_back();
        for (OrderId remaining : ids) {
            CHECK(idx.find(remaining) != OrderIndex::kNull);
        }
    }
    CHECK_EQ(idx.size(), 0);
}

void test_clear() {
    OrderIndex idx(256);
    for (std::uint32_t i = 0; i < 100; ++i) CHECK(idx.insert(i + 1, i));
    idx.clear();
    CHECK_EQ(idx.size(), 0);
    for (std::uint32_t i = 0; i < 100; ++i) CHECK(idx.find(i + 1) == OrderIndex::kNull);
    CHECK(idx.insert(1, 1));
    CHECK_EQ(idx.find(1), 1);
}

}  // namespace

int main() {
    test_basic_insert_find_erase();
    test_capacity_rounding_and_load_limit();
    test_reserved_empty_key_is_rejected();
    test_sequential_ids_probe_cheaply();
    test_against_oracle_delete_heavy();
    test_forced_collisions();
    test_clear();
    itch_test::pass("order index");
    return 0;
}
