/**
 * EduRoute Comprehensive Unit Test Suite
 * Covers Core Primitives, Division 2 Anti-Cheat, Division 3 Adaptive Rank, and C ABI.
 * STRICTLY NO STL CONTAINERS (No std::vector, std::string, std::map).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "core/types.hpp"
#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"
#include "core/dsu.hpp"
#include "core/string_view.hpp"
#include "core/bit_stream.hpp"

#include "anticheat/kmer_winnow.hpp"
#include "anticheat/anomaly_stats.hpp"
#include "anticheat/collusion_graph.hpp"

#include "adaptive_rank/fenwick_score.hpp"
#include "adaptive_rank/order_stat_avl.hpp"
#include "adaptive_rank/irt_selector.hpp"

#include "eduroute_c_api.h"

// -----------------------------------------------------------------------------
// Minimalist Test Harness (No STL)
// -----------------------------------------------------------------------------
static int g_total_assertions = 0;
static int g_failed_assertions = 0;
static int g_suites_run = 0;

#define TEST_ASSERT(cond, msg)                                                                     \
    do {                                                                                           \
        ++g_total_assertions;                                                                      \
        if (!(cond)) {                                                                             \
            std::printf("  [FAIL] %s:%d: Assertion failed: (%s) - %s\n", __FILE__, __LINE__, #cond, msg); \
            ++g_failed_assertions;                                                                 \
        }                                                                                          \
    } while (0)

#define TEST_SUITE_START(name)                                                                     \
    do {                                                                                           \
        ++g_suites_run;                                                                             \
        std::printf("[RUNNING TEST SUITE] %s\n", name);                                            \
    } while (0)

// -----------------------------------------------------------------------------
// Suite 1: Core Primitives
// -----------------------------------------------------------------------------

static void test_core_arena () {
    TEST_SUITE_START("Core Arena Allocator");

    constexpr core::usize ARENA_SIZE = 4096;
    core::Arena arena(ARENA_SIZE);

    TEST_ASSERT(arena.capacity() == ARENA_SIZE, "Arena capacity matches request");
    TEST_ASSERT(arena.used() == 0, "Initial arena usage is 0");

    // Basic allocation and alignment
    void *p1 = arena.allocate(100, 8);
    TEST_ASSERT(p1 != nullptr, "p1 allocated");
    TEST_ASSERT((reinterpret_cast<std::uintptr_t>(p1) % 8) == 0, "p1 is 8-byte aligned");
    TEST_ASSERT(arena.used() >= 100, "Used bytes increased");

    // 64-byte alignment
    void *p2 = arena.allocate(50, 64);
    TEST_ASSERT(p2 != nullptr, "p2 allocated");
    TEST_ASSERT((reinterpret_cast<std::uintptr_t>(p2) % 64) == 0, "p2 is 64-byte aligned");

    // Create trivial struct
    struct Point { int x; int y; float z; };
    Point *pt = arena.create<Point>(10, 20, 30.5f);
    TEST_ASSERT(pt != nullptr, "Point constructed in-place");
    TEST_ASSERT(pt->x == 10 && pt->y == 20 && pt->z == 30.5f, "Point values preserved");

    // Marker and rollback
    auto marker = arena.get_marker();
    core::usize used_at_mark = arena.used();

    void *temp = arena.allocate(500, 8);
    TEST_ASSERT(temp != nullptr, "temp allocated");
    TEST_ASSERT(arena.used() > used_at_mark, "usage grew after temp");

    arena.reset_to_marker(marker);
    TEST_ASSERT(arena.used() == used_at_mark, "arena usage rolled back to marker");

    // ScopedArena RAII test
    {
        core::ScopedArena scoped(arena);
        void *scoped_mem = scoped.get().allocate(200, 8);
        TEST_ASSERT(scoped_mem != nullptr, "scoped_mem allocated");
        TEST_ASSERT(arena.used() > used_at_mark, "arena used grew inside scope");
    }
    TEST_ASSERT(arena.used() == used_at_mark, "ScopedArena automatically rolled back memory on exit");

    // Exhaustion test
    void *huge = arena.allocate(ARENA_SIZE * 2, 8);
    TEST_ASSERT(huge == nullptr, "OOM allocation returns nullptr");

    // Full reset
    arena.reset();
    TEST_ASSERT(arena.used() == 0, "arena.reset() restores usage to 0");
}

static void test_core_dyn_array () {
    TEST_SUITE_START("Core Array<T>");

    core::Arena arena(8192);
    core::Array<int> arr(arena, 4);

    TEST_ASSERT(arr.empty(), "New array is empty");
    TEST_ASSERT(arr.size() == 0, "Size is 0");
    TEST_ASSERT(arr.capacity() >= 4, "Reserved capacity is at least 4");

    for (int i = 0; i < 20; ++i) {
        arr.push_back(i * 10);
    }

    TEST_ASSERT(arr.size() == 20, "Pushed 20 elements");
    TEST_ASSERT(arr[0] == 0, "First element is 0");
    TEST_ASSERT(arr[19] == 190, "Last element is 190");

    arr.pop_back();
    TEST_ASSERT(arr.size() == 19, "pop_back reduced size to 19");
    TEST_ASSERT(arr[arr.size() - 1] == 180, "New last element is 180");

    // Range-based for loop iteration
    int sum = 0;
    for (int v : arr) {
        sum += v;
    }
    TEST_ASSERT(sum == (19 * 180) / 2, "Iterator sum over elements is correct");

    // Emplace back struct
    struct Item { int id; float weight; };
    core::Array<Item> items(arena);
    items.emplace_back(1, 4.5f);
    items.emplace_back(2, 9.2f);
    TEST_ASSERT(items.size() == 2, "Emplaced 2 items");
    TEST_ASSERT(items[0].id == 1 && items[0].weight == 4.5f, "Item 0 matches");
    TEST_ASSERT(items[1].id == 2 && items[1].weight == 9.2f, "Item 1 matches");

    items.clear();
    TEST_ASSERT(items.empty(), "clear() sets size to 0");
}

static void test_core_hash_map () {
    TEST_SUITE_START("Core HashMap<K, V>");

    core::Arena arena(32768);
    core::HashMap<core::u32, core::u32> map(arena, 16);

    TEST_ASSERT(map.empty(), "New map is empty");
    TEST_ASSERT(map.size() == 0, "Size is 0");

    // Insert and find
    map.insert(100, 500);
    map.insert(200, 600);
    map.insert(300, 700);

    TEST_ASSERT(map.size() == 3, "Size is 3");
    core::u32 *v1 = map.find(100);
    core::u32 *v2 = map.find(200);
    core::u32 *v3 = map.find(300);
    core::u32 *v4 = map.find(999);

    TEST_ASSERT(v1 != nullptr && *v1 == 500, "Key 100 found with value 500");
    TEST_ASSERT(v2 != nullptr && *v2 == 600, "Key 200 found with value 600");
    TEST_ASSERT(v3 != nullptr && *v3 == 700, "Key 300 found with value 700");
    TEST_ASSERT(v4 == nullptr, "Key 999 not found");

    // Update existing key
    map.insert(100, 555);
    TEST_ASSERT(map.size() == 3, "Size remains 3 after update");
    TEST_ASSERT(*map.find(100) == 555, "Value for key 100 successfully updated to 555");

    // Rehashing test: insert 150 elements
    for (core::u32 i = 1000; i < 1150; ++i) {
        map.insert(i, i * 2);
    }
    TEST_ASSERT(map.size() == 153, "Map rehashed and grew to 153 elements");

    // Verify all keys remain intact after rehash
    bool all_found = true;
    for (core::u32 i = 1000; i < 1150; ++i) {
        core::u32 *val = map.find(i);
        if (!val || *val != i * 2) {
            all_found = false;
            break;
        }
    }
    TEST_ASSERT(all_found, "All 150 elements found with correct values after rehash");
}

static void test_core_dsu () {
    TEST_SUITE_START("Core DisjointSet (DSU)");

    core::Arena arena(4096);
    core::DisjointSet dsu(arena, 10);

    TEST_ASSERT(dsu.size() == 10, "DSU size is 10");
    TEST_ASSERT(dsu.num_components() == 10, "Initial components is 10");

    for (core::u32 i = 0; i < 10; ++i) {
        TEST_ASSERT(dsu.component_size(i) == 1, "Initial component size is 1");
    }

    // Join 1 and 2
    TEST_ASSERT(dsu.join(1, 2) == true, "Join 1 and 2 succeeded");
    TEST_ASSERT(dsu.connected(1, 2) == true, "1 and 2 are connected");
    TEST_ASSERT(dsu.num_components() == 9, "Components reduced to 9");
    TEST_ASSERT(dsu.component_size(1) == 2, "Component size is now 2");

    // Redundant join
    TEST_ASSERT(dsu.join(1, 2) == false, "Redundant join returns false");

    // Join 3 and 4, then join 2 and 3 -> cluster {1, 2, 3, 4}
    dsu.join(3, 4);
    dsu.join(2, 3);
    TEST_ASSERT(dsu.connected(1, 4) == true, "1 and 4 are transitively connected");
    TEST_ASSERT(dsu.component_size(1) == 4, "Cluster size is 4");
    TEST_ASSERT(dsu.num_components() == 7, "Total components is 7");
    TEST_ASSERT(dsu.connected(1, 5) == false, "1 and 5 are not connected");
}

static void test_core_string_view () {
    TEST_SUITE_START("Core StringView");

    core::StringView sv("Hello EduRoute World");
    TEST_ASSERT(sv.size() == 20, "StringView length matches");
    TEST_ASSERT(!sv.empty(), "StringView is not empty");
    TEST_ASSERT(sv[0] == 'H' && sv[6] == 'E', "Index access correct");

    core::StringView sub = sv.substr(6, 8);
    TEST_ASSERT(sub == core::StringView("EduRoute"), "substr(6, 8) equals EduRoute");

    core::StringView mod = sv;
    mod.remove_prefix(6);
    TEST_ASSERT(mod.substr(0, 8) == core::StringView("EduRoute"), "remove_prefix works");

    mod.remove_suffix(6);
    TEST_ASSERT(mod == core::StringView("EduRoute"), "remove_suffix works");

    // Hash consistency
    core::Hash64 h1 = core::StringView("TestString").hash();
    core::Hash64 h2 = core::StringView("TestString").hash();
    core::Hash64 h3 = core::StringView("DifferentString").hash();
    TEST_ASSERT(h1 == h2, "Identical strings produce identical FNV-1a hash");
    TEST_ASSERT(h1 != h3, "Different strings produce different hashes");
}

static void test_core_bit_stream () {
    TEST_SUITE_START("Core BitWriter & BitReader");

    core::Arena arena(1024);
    core::BitWriter writer(arena, 64);

    // Write sequence of bits
    writer.write_bit(true);       // 1
    writer.write_bit(false);      // 0
    writer.write_bit(true);       // 1
    writer.write_bits(0x0Au, 4);  // 1010
    writer.write_bits(0x3Fu, 6);  // 111111
    writer.flush();

    TEST_ASSERT(writer.byte_size() > 0, "Writer generated byte output");

    core::BitReader reader(writer.data(), writer.byte_size());
    TEST_ASSERT(reader.read_bit() == true, "Bit 1 matches");
    TEST_ASSERT(reader.read_bit() == false, "Bit 2 matches");
    TEST_ASSERT(reader.read_bit() == true, "Bit 3 matches");
    TEST_ASSERT(reader.read_bits(4) == 0x0Au, "4-bit value 0x0A matches");
    TEST_ASSERT(reader.read_bits(6) == 0x3Fu, "6-bit value 0x3F matches");
}

// -----------------------------------------------------------------------------
// Suite 2: Division 2 Anti-Cheat
// -----------------------------------------------------------------------------

static void test_anticheat_kmer_winnow () {
    TEST_SUITE_START("Div 2 KmerWinnow Fingerprinting");

    core::Arena pers_arena(1024 * 1024);
    core::Arena scratch_arena(1024 * 1024);
    core::anticheat::KmerWinnow winnow(pers_arena);

    core::Array<core::Hash64> fps1(scratch_arena, 64);
    core::Array<core::Hash64> fps2(scratch_arena, 64);

    // Test text < KMER_LEN (5)
    core::anticheat::KmerWinnow::extract_fingerprints(core::StringView("Ab1"), scratch_arena, fps1);
    TEST_ASSERT(fps1.empty(), "Text shorter than KMER_LEN produces 0 fingerprints");

    // Test identical text produces identical fingerprints
    core::StringView textA("Balanced binary search trees maintain O(log n) height.");
    core::StringView textB("balanced BINARY search trees maintain o(log n) height!!");

    fps1.clear();
    fps2.clear();
    core::anticheat::KmerWinnow::extract_fingerprints(textA, scratch_arena, fps1);
    core::anticheat::KmerWinnow::extract_fingerprints(textB, scratch_arena, fps2);

    TEST_ASSERT(!fps1.empty(), "textA extracted fingerprints");
    TEST_ASSERT(fps1.size() == fps2.size(), "Fingerprint counts match despite case/punctuation differences");

    bool identical = true;
    for (core::usize i = 0; i < fps1.size(); ++i) {
        if (fps1[i] != fps2[i]) { identical = false; break; }
    }
    TEST_ASSERT(identical, "Fingerprints match exactly after normalization");

    // Test matching via index_and_match
    core::Array<core::anticheat::TextSimMatch> matches(scratch_arena, 16);

    // Index Student 1
    winnow.index_and_match(1, 10, fps1, scratch_arena, matches);
    TEST_ASSERT(matches.empty(), "First student has no prior matches");

    // Student 2 submits the identical text for the same question
    matches.clear();
    winnow.index_and_match(2, 10, fps2, scratch_arena, matches);
    TEST_ASSERT(!matches.empty(), "Student 2 matched with Student 1");
    TEST_ASSERT(matches[0].other_student == 1, "Matched other_student is 1");
    TEST_ASSERT(matches[0].cont_score >= 0.95f, "Containment score is ~1.0 for identical text");

    // Student 3 submits completely different text
    core::StringView textC("Quantum computing utilizes superposition and entanglement of qubits.");
    core::Array<core::Hash64> fps3(scratch_arena, 64);
    core::anticheat::KmerWinnow::extract_fingerprints(textC, scratch_arena, fps3);

    matches.clear();
    winnow.index_and_match(3, 10, fps3, scratch_arena, matches);
    TEST_ASSERT(matches.empty(), "Completely different text produces 0 collisions");
}

static void test_anticheat_anomaly_stats () {
    TEST_SUITE_START("Div 2 AnomalyStats (Time-Sync & Rare Errors)");

    core::Arena arena(1024 * 1024);
    core::Arena scratch(1024 * 1024);
    core::anticheat::AnomalyStats stats;
    core::StatusCode st = stats.init(arena, 512);
    TEST_ASSERT(st == core::StatusCode::Ok, "AnomalyStats init succeeded");
    TEST_ASSERT(stats.ready(), "AnomalyStats is ready");

    // ---- 1. Time-sync spike detection (|Δt_A - Δt_B| <= 500 ms) ----
    core::Array<core::anticheat::StatAnomalyMatch> matches(scratch, 16);

    // Student 1 submits dt = 10000 ms on Q1
    stats.record_dt(1, 1, 10000, scratch, matches);
    TEST_ASSERT(matches.empty(), "First student has no time-sync peers");

    // Student 2 submits dt = 10150 ms on Q1 (diff = 150 ms <= 500 ms)
    matches.clear();
    stats.record_dt(2, 1, 10150, scratch, matches);
    TEST_ASSERT(!matches.empty(), "Time-sync spike detected between Student 2 and Student 1");
    TEST_ASSERT(matches[0].other_student == 1, "Peer is Student 1");
    TEST_ASSERT(matches[0].time_score >= 1.0f, "Time score incremented");

    // Student 3 submits dt = 15000 ms on Q1 (diff = 5000 ms > 500 ms)
    matches.clear();
    stats.record_dt(3, 1, 15000, scratch, matches);
    TEST_ASSERT(matches.empty(), "Student 3 with far dt triggers no time-sync spike");

    // ---- 2. Shared rare error detection ----
    // Question 2: Correct answer is option 0. Option 3 is chosen by only rare students.
    // Populate with 35 normal students choosing option 0 (total submissions > 35 ensures 2/37 < 10%)
    for (core::StudentId s = 10; s < 45; ++s) {
        matches.clear();
        stats.record_mcq(s, 2, 0, 0, scratch, matches); // correct
    }

    // Student 4 chooses wrong option 3 (rare: 1/16 < 10%)
    matches.clear();
    stats.record_mcq(4, 2, 3, 0, scratch, matches);
    TEST_ASSERT(matches.empty(), "First student choosing rare error has no peer match yet");

    // Student 5 also chooses wrong option 3 -> should trigger rare error hit with Student 4!
    matches.clear();
    stats.record_mcq(5, 2, 3, 0, scratch, matches);
    TEST_ASSERT(!matches.empty(), "Shared rare error detected between Student 5 and Student 4");
    TEST_ASSERT(matches[0].other_student == 4, "Matched other student is Student 4");
    TEST_ASSERT(matches[0].err_score >= 2.0f, "Err score incremented");

    // Pair score symmetry
    core::f32 t_score = 0.0f, e_score = 0.0f;
    TEST_ASSERT(stats.get_pair_scores(4, 5, &t_score, &e_score) == core::StatusCode::Ok, "Query (4, 5) succeeded");
    core::f32 t_score2 = 0.0f, e_score2 = 0.0f;
    TEST_ASSERT(stats.get_pair_scores(5, 4, &t_score2, &e_score2) == core::StatusCode::Ok, "Query (5, 4) succeeded");
    TEST_ASSERT(t_score == t_score2 && e_score == e_score2, "Pair scores are symmetric (A,B == B,A)");
}

static void test_anticheat_collusion_graph () {
    TEST_SUITE_START("Div 2 CollusionGraph & DSU Cheating Clusters");

    core::Arena arena(1024 * 1024);
    core::anticheat::CollusionGraph graph;
    TEST_ASSERT(graph.init(arena, 100, 256) == core::StatusCode::Ok, "CollusionGraph init succeeded");

    bool flagged = false;

    // Initially unflagged
    TEST_ASSERT(!graph.is_flagged(1), "Student 1 initially not flagged");
    TEST_ASSERT(!graph.is_flagged(2), "Student 2 initially not flagged");

    // Add mild time-sync suspicion (weight = 1.0 * 1.0 = 1.0 < 3.0 threshold)
    graph.update_stat(1, 2, 1.0f, 0.0f, flagged);
    TEST_ASSERT(!flagged, "W=1.0 is below COLLUSION_THRESHOLD (3.0)");
    TEST_ASSERT(!graph.is_flagged(1), "Student 1 still not in cheating cluster");

    // Add high text similarity (W = 1.0 + 4.0 * 0.9 = 4.6 >= 3.0 threshold!)
    graph.update_text(1, 2, 0.9f, flagged);
    TEST_ASSERT(flagged, "Combined weight crossed 3.0 -> flagged confirmed colluders");
    TEST_ASSERT(graph.is_flagged(1), "Student 1 is now flagged in DSU");
    TEST_ASSERT(graph.is_flagged(2), "Student 2 is now flagged in DSU");
    TEST_ASSERT(graph.cluster_root(1) == graph.cluster_root(2), "1 and 2 share the same DSU cluster root");
    TEST_ASSERT(graph.cluster_size(1) == 2, "Cluster size is 2");

    // Transitive ring formation: Student 2 colludes with Student 3
    bool flagged2 = false;
    graph.update_text(2, 3, 1.0f, flagged2); // W = 4.0 >= 3.0
    TEST_ASSERT(flagged2, "2 and 3 confirmed colluders");
    TEST_ASSERT(graph.cluster_root(1) == graph.cluster_root(3), "1, 2, and 3 are in the same DSU cluster");
    TEST_ASSERT(graph.cluster_size(1) == 3, "Cluster size expanded to 3 students");

    // Verify edge retrieval
    core::anticheat::CollusionEdge edge{};
    TEST_ASSERT(graph.get_edge(1, 2, &edge), "Edge (1, 2) retrieved");
    TEST_ASSERT(edge.combined_weight >= 4.0f, "Combined weight accurately recorded");
    TEST_ASSERT(edge.peak_text_sim == 0.9f, "Peak text similarity matches");

    // Verify confirmed edges export
    core::anticheat::CollusionEdge confirmed[10];
    core::usize edge_count = graph.get_confirmed_edges(confirmed, 10);
    TEST_ASSERT(edge_count == 2, "2 confirmed collusion edges in graph");
}

// -----------------------------------------------------------------------------
// Suite 3: Division 3 Adaptive Rank
// -----------------------------------------------------------------------------

static void test_rank_fenwick_score () {
    TEST_SUITE_START("Div 3 FenwickScore Percentile Engine");

    core::Arena arena(1024 * 1024);
    adaptive_rank::FenwickScore fenwick;
    TEST_ASSERT(fenwick.init(arena) == core::StatusCode::Ok, "FenwickScore init succeeded");

    TEST_ASSERT(fenwick.total() == 0, "Initial total is 0");

    // Add scores for students
    TEST_ASSERT(fenwick.add(2000) == core::StatusCode::Ok, "add 2000 ok");
    TEST_ASSERT(fenwick.add(5000) == core::StatusCode::Ok, "add 5000 ok");
    TEST_ASSERT(fenwick.add(5000) == core::StatusCode::Ok, "add 5000 tie ok");
    TEST_ASSERT(fenwick.add(8000) == core::StatusCode::Ok, "add 8000 ok");
    TEST_ASSERT(fenwick.add(9000) == core::StatusCode::Ok, "add 9000 ok");

    TEST_ASSERT(fenwick.total() == 5, "Total count is 5");
    TEST_ASSERT(fenwick.count_at(5000) == 2, "Exact count at 5000 is 2");
    TEST_ASSERT(fenwick.count_less(5000) == 1, "Count strictly less than 5000 is 1 (2000)");
    TEST_ASSERT(fenwick.count_greater(5000) == 2, "Count strictly greater than 5000 is 2 (8000, 9000)");
    TEST_ASSERT(fenwick.count_in_range(4000, 8500) == 3, "Range [4000, 8500] contains 3 scores (5000, 5000, 8000)");

    // Ranks (1-based, higher score = better rank)
    TEST_ASSERT(fenwick.rank(9000) == 1, "Score 9000 has rank 1 (best)");
    TEST_ASSERT(fenwick.rank(8000) == 2, "Score 8000 has rank 2");
    TEST_ASSERT(fenwick.rank(5000) == 3, "Score 5000 has rank 3");
    TEST_ASSERT(fenwick.rank(2000) == 5, "Score 2000 has rank 5");

    // Percentiles: (below + 0.5 * tied) / total * 100
    // For 5000: (1 + 0.5 * 2) / 5 * 100 = 40.0%
    core::f64 pct = fenwick.percentile(5000);
    TEST_ASSERT(std::fabs(pct - 40.0) < 1e-4, "Percentile for score 5000 is exactly 40%");

    // Binary lifting search: score_at_rank
    core::i32 score_out = 0;
    TEST_ASSERT(fenwick.score_at_rank(1, &score_out) == core::StatusCode::Ok, "score_at_rank(1) ok");
    TEST_ASSERT(score_out == 9000, "Rank 1 is score 9000");

    TEST_ASSERT(fenwick.score_at_rank(2, &score_out) == core::StatusCode::Ok, "score_at_rank(2) ok");
    TEST_ASSERT(score_out == 8000, "Rank 2 is score 8000");

    // Update score: 2000 -> 9500 (student improves)
    TEST_ASSERT(fenwick.update(2000, 9500) == core::StatusCode::Ok, "update(2000, 9500) ok");
    TEST_ASSERT(fenwick.total() == 5, "Total count remains 5 after update");
    TEST_ASSERT(fenwick.rank(9500) == 1, "Updated score 9500 is now rank 1");
    TEST_ASSERT(fenwick.rank(9000) == 2, "Former top score 9000 is now rank 2");

    // Clamping: negative scores and scores > 10000 do not crash
    TEST_ASSERT(fenwick.add(-100) == core::StatusCode::Ok, "Negative score clamped to bucket 0");
    TEST_ASSERT(fenwick.add(999999) == core::StatusCode::Ok, "Excessive score clamped to max bucket");
}

static void test_rank_order_stat_avl () {
    TEST_SUITE_START("Div 3 OrderStatAvl Tie-Broken Leaderboard");

    core::Arena arena(65536);
    core::adaptive_rank::OrderStatAvl avl;
    TEST_ASSERT(avl.init(arena, 64) == core::StatusCode::Ok, "OrderStatAvl init succeeded");

    TEST_ASSERT(avl.size() == 0, "Tree is empty");
    TEST_ASSERT(avl.validate(), "Empty tree validates invariants");

    // Insert keys:
    // Key ordering: higher score > lower score; lower time > higher time; lower sid > higher sid
    core::LeaderboardKey k1{8000, 30000, 1}; // Score 8000, Time 30s, Student 1
    core::LeaderboardKey k2{8000, 20000, 2}; // Score 8000, Time 20s (beats k1 on time!), Student 2
    core::LeaderboardKey k3{9000, 40000, 3}; // Score 9000 (highest score!), Student 3
    core::LeaderboardKey k4{5000, 10000, 4}; // Score 5000 (lowest score), Student 4

    TEST_ASSERT(avl.insert(k1) == core::StatusCode::Ok, "insert k1 ok");
    TEST_ASSERT(avl.insert(k2) == core::StatusCode::Ok, "insert k2 ok");
    TEST_ASSERT(avl.insert(k3) == core::StatusCode::Ok, "insert k3 ok");
    TEST_ASSERT(avl.insert(k4) == core::StatusCode::Ok, "insert k4 ok");

    TEST_ASSERT(avl.size() == 4, "4 keys inserted");
    TEST_ASSERT(avl.validate(), "AVL invariants hold after 4 insertions");

    // Ranks:
    // 1st: k3 (Score 9000)
    // 2nd: k2 (Score 8000, Time 20s)
    // 3rd: k1 (Score 8000, Time 30s)
    // 4th: k4 (Score 5000)
    core::u32 rank = 0;
    avl.rank_of(k3, &rank);
    TEST_ASSERT(rank == 1, "k3 (9000) is Rank 1");
    avl.rank_of(k2, &rank);
    TEST_ASSERT(rank == 2, "k2 (8000, 20s) is Rank 2 (tie-broken by faster time)");
    avl.rank_of(k1, &rank);
    TEST_ASSERT(rank == 3, "k1 (8000, 30s) is Rank 3");
    avl.rank_of(k4, &rank);
    TEST_ASSERT(rank == 4, "k4 (5000) is Rank 4");

    // Select (k-th element)
    core::LeaderboardKey selected{};
    TEST_ASSERT(avl.select(1, &selected) == core::StatusCode::Ok, "select(1) ok");
    TEST_ASSERT(selected == k3, "select(1) retrieved k3");
    TEST_ASSERT(avl.select(2, &selected) == core::StatusCode::Ok, "select(2) ok");
    TEST_ASSERT(selected == k2, "select(2) retrieved k2");

    // Top K array
    core::LeaderboardKey top[4];
    core::u32 written = avl.top_k(3, top);
    TEST_ASSERT(written == 3, "top_k wrote 3 items");
    TEST_ASSERT(top[0] == k3 && top[1] == k2 && top[2] == k1, "top_k sorted order verified");

    // Update k4 to beat k3 (k4 gets score 9500)
    core::LeaderboardKey k4_new{9500, 15000, 4};
    TEST_ASSERT(avl.update(k4, k4_new) == core::StatusCode::Ok, "update key succeeded");
    TEST_ASSERT(avl.validate(), "AVL invariants hold after update");
    avl.rank_of(k4_new, &rank);
    TEST_ASSERT(rank == 1, "k4_new is now Rank 1");

    // Erase
    TEST_ASSERT(avl.erase(k1) == core::StatusCode::Ok, "erase k1 succeeded");
    TEST_ASSERT(avl.size() == 3, "Size reduced to 3");
    TEST_ASSERT(!avl.contains(k1), "k1 no longer in tree");
    TEST_ASSERT(avl.validate(), "AVL invariants hold after erase");
}

static void test_rank_irt_selector () {
    TEST_SUITE_START("Div 3 IRT QuestionBank & Adaptive Session");

    core::Arena arena(1024 * 1024);
    adaptive_rank::QuestionBank bank;
    TEST_ASSERT(bank.init(arena, 20) == core::StatusCode::Ok, "QuestionBank init succeeded");

    // Add calibrated questions across difficulty tiers
    TEST_ASSERT(bank.add_calibrated(1, 0, -2.0, 1, 1.0) == core::StatusCode::Ok, "add Q1 ok");
    TEST_ASSERT(bank.add_calibrated(2, 0, -1.0, 2, 1.0) == core::StatusCode::Ok, "add Q2 ok");
    TEST_ASSERT(bank.add_calibrated(3, 0,  0.0, 0, 1.0) == core::StatusCode::Ok, "add Q3 ok");
    TEST_ASSERT(bank.add_calibrated(4, 0,  1.0, 3, 1.0) == core::StatusCode::Ok, "add Q4 ok");
    TEST_ASSERT(bank.add_calibrated(5, 0,  2.0, 1, 1.0) == core::StatusCode::Ok, "add Q5 ok");

    TEST_ASSERT(bank.size() == 5, "5 questions in bank");
    TEST_ASSERT(bank.find(3) != nullptr, "Question 3 found");
    TEST_ASSERT(bank.find(99) == nullptr, "Question 99 not found");

    // Create an ExamSession
    adaptive_rank::SelectorConfig cfg{};
    cfg.max_questions = 4;
    cfg.min_questions = 2;
    cfg.se_stop = 0.2;
    cfg.prior_mean = 0.0;
    cfg.prior_sd = 1.0;

    adaptive_rank::ExamSession session;
    TEST_ASSERT(session.init(arena, bank, 101, cfg) == core::StatusCode::Ok, "ExamSession init succeeded");
    TEST_ASSERT(!session.finished(), "ExamSession is not finished initially");

    // Get first question
    core::QuestionId qid = core::INVALID_QUESTION_ID;
    TEST_ASSERT(session.next_question(&qid) == core::StatusCode::Ok, "next_question returned question");
    TEST_ASSERT(qid != core::INVALID_QUESTION_ID, "qid is valid");

    // Student answers correctly
    bool correct = false;
    core::u8 correct_opt = bank.find(qid)->correct_option;
    TEST_ASSERT(session.submit_option(qid, correct_opt, 5000, &correct) == core::StatusCode::Ok, "submit_option ok");
    TEST_ASSERT(correct == true, "Answer was graded correct");
    TEST_ASSERT(session.answered() == 1, "1 question answered");
    TEST_ASSERT(session.ability() > 0.0, "Ability theta increased after correct answer");

    // Get next adaptive question
    core::QuestionId qid2 = core::INVALID_QUESTION_ID;
    TEST_ASSERT(session.next_question(&qid2) == core::StatusCode::Ok, "next_question 2 ok");
    TEST_ASSERT(qid2 != qid, "Next question is distinct from answered question");

    // Student answers wrongly
    core::u8 wrong_opt = (bank.find(qid2)->correct_option + 1) % 4;
    TEST_ASSERT(session.submit_option(qid2, wrong_opt, 6000, &correct) == core::StatusCode::Ok, "submit_option 2 ok");
    TEST_ASSERT(correct == false, "Answer was graded wrong");
    TEST_ASSERT(session.answered() == 2, "2 questions answered");
    TEST_ASSERT(session.scaled_score() >= 0 && session.scaled_score() <= 10000, "Scaled score within [0, 10000]");
}

// -----------------------------------------------------------------------------
// Suite 4: C ABI & End-to-End Ingest Pipeline
// -----------------------------------------------------------------------------

static void test_c_api_integration () {
    TEST_SUITE_START("C ABI Lifecycle, Wire Packets & Ingest Pipeline");

    // 1. Create Engine
    EduRouteEngine *engine = eduroute_create(100, 20, nullptr);
    TEST_ASSERT(engine != nullptr, "eduroute_create succeeded");

    core::usize mem_start = eduroute_get_arena_Memory_used(engine);
    TEST_ASSERT(mem_start > 0, "Initial arena memory is tracked");

    // 2. Register Questions
    eduroute_register_question(engine, 1, 0, 900.0f, 15000, 1);
    eduroute_register_question(engine, 2, 0, 1100.0f, 15000, 2);
    eduroute_register_question(engine, 3, 0, 1300.0f, 20000, 0);
    eduroute_register_question(engine, 4, 0, 1500.0f, 25000, 3);

    // 3. Build valid wire packet for Student 1
    core::u8 wire_buf[512];
    core::usize packet_sz = eduroute_build_wire_packet(
        engine,
        1,   // student_id
        1,   // question_id
        0,   // action_type = MCQ
        1,   // correct mcq_option
        4500,// monotonic_dt_ms
        nullptr, 0,
        wire_buf, sizeof(wire_buf)
    );
    TEST_ASSERT(packet_sz == sizeof(EduWireHeader), "Wire packet size matches header size (32 bytes)");

    // 4. Ingest packet
    EduIngestResult res = eduroute_ingest_packet(engine, wire_buf, packet_sz, 1000);
    TEST_ASSERT(res.status_code == 0, "Ingest status_code is Ok (0)");
    TEST_ASSERT(res.current_score > 0, "Score is positive");
    TEST_ASSERT(res.live_rank == 1, "First student is live rank 1");
    TEST_ASSERT(res.next_question_id != 0xFFFF, "Adaptive next question served");
    TEST_ASSERT(res.is_flagged_ring == 0, "Student 1 is not flagged");

    // 5. Corrupted packet test (tampered MAC tag)
    EduWireHeader corrupted_hdr;
    std::memcpy(&corrupted_hdr, wire_buf, sizeof(EduWireHeader));
    corrupted_hdr.mac_tag16 ^= 0xFFFFu; // flip bits
    EduIngestResult corrupt_res = eduroute_ingest_packet(
        engine, reinterpret_cast<const core::u8*>(&corrupted_hdr), sizeof(EduWireHeader), 1001
    );
    TEST_ASSERT(corrupt_res.status_code == static_cast<core::u8>(core::StatusCode::HashChainMismatch),
                "Corrupted MAC rejected with HashChainMismatch");

    // 6. Test collusion detection via wire packets
    // Student 10 and 11 submit identical descriptive answers with close timing
    const char *cheating_text = "Dijkstra algorithm calculates shortest path with priority queue";
    core::usize text_len = std::strlen(cheating_text);

    core::u8 buf10[512];
    core::usize sz10 = eduroute_build_wire_packet(
        engine, 10, 3, 1 /* TextDelta */, 0xFF, 12000, cheating_text, static_cast<core::u32>(text_len), buf10, sizeof(buf10)
    );
    eduroute_ingest_packet(engine, buf10, sz10, 2000);

    core::u8 buf11[512];
    core::usize sz11 = eduroute_build_wire_packet(
        engine, 11, 3, 1 /* TextDelta */, 0xFF, 12050, cheating_text, static_cast<core::u32>(text_len), buf11, sizeof(buf11)
    );
    EduIngestResult res11 = eduroute_ingest_packet(engine, buf11, sz11, 2050);

    TEST_ASSERT(res11.is_flagged_ring == 1, "Student 11 flagged in DSU cheating ring");

    // 7. Verify Leaderboard export
    EduLeaderboardRow rows[10];
    core::usize row_count = eduroute_get_top_k(engine, rows, 10);
    TEST_ASSERT(row_count >= 1, "Top K leaderboard retrieved rows");
    TEST_ASSERT(rows[0].rank == 1, "First row is Rank 1");

    // 8. Verify Collusion Edges export
    EduCollusionEdge edges[10];
    core::usize edge_count = eduroute_get_collusion_edges(engine, edges, 10);
    TEST_ASSERT(edge_count >= 1, "Collusion edge retrieved between Students 10 and 11");
    TEST_ASSERT(edges[0].combined_weight >= 3.0f, "Edge weight >= 3.0 threshold");

    // 9. Destroy Engine
    eduroute_destroy(engine);
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------
int main () {
    std::printf("===============================================================\n");
    std::printf("       EduRoute Engine Extensive Unit Test Suite (No STL)     \n");
    std::printf("===============================================================\n\n");

    // Suite 1: Core Primitives
    test_core_arena();
    test_core_dyn_array();
    test_core_hash_map();
    test_core_dsu();
    test_core_string_view();
    test_core_bit_stream();

    // Suite 2: Division 2 Anti-Cheat
    test_anticheat_kmer_winnow();
    test_anticheat_anomaly_stats();
    test_anticheat_collusion_graph();

    // Suite 3: Division 3 Adaptive Rank
    test_rank_fenwick_score();
    test_rank_order_stat_avl();
    test_rank_irt_selector();

    // Suite 4: C ABI & Pipeline Integration
    test_c_api_integration();

    std::printf("\n===============================================================\n");
    std::printf(" Test Results: %d/%d assertions PASSED (%d failed) across %d suites\n",
                g_total_assertions - g_failed_assertions, g_total_assertions, g_failed_assertions, g_suites_run);
    std::printf("===============================================================\n");

    return (g_failed_assertions == 0) ? 0 : 1;
}
