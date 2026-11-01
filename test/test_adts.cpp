/**
 * EduRoute Comprehensive Unit Test Suite
 * Covers Core Primitives, Division 2 Anti-Cheat, Division 3 Adaptive Rank, and C ABI.
 * STRICTLY NO STL CONTAINERS (No std::vector, std::string, std::map).
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/arena_alloc.hpp"
#include "core/bit_stream.hpp"
#include "core/dsu.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"
#include "core/string_view.hpp"
#include "core/types.hpp"

#include "anticheat/anomaly_stats.hpp"
#include "anticheat/collusion_graph.hpp"
#include "anticheat/kmer_winnow.hpp"

#include "adaptive_rank/fenwick_score.hpp"
#include "adaptive_rank/irt_selector.hpp"
#include "adaptive_rank/order_stat_avl.hpp"

#include "sync_engine/huffman_delta.hpp"
#include "sync_engine/merkle_dag.hpp"
#include "sync_engine/wal_ring.hpp"

#include "eduroute_c_api.h"

// Test framework state
static int g_total_assertions  = 0;
static int g_failed_assertions = 0;
static int g_suites_run        = 0;

#define TEST_ASSERT(cond, msg)                                                                                         \
    do {                                                                                                               \
        ++g_total_assertions;                                                                                          \
        if (!(cond)) {                                                                                                 \
            std::printf ("  [FAIL] %s:%d: Assertion failed: (%s) - %s\n", __FILE__, __LINE__, #cond, msg);             \
            ++g_failed_assertions;                                                                                     \
        }                                                                                                              \
    } while (0)

#define TEST_SUITE_START(name)                                                                                         \
    do {                                                                                                               \
        ++g_suites_run;                                                                                                \
        std::printf ("[RUNNING TEST SUITE] %s\n", name);                                                               \
    } while (0)

// Core arena & data structure unit tests

static void test_core_arena () {
    TEST_SUITE_START ("Core Arena Allocator");

    constexpr core::usize ARENA_SIZE = 4096;
    core::Arena arena (ARENA_SIZE);

    TEST_ASSERT (arena.capacity () == ARENA_SIZE, "Arena capacity matches request");
    TEST_ASSERT (arena.used () == 0, "Initial arena usage is 0");

    // Bump pointer allocation & 8-byte alignment
    void *p1 = arena.allocate (100, 8);
    TEST_ASSERT (p1 != nullptr, "p1 allocated");
    TEST_ASSERT ((reinterpret_cast<std::uintptr_t> (p1) % 8) == 0, "p1 is 8-byte aligned");
    TEST_ASSERT (arena.used () >= 100, "Used bytes increased");

    // Stricter alignment test (64-byte cache line)
    void *p2 = arena.allocate (50, 64);
    TEST_ASSERT (p2 != nullptr, "p2 allocated");
    TEST_ASSERT ((reinterpret_cast<std::uintptr_t> (p2) % 64) == 0, "p2 is 64-byte aligned");

    // In-place placement new on POD struct
    struct Point {
            int x;
            int y;
            float z;
    };
    Point *pt = arena.create<Point> (10, 20, 30.5f);
    TEST_ASSERT (pt != nullptr, "Point constructed in-place");
    TEST_ASSERT (pt->x == 10 && pt->y == 20 && pt->z == 30.5f, "Point values preserved");

    // Rewind / rollback check using saved marker
    auto marker              = arena.get_marker ();
    core::usize used_at_mark = arena.used ();

    void *temp = arena.allocate (500, 8);
    TEST_ASSERT (temp != nullptr, "temp allocated");
    TEST_ASSERT (arena.used () > used_at_mark, "usage grew after temp");

    arena.reset_to_marker (marker);
    TEST_ASSERT (arena.used () == used_at_mark, "arena usage rolled back to marker");

    // ScopedArena RAII helper: unwinds allocations on block scope exit
    {
        core::ScopedArena scoped (arena);
        void *scoped_mem = scoped.get ().allocate (200, 8);
        TEST_ASSERT (scoped_mem != nullptr, "scoped_mem allocated");
        TEST_ASSERT (arena.used () > used_at_mark, "arena used grew inside scope");
    }
    TEST_ASSERT (arena.used () == used_at_mark, "ScopedArena automatically rolled back memory on exit");

    // Make sure allocator doesn't panic on OOM, just returns nullptr
    void *huge = arena.allocate (ARENA_SIZE * 2, 8);
    TEST_ASSERT (huge == nullptr, "OOM allocation returns nullptr");

    // Total arena wipe
    arena.reset ();
    TEST_ASSERT (arena.used () == 0, "arena.reset() restores usage to 0");
}

static void test_core_dyn_array () {
    TEST_SUITE_START ("Core Array<T>");

    core::Arena arena (8192);
    core::Array<int> arr (arena, 4);

    TEST_ASSERT (arr.empty (), "New array is empty");
    TEST_ASSERT (arr.size () == 0, "Size is 0");
    TEST_ASSERT (arr.capacity () >= 4, "Reserved capacity is at least 4");

    for (int i = 0; i < 20; ++i) { arr.push_back (i * 10); }

    TEST_ASSERT (arr.size () == 20, "Pushed 20 elements");
    TEST_ASSERT (arr[0] == 0, "First element is 0");
    TEST_ASSERT (arr[19] == 190, "Last element is 190");

    arr.pop_back ();
    TEST_ASSERT (arr.size () == 19, "pop_back reduced size to 19");
    TEST_ASSERT (arr[arr.size () - 1] == 180, "New last element is 180");

    // Verify range-for iterator support
    int sum = 0;
    for (int v: arr) { sum += v; }
    TEST_ASSERT (sum == (19 * 180) / 2, "Iterator sum over elements is correct");

    // Emplace back test with forwarding constructor args
    struct Item {
            int id;
            float weight;
    };
    core::Array<Item> items (arena);
    items.emplace_back (1, 4.5f);
    items.emplace_back (2, 9.2f);
    TEST_ASSERT (items.size () == 2, "Emplaced 2 items");
    TEST_ASSERT (items[0].id == 1 && items[0].weight == 4.5f, "Item 0 matches");
    TEST_ASSERT (items[1].id == 2 && items[1].weight == 9.2f, "Item 1 matches");

    items.clear ();
    TEST_ASSERT (items.empty (), "clear() sets size to 0");
}

static void test_core_hash_map () {
    TEST_SUITE_START ("Core HashMap<K, V>");

    core::Arena arena (32768);
    core::HashMap<core::u32, core::u32> map (arena, 16);

    TEST_ASSERT (map.empty (), "New map is empty");
    TEST_ASSERT (map.size () == 0, "Size is 0");

    // Insert keys & test find
    map.insert (100, 500);
    map.insert (200, 600);
    map.insert (300, 700);

    TEST_ASSERT (map.size () == 3, "Size is 3");
    core::u32 *v1 = map.find (100);
    core::u32 *v2 = map.find (200);
    core::u32 *v3 = map.find (300);
    core::u32 *v4 = map.find (999);

    TEST_ASSERT (v1 != nullptr && *v1 == 500, "Key 100 found with value 500");
    TEST_ASSERT (v2 != nullptr && *v2 == 600, "Key 200 found with value 600");
    TEST_ASSERT (v3 != nullptr && *v3 == 700, "Key 300 found with value 700");
    TEST_ASSERT (v4 == nullptr, "Key 999 not found");

    // Overwriting existing key should update value in-place
    map.insert (100, 555);
    TEST_ASSERT (map.size () == 3, "Size remains 3 after update");
    TEST_ASSERT (*map.find (100) == 555, "Value for key 100 successfully updated to 555");

    // Stress test rehashing: insert 150 items to trigger multiple growth steps
    for (core::u32 i = 1000; i < 1150; ++i) { map.insert (i, i * 2); }
    TEST_ASSERT (map.size () == 153, "Map rehashed and grew to 153 elements");

    // Check that Robin Hood displacement preserved every key after table expansion
    bool all_found = true;
    for (core::u32 i = 1000; i < 1150; ++i) {
        core::u32 *val = map.find (i);
        if (!val || *val != i * 2) {
            all_found = false;
            break;
        }
    }
    TEST_ASSERT (all_found, "All 150 elements found with correct values after rehash");
}

static void test_core_dsu () {
    TEST_SUITE_START ("Core DisjointSet (DSU)");

    core::Arena arena (4096);
    core::DisjointSet dsu (arena, 10);

    TEST_ASSERT (dsu.size () == 10, "DSU size is 10");
    TEST_ASSERT (dsu.num_components () == 10, "Initial components is 10");

    for (core::u32 i = 0; i < 10; ++i) { TEST_ASSERT (dsu.component_size (i) == 1, "Initial component size is 1"); }

    // Join elements 1 and 2
    TEST_ASSERT (dsu.join (1, 2) == true, "Join 1 and 2 succeeded");
    TEST_ASSERT (dsu.connected (1, 2) == true, "1 and 2 are connected");
    TEST_ASSERT (dsu.num_components () == 9, "Components reduced to 9");
    TEST_ASSERT (dsu.component_size (1) == 2, "Component size is now 2");

    // Duplicate join should be a no-op
    TEST_ASSERT (dsu.join (1, 2) == false, "Redundant join returns false");

    // Connect {1,2} with {3,4} -> merged component of 4 items
    dsu.join (3, 4);
    dsu.join (2, 3);
    TEST_ASSERT (dsu.connected (1, 4) == true, "1 and 4 are transitively connected");
    TEST_ASSERT (dsu.component_size (1) == 4, "Cluster size is 4");
    TEST_ASSERT (dsu.num_components () == 7, "Total components is 7");
    TEST_ASSERT (dsu.connected (1, 5) == false, "1 and 5 are not connected");
}

static void test_core_string_view () {
    TEST_SUITE_START ("Core StringView");

    core::StringView sv ("Hello EduRoute World");
    TEST_ASSERT (sv.size () == 20, "StringView length matches");
    TEST_ASSERT (!sv.empty (), "StringView is not empty");
    TEST_ASSERT (sv[0] == 'H' && sv[6] == 'E', "Index access correct");

    core::StringView sub = sv.substr (6, 8);
    TEST_ASSERT (sub == core::StringView ("EduRoute"), "substr(6, 8) equals EduRoute");

    core::StringView mod = sv;
    mod.remove_prefix (6);
    TEST_ASSERT (mod.substr (0, 8) == core::StringView ("EduRoute"), "remove_prefix works");

    mod.remove_suffix (6);
    TEST_ASSERT (mod == core::StringView ("EduRoute"), "remove_suffix works");

    // FNV-1a hash consistency check
    core::Hash64 h1 = core::StringView ("TestString").hash ();
    core::Hash64 h2 = core::StringView ("TestString").hash ();
    core::Hash64 h3 = core::StringView ("DifferentString").hash ();
    TEST_ASSERT (h1 == h2, "Identical strings produce identical FNV-1a hash");
    TEST_ASSERT (h1 != h3, "Different strings produce different hashes");
}

static void test_core_bit_stream () {
    TEST_SUITE_START ("Core BitWriter & BitReader");

    core::Arena arena (1024);
    core::BitWriter writer (arena, 64);

    // Pack alternating single bits and small numbers
    writer.write_bit (true);      // 1
    writer.write_bit (false);     // 0
    writer.write_bit (true);      // 1
    writer.write_bits (0x0Au, 4); // 1010
    writer.write_bits (0x3Fu, 6); // 111111
    writer.flush ();

    TEST_ASSERT (writer.byte_size () > 0, "Writer generated byte output");

    core::BitReader reader (writer.data (), writer.byte_size ());
    TEST_ASSERT (reader.read_bit () == true, "Bit 1 matches");
    TEST_ASSERT (reader.read_bit () == false, "Bit 2 matches");
    TEST_ASSERT (reader.read_bit () == true, "Bit 3 matches");
    TEST_ASSERT (reader.read_bits (4) == 0x0Au, "4-bit value 0x0A matches");
    TEST_ASSERT (reader.read_bits (6) == 0x3Fu, "6-bit value 0x3F matches");
}

// Anti-cheat engine unit tests

static void test_anticheat_kmer_winnow () {
    TEST_SUITE_START ("Div 2 KmerWinnow Fingerprinting");

    core::Arena pers_arena (1024 * 1024);
    core::Arena scratch_arena (1024 * 1024);
    core::anticheat::KmerWinnow winnow (pers_arena);

    core::Array<core::Hash64> fps1 (scratch_arena, 64);
    core::Array<core::Hash64> fps2 (scratch_arena, 64);

    // Edge case: string shorter than k=5 k-mer length should yield zero fingerprints
    core::anticheat::KmerWinnow::extract_fingerprints (core::StringView ("Ab1"), scratch_arena, fps1);
    TEST_ASSERT (fps1.empty (), "Text shorter than KMER_LEN produces 0 fingerprints");

    // Case and punctuation normalization check
    core::StringView textA ("Balanced binary search trees maintain O(log n) height.");
    core::StringView textB ("balanced BINARY search trees maintain o(log n) height!!");

    fps1.clear ();
    fps2.clear ();
    core::anticheat::KmerWinnow::extract_fingerprints (textA, scratch_arena, fps1);
    core::anticheat::KmerWinnow::extract_fingerprints (textB, scratch_arena, fps2);

    TEST_ASSERT (!fps1.empty (), "textA extracted fingerprints");
    TEST_ASSERT (fps1.size () == fps2.size (), "Fingerprint counts match despite case/punctuation differences");

    bool identical = true;
    for (core::usize i = 0; i < fps1.size (); ++i) {
        if (fps1[i] != fps2[i]) {
            identical = false;
            break;
        }
    }
    TEST_ASSERT (identical, "Fingerprints match exactly after normalization");

    // Inverted index match check
    core::Array<core::anticheat::TextSimMatch> matches (scratch_arena, 16);

    // Index first submission (Student 1)
    winnow.index_and_match (1, 10, fps1, scratch_arena, matches);
    TEST_ASSERT (matches.empty (), "First student has no prior matches");

    // Student 2 submits the identical text for question 10
    matches.clear ();
    winnow.index_and_match (2, 10, fps2, scratch_arena, matches);
    TEST_ASSERT (!matches.empty (), "Student 2 matched with Student 1");
    TEST_ASSERT (matches[0].other_student == 1, "Matched other_student is 1");
    TEST_ASSERT (matches[0].cont_score >= 0.95f, "Containment score is ~1.0 for identical text");

    // Student 3 submits completely different text
    core::StringView textC ("Quantum computing utilizes superposition and entanglement of qubits.");
    core::Array<core::Hash64> fps3 (scratch_arena, 64);
    core::anticheat::KmerWinnow::extract_fingerprints (textC, scratch_arena, fps3);

    matches.clear ();
    winnow.index_and_match (3, 10, fps3, scratch_arena, matches);
    TEST_ASSERT (matches.empty (), "Completely different text produces 0 collisions");
}

static void test_anticheat_anomaly_stats () {
    TEST_SUITE_START ("Div 2 AnomalyStats (Time-Sync & Rare Errors)");

    core::Arena arena (1024 * 1024);
    core::Arena scratch (1024 * 1024);
    core::anticheat::AnomalyStats stats;
    core::StatusCode st = stats.init (arena, 512);
    TEST_ASSERT (st == core::StatusCode::Ok, "AnomalyStats init succeeded");
    TEST_ASSERT (stats.ready (), "AnomalyStats is ready");

    // Test 1: simultaneous submission timing spike (|dt_a - dt_b| <= 500 ms)
    core::Array<core::anticheat::StatAnomalyMatch> matches (scratch, 16);

    // Student 1 answers Q1 at 10.0s
    stats.record_dt (1, 1, 10000, scratch, matches);
    TEST_ASSERT (matches.empty (), "First student has no time-sync peers");

    // Student 2 answers Q1 at 10.15s (150ms delta -> within 500ms suspicious window)
    matches.clear ();
    stats.record_dt (2, 1, 10150, scratch, matches);
    TEST_ASSERT (!matches.empty (), "Time-sync spike detected between Student 2 and Student 1");
    TEST_ASSERT (matches[0].other_student == 1, "Peer is Student 1");
    TEST_ASSERT (matches[0].time_score >= 1.0f, "Time score incremented");

    // Student 3 answers 5s later -> well outside window, no hit
    matches.clear ();
    stats.record_dt (3, 1, 15000, scratch, matches);
    TEST_ASSERT (matches.empty (), "Student 3 with far dt triggers no time-sync spike");

    // Test 2: shared rare mistake detection on MCQ
    // Seed with 35 students picking correct option A
    for (core::StudentId s = 10; s < 45; ++s) {
        matches.clear ();
        stats.record_mcq (s, 2, 0, 0, scratch, matches);
    }

    // Student 4 picks distractor D (< 10% frequency -> rare error)
    matches.clear ();
    stats.record_mcq (4, 2, 3, 0, scratch, matches);
    TEST_ASSERT (matches.empty (), "First student choosing rare error has no peer match yet");

    // Student 5 picks the exact same rare mistake D -> should trigger collusion hit
    matches.clear ();
    stats.record_mcq (5, 2, 3, 0, scratch, matches);
    TEST_ASSERT (!matches.empty (), "Shared rare error detected between Student 5 and Student 4");
    TEST_ASSERT (matches[0].other_student == 4, "Matched other student is Student 4");
    TEST_ASSERT (matches[0].err_score >= 2.0f, "Err score incremented");

    // Verify lookup is symmetric regardless of argument order
    core::f32 t_score = 0.0f, e_score = 0.0f;
    TEST_ASSERT (stats.get_pair_scores (4, 5, &t_score, &e_score) == core::StatusCode::Ok, "Query (4, 5) succeeded");
    core::f32 t_score2 = 0.0f, e_score2 = 0.0f;
    TEST_ASSERT (stats.get_pair_scores (5, 4, &t_score2, &e_score2) == core::StatusCode::Ok, "Query (5, 4) succeeded");
    TEST_ASSERT (t_score == t_score2 && e_score == e_score2, "Pair scores are symmetric (A,B == B,A)");
}

static void test_anticheat_collusion_graph () {
    TEST_SUITE_START ("Div 2 CollusionGraph & DSU Cheating Clusters");

    core::Arena arena (1024 * 1024);
    core::anticheat::CollusionGraph graph;
    TEST_ASSERT (graph.init (arena, 100, 256) == core::StatusCode::Ok, "CollusionGraph init succeeded");

    bool flagged = false;

    // Everyone starts unflagged
    TEST_ASSERT (!graph.is_flagged (1), "Student 1 initially not flagged");
    TEST_ASSERT (!graph.is_flagged (2), "Student 2 initially not flagged");

    // Mild time-sync signal alone (W = 1.0 < 3.0 threshold)
    graph.update_stat (1, 2, 1.0f, 0.0f, flagged);
    TEST_ASSERT (!flagged, "W=1.0 is below COLLUSION_THRESHOLD (3.0)");
    TEST_ASSERT (!graph.is_flagged (1), "Student 1 still not in cheating cluster");

    // Heavy text match pushes W across 3.0 threshold -> flags students
    graph.update_text (1, 2, 0.9f, flagged);
    TEST_ASSERT (flagged, "Combined weight crossed 3.0 -> flagged confirmed colluders");
    TEST_ASSERT (graph.is_flagged (1), "Student 1 is now flagged in DSU");
    TEST_ASSERT (graph.is_flagged (2), "Student 2 is now flagged in DSU");
    TEST_ASSERT (graph.cluster_root (1) == graph.cluster_root (2), "1 and 2 share the same DSU cluster root");
    TEST_ASSERT (graph.cluster_size (1) == 2, "Cluster size is 2");

    // Transitive clustering: student 2 also cheated with student 3
    bool flagged2 = false;
    graph.update_text (2, 3, 1.0f, flagged2); // W = 4.0 >= 3.0
    TEST_ASSERT (flagged2, "2 and 3 confirmed colluders");
    TEST_ASSERT (graph.cluster_root (1) == graph.cluster_root (3), "1, 2, and 3 are in the same DSU cluster");
    TEST_ASSERT (graph.cluster_size (1) == 3, "Cluster size expanded to 3 students");

    // Inspect stored edge stats
    core::anticheat::CollusionEdge edge{};
    TEST_ASSERT (graph.get_edge (1, 2, &edge), "Edge (1, 2) retrieved");
    TEST_ASSERT (edge.combined_weight >= 4.0f, "Combined weight accurately recorded");
    TEST_ASSERT (edge.peak_text_sim == 0.9f, "Peak text similarity matches");

    // Export confirmed edges for teacher dashboard
    core::anticheat::CollusionEdge confirmed[10];
    core::usize edge_count = graph.get_confirmed_edges (confirmed, 10);
    TEST_ASSERT (edge_count == 2, "2 confirmed collusion edges in graph");
}

// -----------------------------------------------------------------------------
// Suite 3: Division 3 Adaptive Rank
// -----------------------------------------------------------------------------

static void test_rank_fenwick_score () {
    TEST_SUITE_START ("Div 3 FenwickScore Percentile Engine");

    core::Arena arena (1024 * 1024);
    adaptive_rank::FenwickScore fenwick;
    TEST_ASSERT (fenwick.init (arena) == core::StatusCode::Ok, "FenwickScore init succeeded");

    TEST_ASSERT (fenwick.total () == 0, "Initial total is 0");

    // Populate initial scores (ranging 0 - 10000)
    TEST_ASSERT (fenwick.add (2000) == core::StatusCode::Ok, "add 2000 ok");
    TEST_ASSERT (fenwick.add (5000) == core::StatusCode::Ok, "add 5000 ok");
    TEST_ASSERT (fenwick.add (5000) == core::StatusCode::Ok, "add 5000 tie ok");
    TEST_ASSERT (fenwick.add (8000) == core::StatusCode::Ok, "add 8000 ok");
    TEST_ASSERT (fenwick.add (9000) == core::StatusCode::Ok, "add 9000 ok");

    TEST_ASSERT (fenwick.total () == 5, "Total count is 5");
    TEST_ASSERT (fenwick.count_at (5000) == 2, "Exact count at 5000 is 2");
    TEST_ASSERT (fenwick.count_less (5000) == 1, "Count strictly less than 5000 is 1 (2000)");
    TEST_ASSERT (fenwick.count_greater (5000) == 2, "Count strictly greater than 5000 is 2 (8000, 9000)");
    TEST_ASSERT (fenwick.count_in_range (4000, 8500) == 3, "Range [4000, 8500] contains 3 scores (5000, 5000, 8000)");

    // 1-based ranks: highest score holds rank 1
    TEST_ASSERT (fenwick.rank (9000) == 1, "Score 9000 has rank 1 (best)");
    TEST_ASSERT (fenwick.rank (8000) == 2, "Score 8000 has rank 2");
    TEST_ASSERT (fenwick.rank (5000) == 3, "Score 5000 has rank 3");
    TEST_ASSERT (fenwick.rank (2000) == 5, "Score 2000 has rank 5");

    // Mid-percentile formula: (strictly_below + 0.5 * tied) / total * 100
    // At score 5000: (1 + 0.5 * 2) / 5 * 100 = 40.0%
    core::f64 pct = fenwick.percentile (5000);
    TEST_ASSERT (std::fabs (pct - 40.0) < 1e-4, "Percentile for score 5000 is exactly 40%");

    // Binary lifting prefix walk to pinpoint rank -> score
    core::i32 score_out = 0;
    TEST_ASSERT (fenwick.score_at_rank (1, &score_out) == core::StatusCode::Ok, "score_at_rank(1) ok");
    TEST_ASSERT (score_out == 9000, "Rank 1 is score 9000");

    TEST_ASSERT (fenwick.score_at_rank (2, &score_out) == core::StatusCode::Ok, "score_at_rank(2) ok");
    TEST_ASSERT (score_out == 8000, "Rank 2 is score 8000");

    // Student retakes test or improves score: remove 2000 and insert 9500
    TEST_ASSERT (fenwick.update (2000, 9500) == core::StatusCode::Ok, "update(2000, 9500) ok");
    TEST_ASSERT (fenwick.total () == 5, "Total count remains 5 after update");
    TEST_ASSERT (fenwick.rank (9500) == 1, "Updated score 9500 is now rank 1");
    TEST_ASSERT (fenwick.rank (9000) == 2, "Former top score 9000 is now rank 2");

    // Sanity check clamping so weird inputs don't segfault bucket lookups
    TEST_ASSERT (fenwick.add (-100) == core::StatusCode::Ok, "Negative score clamped to bucket 0");
    TEST_ASSERT (fenwick.add (999999) == core::StatusCode::Ok, "Excessive score clamped to max bucket");
}

static void test_rank_order_stat_avl () {
    TEST_SUITE_START ("Div 3 OrderStatAvl Tie-Broken Leaderboard");

    core::Arena arena (65536);
    core::adaptive_rank::OrderStatAvl avl;
    TEST_ASSERT (avl.init (arena, 64) == core::StatusCode::Ok, "OrderStatAvl init succeeded");

    TEST_ASSERT (avl.size () == 0, "Tree is empty");
    TEST_ASSERT (avl.validate (), "Empty tree validates invariants");

    // Tie-break rules: highest score wins -> fastest completion time -> lowest student ID
    core::LeaderboardKey k1{8000, 30000, 1}; // 8000 pts in 30s, sid 1
    core::LeaderboardKey k2{8000, 20000, 2}; // 8000 pts in 20s (faster than k1), sid 2
    core::LeaderboardKey k3{9000, 40000, 3}; // 9000 pts in 40s (outright leader), sid 3
    core::LeaderboardKey k4{5000, 10000, 4}; // 5000 pts in 10s (trailing score), sid 4

    TEST_ASSERT (avl.insert (k1) == core::StatusCode::Ok, "insert k1 ok");
    TEST_ASSERT (avl.insert (k2) == core::StatusCode::Ok, "insert k2 ok");
    TEST_ASSERT (avl.insert (k3) == core::StatusCode::Ok, "insert k3 ok");
    TEST_ASSERT (avl.insert (k4) == core::StatusCode::Ok, "insert k4 ok");

    TEST_ASSERT (avl.size () == 4, "4 keys inserted");
    TEST_ASSERT (avl.validate (), "AVL invariants hold after 4 insertions");

    // Expected standings:
    // 1st: k3 (9000 pts)
    // 2nd: k2 (8000 pts, faster time)
    // 3rd: k1 (8000 pts, slower time)
    // 4th: k4 (5000 pts)
    core::u32 rank = 0;
    avl.rank_of (k3, &rank);
    TEST_ASSERT (rank == 1, "k3 (9000) is Rank 1");
    avl.rank_of (k2, &rank);
    TEST_ASSERT (rank == 2, "k2 (8000, 20s) is Rank 2 (tie-broken by faster time)");
    avl.rank_of (k1, &rank);
    TEST_ASSERT (rank == 3, "k1 (8000, 30s) is Rank 3");
    avl.rank_of (k4, &rank);
    TEST_ASSERT (rank == 4, "k4 (5000) is Rank 4");

    // Subtree-size order-statistic selection (1-indexed)
    core::LeaderboardKey selected{};
    TEST_ASSERT (avl.select (1, &selected) == core::StatusCode::Ok, "select(1) ok");
    TEST_ASSERT (selected == k3, "select(1) retrieved k3");
    TEST_ASSERT (avl.select (2, &selected) == core::StatusCode::Ok, "select(2) ok");
    TEST_ASSERT (selected == k2, "select(2) retrieved k2");

    // In-order traversal export for top-K display
    core::LeaderboardKey top[4];
    core::u32 written = avl.top_k (3, top);
    TEST_ASSERT (written == 3, "top_k wrote 3 items");
    TEST_ASSERT (top[0] == k3 && top[1] == k2 && top[2] == k1, "top_k sorted order verified");

    // Student 4 resubmits and surges into first place (9500 pts)
    core::LeaderboardKey k4_new{9500, 15000, 4};
    TEST_ASSERT (avl.update (k4, k4_new) == core::StatusCode::Ok, "update key succeeded");
    TEST_ASSERT (avl.validate (), "AVL invariants hold after update");
    avl.rank_of (k4_new, &rank);
    TEST_ASSERT (rank == 1, "k4_new is now Rank 1");

    // Deletion check
    TEST_ASSERT (avl.erase (k1) == core::StatusCode::Ok, "erase k1 succeeded");
    TEST_ASSERT (avl.size () == 3, "Size reduced to 3");
    TEST_ASSERT (!avl.contains (k1), "k1 no longer in tree");
    TEST_ASSERT (avl.validate (), "AVL invariants hold after erase");
}

static void test_rank_irt_selector () {
    TEST_SUITE_START ("Div 3 IRT QuestionBank & Adaptive Session");

    core::Arena arena (1024 * 1024);
    adaptive_rank::QuestionBank bank;
    TEST_ASSERT (bank.init (arena, 20) == core::StatusCode::Ok, "QuestionBank init succeeded");

    // Populate question pool spanning difficulty spectrum [-2.0, +2.0]
    TEST_ASSERT (bank.add_calibrated (1, 0, -2.0, 1, 1.0) == core::StatusCode::Ok, "add Q1 ok");
    TEST_ASSERT (bank.add_calibrated (2, 0, -1.0, 2, 1.0) == core::StatusCode::Ok, "add Q2 ok");
    TEST_ASSERT (bank.add_calibrated (3, 0, 0.0, 0, 1.0) == core::StatusCode::Ok, "add Q3 ok");
    TEST_ASSERT (bank.add_calibrated (4, 0, 1.0, 3, 1.0) == core::StatusCode::Ok, "add Q4 ok");
    TEST_ASSERT (bank.add_calibrated (5, 0, 2.0, 1, 1.0) == core::StatusCode::Ok, "add Q5 ok");

    TEST_ASSERT (bank.size () == 5, "5 questions in bank");
    TEST_ASSERT (bank.find (3) != nullptr, "Question 3 found");
    TEST_ASSERT (bank.find (99) == nullptr, "Question 99 not found");

    // Configure test stopping criteria
    adaptive_rank::SelectorConfig cfg{};
    cfg.max_questions = 4;
    cfg.min_questions = 2;
    cfg.se_stop       = 0.2;
    cfg.prior_mean    = 0.0;
    cfg.prior_sd      = 1.0;

    adaptive_rank::ExamSession session;
    TEST_ASSERT (session.init (arena, bank, 101, cfg) == core::StatusCode::Ok, "ExamSession init succeeded");
    TEST_ASSERT (!session.finished (), "ExamSession is not finished initially");

    // Pick entry-level item based on prior theta
    core::QuestionId qid = core::INVALID_QUESTION_ID;
    TEST_ASSERT (session.next_question (&qid) == core::StatusCode::Ok, "next_question returned question");
    TEST_ASSERT (qid != core::INVALID_QUESTION_ID, "qid is valid");

    // Correct answer -> ability estimate should increase
    bool correct         = false;
    core::u8 correct_opt = bank.find (qid)->correct_option;
    TEST_ASSERT (session.submit_option (qid, correct_opt, 5000, &correct) == core::StatusCode::Ok, "submit_option ok");
    TEST_ASSERT (correct == true, "Answer was graded correct");
    TEST_ASSERT (session.answered () == 1, "1 question answered");
    TEST_ASSERT (session.ability () > 0.0, "Ability theta increased after correct answer");

    // Fisher information should steer towards a harder question now
    core::QuestionId qid2 = core::INVALID_QUESTION_ID;
    TEST_ASSERT (session.next_question (&qid2) == core::StatusCode::Ok, "next_question 2 ok");
    TEST_ASSERT (qid2 != qid, "Next question is distinct from answered question");

    // Miss the harder question -> score stays within normalized scale
    core::u8 wrong_opt = (bank.find (qid2)->correct_option + 1) % 4;
    TEST_ASSERT (session.submit_option (qid2, wrong_opt, 6000, &correct) == core::StatusCode::Ok, "submit_option 2 ok");
    TEST_ASSERT (correct == false, "Answer was graded wrong");
    TEST_ASSERT (session.answered () == 2, "2 questions answered");
    TEST_ASSERT (session.scaled_score () >= 0 && session.scaled_score () <= 10000, "Scaled score within [0, 10000]");
}

// -----------------------------------------------------------------------------
// Suite 4: C ABI & End-to-End Ingest Pipeline
// -----------------------------------------------------------------------------

static void test_c_api_integration () {
    TEST_SUITE_START ("C ABI Lifecycle, Wire Packets & Ingest Pipeline");

    // Initialize engine without persistence file backing first
    EduRouteEngine *engine = eduroute_create (100, 20, nullptr);
    TEST_ASSERT (engine != nullptr, "eduroute_create succeeded");

    core::usize mem_start = eduroute_get_arena_Memory_used (engine);
    TEST_ASSERT (mem_start > 0, "Initial arena memory is tracked");

    // Pre-load exam questions with distinct Elo/IRT calibrations
    eduroute_register_question (engine, 1, 0, 900.0f, 15000, 1);
    eduroute_register_question (engine, 2, 0, 1100.0f, 15000, 2);
    eduroute_register_question (engine, 3, 0, 1300.0f, 20000, 0);
    eduroute_register_question (engine, 4, 0, 1500.0f, 25000, 3);

    // Construct a synthetic 32-byte wire header for student 1's first answer
    core::u8 wire_buf[512];
    core::usize packet_sz = eduroute_build_wire_packet (engine,
                                                        1,    // student_id
                                                        1,    // question_id
                                                        0,    // action_type = MCQ
                                                        1,    // correct mcq_option
                                                        4500, // monotonic_dt_ms
                                                        nullptr, 0, wire_buf, sizeof (wire_buf));
    TEST_ASSERT (packet_sz == sizeof (EduWireHeader), "Wire packet size matches header size (32 bytes)");

    // Ingest the legitimate submission through full pipeline
    EduIngestResult res = eduroute_ingest_packet (engine, wire_buf, packet_sz, 1000);
    TEST_ASSERT (res.status_code == 0, "Ingest status_code is Ok (0)");
    TEST_ASSERT (res.current_score > 0, "Score is positive");
    TEST_ASSERT (res.live_rank == 1, "First student is live rank 1");
    TEST_ASSERT (res.next_question_id != 0xFFFF, "Adaptive next question served");
    TEST_ASSERT (res.is_flagged_ring == 0, "Student 1 is not flagged");

    // Tamper with the MAC tag to confirm hash chain integrity rejection
    EduWireHeader corrupted_hdr;
    std::memcpy (&corrupted_hdr, wire_buf, sizeof (EduWireHeader));
    corrupted_hdr.mac_tag16 ^= 0xFFFFu; // bit flip tag
    EduIngestResult corrupt_res = eduroute_ingest_packet (engine, reinterpret_cast<const core::u8 *> (&corrupted_hdr),
                                                          sizeof (EduWireHeader), 1001);
    TEST_ASSERT (corrupt_res.status_code == static_cast<core::u8> (core::StatusCode::HashChainMismatch),
                 "Corrupted MAC rejected with HashChainMismatch");

    // Simultaneous descriptive submissions with near-zero delta time to trigger collusion
    const char *cheating_text = "Dijkstra algorithm calculates shortest path with priority queue";
    core::usize text_len      = std::strlen (cheating_text);

    core::u8 buf10[512];
    core::usize sz10 = eduroute_build_wire_packet (engine, 10, 3, 1 /* TextDelta */, 0xFF, 12000, cheating_text,
                                                   static_cast<core::u32> (text_len), buf10, sizeof (buf10));
    eduroute_ingest_packet (engine, buf10, sz10, 2000);

    core::u8 buf11[512];
    core::usize sz11      = eduroute_build_wire_packet (engine, 11, 3, 1 /* TextDelta */, 0xFF, 12050, cheating_text,
                                                        static_cast<core::u32> (text_len), buf11, sizeof (buf11));
    EduIngestResult res11 = eduroute_ingest_packet (engine, buf11, sz11, 2050);

    TEST_ASSERT (res11.is_flagged_ring == 1, "Student 11 flagged in DSU cheating ring");

    // Check leaderboard C ABI export
    EduLeaderboardRow rows[10];
    core::usize row_count = eduroute_get_top_k (engine, rows, 10);
    TEST_ASSERT (row_count >= 1, "Top K leaderboard retrieved rows");
    TEST_ASSERT (rows[0].rank == 1, "First row is Rank 1");

    // Verify collusion edge exported to frontend/bridge
    EduCollusionEdge edges[10];
    core::usize edge_count = eduroute_get_collusion_edges (engine, edges, 10);
    TEST_ASSERT (edge_count >= 1, "Collusion edge retrieved between Students 10 and 11");
    TEST_ASSERT (edges[0].combined_weight >= 3.0f, "Edge weight >= 3.0 threshold");

    // Clean up
    eduroute_destroy (engine);
}

// -----------------------------------------------------------------------------
// Suite 5: Division 1 Sync Engine - Merkle DAG
// -----------------------------------------------------------------------------
static void test_sync_merkle_dag () {
    TEST_SUITE_START ("Div 1 MerkleDAG Revision History & Tamper Proofing");

    const char *dag_tmp_file = "/tmp/test_merkle_dag_unit.bin";
    std::remove (dag_tmp_file);

    core::Arena arena (1024 * 1024);

    // Test in-memory chain building without file backing
    {
        sync_engine::MerkleDAG dag (arena);
        TEST_ASSERT (!dag.is_open (), "MerkleDAG is not open before file bind");
        TEST_ASSERT (dag.node_count () == 0, "Initial node count is 0");

        core::Hash64 h1 = 0, h2 = 0;
        bool app1 = dag.append (10, 101, 1000, core::StringView ("Version 1"), &h1);
        TEST_ASSERT (app1, "In-memory append 1 succeeds");
        TEST_ASSERT (dag.node_count () == 1, "Node count is 1 after first append");
        TEST_ASSERT (h1 != 0, "Generated node hash is non-zero");

        bool app2 = dag.append (10, 101, 2000, core::StringView ("Version 2"), &h2);
        TEST_ASSERT (app2, "In-memory append 2 succeeds");
        TEST_ASSERT (dag.node_count () == 2, "Node count is 2 after second append");
        TEST_ASSERT (h1 != h2, "Different versions produce different hashes");

        sync_engine::MerkleNode latest_node{};
        TEST_ASSERT (dag.latest (10, 101, latest_node), "dag.latest succeeds");
        TEST_ASSERT (latest_node.node_hash == h2, "Latest node hash matches h2");
        TEST_ASSERT (latest_node.parent_hash == h1, "Latest parent hash matches previous node hash h1");
        TEST_ASSERT (latest_node.answer.size () == 9, "Answer length matches");
        TEST_ASSERT (std::memcmp (latest_node.answer.data (), "Version 2", 9) == 0, "Answer content matches");

        sync_engine::MerkleNode hist0{}, hist1{};
        TEST_ASSERT (dag.history (10, 101, 0, hist0), "dag.history index 0 succeeds");
        TEST_ASSERT (hist0.node_hash == h2, "History index 0 is newest (h2)");
        TEST_ASSERT (dag.history (10, 101, 1, hist1), "dag.history index 1 succeeds");
        TEST_ASSERT (hist1.node_hash == h1, "History index 1 is predecessor (h1)");
        TEST_ASSERT (dag.history_size (10, 101) == 2, "History size is 2");
    }

    // Disk-backed durability and crash replay
    {
        core::Arena arena_disk (1024 * 1024);
        sync_engine::MerkleDAG dag (arena_disk);
        TEST_ASSERT (dag.open (dag_tmp_file), "dag.open creates new snapshot file");
        TEST_ASSERT (dag.is_open (), "dag.is_open is true");

        core::Hash64 file_hashes[7];
        for (int i = 0; i < 7; ++i) {
            char buf[32];
            std::snprintf (buf, sizeof (buf), "State Revision %d", i);
            TEST_ASSERT (dag.append (42, 5, 5000 + static_cast<core::TimestampMs> (i * 100), core::StringView (buf),
                                     &file_hashes[i]),
                         "Append to disk file succeeds");
        }
        TEST_ASSERT (dag.node_count () == 7, "7 nodes appended to file");
        TEST_ASSERT (dag.history_size (42, 5) == 5, "history_size capped at HISTORY_LIMIT (5)");

        dag.close ();
        TEST_ASSERT (!dag.is_open (), "dag.close succeeds");

        // Reopen file into a completely fresh arena to simulate a fresh process start
        core::Arena arena_reopen (1024 * 1024);
        sync_engine::MerkleDAG recovered (arena_reopen);
        TEST_ASSERT (recovered.open (dag_tmp_file), "Reopening existing DAG file succeeds");
        TEST_ASSERT (recovered.node_count () == 7, "All 7 records recovered from disk");

        sync_engine::MerkleNode rec_latest{};
        TEST_ASSERT (recovered.latest (42, 5, rec_latest), "Recovered latest snapshot exists");
        TEST_ASSERT (rec_latest.node_hash == file_hashes[6], "Recovered latest hash matches file_hashes[6]");
        TEST_ASSERT (rec_latest.parent_hash == file_hashes[5], "Recovered parent hash matches file_hashes[5]");
        TEST_ASSERT (rec_latest.answer.size () == 16, "Recovered answer length matches");
        TEST_ASSERT (std::memcmp (rec_latest.answer.data (), "State Revision 6", 16) == 0,
                     "Recovered answer text matches exactly");
        recovered.close ();
    }

    // Tamper with binary header bytes and confirm corrupted files are rejected
    {
        FILE *fp = std::fopen (dag_tmp_file, "r+b");
        TEST_ASSERT (fp != nullptr, "Open DAG file for tampering");
        if (fp) {
            std::fseek (fp, 15, SEEK_SET); // Corrupt magic or version byte in header
            std::fputc (0xFF, fp);
            std::fclose (fp);
        }

        core::Arena arena_corrupt (1024 * 1024);
        sync_engine::MerkleDAG corrupted (arena_corrupt);
        TEST_ASSERT (!corrupted.open (dag_tmp_file), "Corrupted DAG file rejected during replay");
        corrupted.close ();
    }

    std::remove (dag_tmp_file);
}

// -----------------------------------------------------------------------------
// Suite 6: Division 1 Sync Engine - Huffman Delta Compression
// -----------------------------------------------------------------------------
static void test_sync_huffman_delta () {
    TEST_SUITE_START ("Div 1 Huffman Delta Micro-Binary Compression");

    core::Arena arena (1024 * 1024);

    // Compute text diff between drafts
    sync_engine::TextDeltaArray deltas (arena);
    const char *old_txt = "The quick brown fox jumps";
    const char *new_txt = "The quick brown fox jumps over the lazy dog";

    sync_engine::generate_text_delta (1, core::StringView (old_txt), core::StringView (new_txt), deltas);
    TEST_ASSERT (deltas.size () == 1, "Delta generated when text changes");
    TEST_ASSERT (deltas[0].answer_id == 1, "Delta has correct answer_id");

    // Redundant save: identical text should produce zero deltas
    sync_engine::generate_text_delta (1, core::StringView (new_txt), core::StringView (new_txt), deltas);
    TEST_ASSERT (deltas.size () == 1, "No duplicate delta generated when text is unchanged");

    // Build frequency table and pack bits into compressed payload
    sync_engine::CompressedTextDelta compressed = sync_engine::compress_text_delta (arena, deltas);
    TEST_ASSERT (compressed.valid, "Huffman compression produced valid delta");
    TEST_ASSERT (compressed.delta_count == 1, "Compressed delta_count is 1");
    TEST_ASSERT (compressed.original_size > 0, "Original size > 0");
    TEST_ASSERT (compressed.frequencies.size () == 256, "Huffman frequency table has 256 entries");
    TEST_ASSERT (compressed.bytes.size () > 0, "Compressed output bytes > 0");

    // Decompress and reconstruct diff records
    core::Array<core::u8> text_storage (arena);
    sync_engine::TextDeltaArray decompressed = sync_engine::decompress_text_delta (arena, compressed, text_storage);
    TEST_ASSERT (decompressed.size () == 1, "Decompressed delta array size matches 1");
    TEST_ASSERT (decompressed[0].answer_id == 1, "Decompressed answer_id matches 1");
    TEST_ASSERT (decompressed[0].new_value.size () == std::strlen (new_txt), "Decompressed new text length matches");
    TEST_ASSERT (std::memcmp (decompressed[0].new_value.data (), new_txt, decompressed[0].new_value.size ()) == 0,
                 "Decompressed text exactly matches original string");

    // Apply recovered delta onto original baseline string
    core::Array<core::u8> applied_out (arena);
    bool applied = sync_engine::apply_text_delta (core::StringView (old_txt), decompressed[0], applied_out);
    TEST_ASSERT (applied, "apply_text_delta succeeds when old_value matches");
    TEST_ASSERT (applied_out.size () == std::strlen (new_txt), "Applied output length matches");
    TEST_ASSERT (std::memcmp (applied_out.data (), new_txt, applied_out.size ()) == 0,
                 "Applied output string matches new_txt");
}

// -----------------------------------------------------------------------------
// Suite 7: Division 1 Sync Engine - Write Ahead Log (WAL)
// -----------------------------------------------------------------------------
static void test_sync_wal_ring () {
    TEST_SUITE_START ("Div 1 Write-Ahead Log (WAL Ring Buffer)");

    const char *wal_tmp_file = "/tmp/test_wal_unit.bin";
    std::remove (wal_tmp_file);

    core::Arena arena (1024 * 1024);
    {
        sync_engine::WriteAheadLog wal (arena);
        TEST_ASSERT (wal.open (wal_tmp_file), "wal.open succeeds");
        TEST_ASSERT (wal.is_open (), "wal.is_open is true");
        TEST_ASSERT (wal.pending_count () == 0, "Initial pending count is 0");

        // Write first unacknowledged batch
        sync_engine::TextDeltaArray deltas1 (arena);
        sync_engine::generate_text_delta (1, core::StringView ("init"), core::StringView ("first edit"), deltas1);
        sync_engine::CompressedTextDelta comp1 = sync_engine::compress_text_delta (arena, deltas1);

        core::u64 seq1 = 0;
        TEST_ASSERT (wal.append (101, 1000, comp1, &seq1), "wal.append batch 1 succeeds");
        TEST_ASSERT (seq1 == 1, "Sequence 1 assigned");
        TEST_ASSERT (wal.pending_count () == 1, "Pending count is 1");

        // Write second edit before the first gets ACKed
        sync_engine::TextDeltaArray deltas2 (arena);
        sync_engine::generate_text_delta (1, core::StringView ("first edit"), core::StringView ("second edit"),
                                          deltas2);
        sync_engine::CompressedTextDelta comp2 = sync_engine::compress_text_delta (arena, deltas2);

        core::u64 seq2 = 0;
        TEST_ASSERT (wal.append (101, 2000, comp2, &seq2), "wal.append batch 2 succeeds");
        TEST_ASSERT (seq2 == 2, "Sequence 2 assigned");
        TEST_ASSERT (wal.pending_count () == 2, "Pending count is 2");

        // Peek head of the pending queue (FIFO order)
        const sync_engine::WalRecord *rec = nullptr;
        TEST_ASSERT (wal.peek_next (rec), "wal.peek_next returns oldest record");
        TEST_ASSERT (rec != nullptr && rec->sequence == 1, "Oldest record is sequence 1");
        TEST_ASSERT (rec->student_id == 101, "Record student_id matches");

        // ACK sequence 1 and check that ring buffer head advances
        TEST_ASSERT (wal.acknowledge (1), "wal.acknowledge(1) succeeds");
        TEST_ASSERT (wal.acknowledged_sequence () == 1, "Acknowledged sequence is 1");
        TEST_ASSERT (wal.pending_count () == 1, "Pending count reduced to 1");

        // Head should now point to sequence 2
        TEST_ASSERT (wal.peek_next (rec), "wal.peek_next returns next pending");
        TEST_ASSERT (rec != nullptr && rec->sequence == 2, "Next record is sequence 2");

        wal.close ();
    }

    // Reopen and ensure unacknowledged records survive crash
    {
        core::Arena arena_reopen (1024 * 1024);
        sync_engine::WriteAheadLog wal_reopened (arena_reopen);
        TEST_ASSERT (wal_reopened.open (wal_tmp_file), "Reopen WAL file succeeds");
        TEST_ASSERT (wal_reopened.acknowledged_sequence () == 1, "Acknowledged sequence restored as 1");
        TEST_ASSERT (wal_reopened.pending_count () == 1, "1 pending record remaining");

        const sync_engine::WalRecord *rec = nullptr;
        TEST_ASSERT (wal_reopened.peek_next (rec), "peek_next returns sequence 2 after reopen");
        TEST_ASSERT (rec != nullptr && rec->sequence == 2, "Sequence 2 is unacknowledged");

        TEST_ASSERT (wal_reopened.acknowledge (2), "Acknowledge sequence 2");
        TEST_ASSERT (wal_reopened.pending_count () == 0, "0 pending records remaining");
        wal_reopened.close ();
    }

    std::remove (wal_tmp_file);
}

// -----------------------------------------------------------------------------
// Suite 8: C ABI Merkle DAG & Sync Engine Pipeline Integration
// -----------------------------------------------------------------------------
static void test_c_api_merkle_dag () {
    TEST_SUITE_START ("C ABI Merkle DAG Ingest & Query Integration");

    const char *wal_path = "/tmp/test_eduroute_c_api_sync.bin";
    const char *dag_path = "/tmp/test_eduroute_c_api_sync.bin.dag";
    std::remove (wal_path);
    std::remove (dag_path);

    // Boot engine bound to persistent WAL and DAG backing files
    EduRouteEngine *engine = eduroute_create (50, 10, wal_path);
    TEST_ASSERT (engine != nullptr, "eduroute_create with WAL/DAG file path succeeded");
    TEST_ASSERT (eduroute_get_dag_node_count (engine) == 0, "Initial DAG node count is 0");

    // Register test question
    eduroute_register_question (engine, 1, 0, 1000.0f, 15000, 2);
    eduroute_register_question (engine, 2, 0, 1200.0f, 20000, 0);

    // Ingest student MCQ submission
    core::u8 wire_buf[512];
    core::usize sz1      = eduroute_build_wire_packet (engine, 5, 1, 0 /* MCQ */, 2 /* option C */, 3000, nullptr, 0,
                                                       wire_buf, sizeof (wire_buf));
    EduIngestResult res1 = eduroute_ingest_packet (engine, wire_buf, sz1, 100);
    TEST_ASSERT (res1.status_code == 0, "MCQ packet ingest succeeded");
    TEST_ASSERT (eduroute_get_dag_node_count (engine) == 1, "DAG node count incremented to 1");

    // Inspect latest DAG state for (student 5, question 1)
    uint64_t parent_h = 0, node_h = 0;
    char ans_buf[256]{};
    size_t ans_len = 0;
    int ok         = eduroute_get_dag_latest (engine, 5, 1, &parent_h, &node_h, ans_buf, sizeof (ans_buf), &ans_len);
    TEST_ASSERT (ok == 1, "eduroute_get_dag_latest returns true for MCQ submission");
    TEST_ASSERT (ans_len == 1, "MCQ answer length is 1");
    TEST_ASSERT (ans_buf[0] == 'C', "MCQ answer option is 'C'");

    // Ingest first revision of a descriptive answer
    const char *text_v1 = "Binary search tree has O(log n) search time on average";
    core::usize sz2 =
            eduroute_build_wire_packet (engine, 5, 2, 1 /* TextDelta */, 0xFF, 6000, text_v1,
                                        static_cast<core::u32> (std::strlen (text_v1)), wire_buf, sizeof (wire_buf));
    eduroute_ingest_packet (engine, wire_buf, sz2, 200);
    TEST_ASSERT (eduroute_get_dag_node_count (engine) == 2, "DAG node count incremented to 2");
    TEST_ASSERT (eduroute_get_dag_history_size (engine, 5, 2) == 1, "History size for (5, 2) is 1");

    // Student edits answer to add balancing details
    const char *text_v2 = "Binary search tree with self-balancing guarantees O(log n) worst case";
    core::usize sz3 =
            eduroute_build_wire_packet (engine, 5, 2, 1 /* TextDelta */, 0xFF, 11000, text_v2,
                                        static_cast<core::u32> (std::strlen (text_v2)), wire_buf, sizeof (wire_buf));
    eduroute_ingest_packet (engine, wire_buf, sz3, 300);
    TEST_ASSERT (eduroute_get_dag_node_count (engine) == 3, "DAG node count incremented to 3");
    TEST_ASSERT (eduroute_get_dag_history_size (engine, 5, 2) == 2, "History size for (5, 2) updated to 2");

    // DAG head should now yield revision v2
    ok = eduroute_get_dag_latest (engine, 5, 2, &parent_h, &node_h, ans_buf, sizeof (ans_buf), &ans_len);
    TEST_ASSERT (ok == 1, "eduroute_get_dag_latest returns true for revised text");
    TEST_ASSERT (ans_len == std::strlen (text_v2), "Revised answer length matches");
    TEST_ASSERT (std::memcmp (ans_buf, text_v2, ans_len) == 0, "Retrieved latest answer text matches text_v2");

    // Tear down and resurrect engine to verify file replay
    eduroute_destroy (engine);

    EduRouteEngine *reopened = eduroute_create (50, 10, wal_path);
    TEST_ASSERT (reopened != nullptr, "Recreated EduRouteEngine from disk files");
    TEST_ASSERT (eduroute_get_dag_node_count (reopened) == 3, "Reopened engine recovered all 3 DAG nodes");

    ok = eduroute_get_dag_latest (reopened, 5, 2, &parent_h, &node_h, ans_buf, sizeof (ans_buf), &ans_len);
    TEST_ASSERT (ok == 1, "Recovered latest DAG answer on reopened engine");
    TEST_ASSERT (std::memcmp (ans_buf, text_v2, ans_len) == 0, "Answer text preserved across engine reboot");

    eduroute_destroy (reopened);

    std::remove (wal_path);
    std::remove (dag_path);
}

// -----------------------------------------------------------------------------
// Suite 9: C ABI Huffman Compression & Offline Disk Recovery Pipeline
// -----------------------------------------------------------------------------
static void test_c_api_offline_recovery_compression () {
    TEST_SUITE_START ("C ABI Huffman Compression & Offline Disk Recovery");

    const char *wal_path      = "/tmp/test_eduroute_offline_sync.bin";
    const char *dag_path      = "/tmp/test_eduroute_offline_sync.bin.dag";
    const char *ring_wal_path = "/tmp/test_eduroute_offline_sync.bin.wal";
    std::remove (wal_path);
    std::remove (dag_path);
    std::remove (ring_wal_path);

    // Set up test engine backed by temporary files
    EduRouteEngine *engine = eduroute_create (50, 10, wal_path);
    TEST_ASSERT (engine != nullptr, "Engine created with WAL/DAG backing");
    TEST_ASSERT (eduroute_wal_pending_count (engine) == 0, "Initial WAL pending count is 0");
    TEST_ASSERT (eduroute_wal_last_sequence (engine) == 0, "Initial WAL last sequence is 0");

    // Register questions (one objective, one essay)
    eduroute_register_question (engine, 1, 0, 1000.0f, 15000, 2); // Q1 MCQ
    eduroute_register_question (engine, 2, 0, 1200.0f, 20000, 0); // Q2 Descriptive

    // Huffman delta compression on incremental typing
    const char *text_draft1 = "Hash tables use open addressing with quadratic probing";
    const char *text_draft2 = "Hash tables use open addressing with quadratic probing to resolve collisions";

    size_t orig_sz = 0, bit_len = 0;
    size_t comp_bytes = eduroute_compress_delta (engine, 2, text_draft1, text_draft2, &orig_sz, &bit_len);
    TEST_ASSERT (orig_sz > 0, "Original delta size is greater than 0");
    TEST_ASSERT (bit_len > 0, "Huffman bit length is greater than 0");
    TEST_ASSERT (comp_bytes > 0 && comp_bytes <= orig_sz + 10, "Compressed delta bytes produced");

    // Unchanged draft must yield zero compressed bytes
    size_t no_delta_orig = 0, no_delta_bits = 0;
    size_t no_delta_bytes =
            eduroute_compress_delta (engine, 2, text_draft1, text_draft1, &no_delta_orig, &no_delta_bits);
    TEST_ASSERT (no_delta_bytes == 0, "Compressing identical text yields 0 deltas");

    // Normal online submission while network is connected
    EduIngestResult res1 =
            eduroute_ingest_recovered_answer (engine, 12, 1, 0 /* MCQ */, 2 /* option C */, 2000, nullptr, 0, 1000);
    TEST_ASSERT (res1.status_code == 0, "Student 12 MCQ submission ingested");
    TEST_ASSERT (eduroute_wal_last_sequence (engine) == 1, "WAL sequence 1 recorded");
    TEST_ASSERT (eduroute_wal_acknowledged_sequence (engine) == 1, "WAL sequence 1 acknowledged on completion");
    TEST_ASSERT (eduroute_wal_pending_count (engine) == 0, "0 pending WAL records in connected state");
    TEST_ASSERT (eduroute_get_dag_node_count (engine) == 1, "DAG node count is 1");

    // Check compression bandwidth savings ratio
    size_t tot_orig = 0, tot_comp = 0, tot_deltas = 0;
    float savings_pct = 0.0f;
    eduroute_get_compression_stats (engine, &tot_orig, &tot_comp, &tot_deltas, &savings_pct);
    TEST_ASSERT (tot_deltas >= 1, "At least 1 delta recorded in telemetry");

    // Student goes offline due to Wi-Fi dropout; edits get buffered to WAL
    uint64_t seq2  = 0;
    int append_ok2 = eduroute_wal_append_unacked (engine, 12, 2, 2500, "", text_draft1, &seq2);
    TEST_ASSERT (append_ok2 == 1, "Offline unacked draft 1 appended to disk WAL");
    TEST_ASSERT (seq2 == 2, "Sequence 2 assigned to offline draft 1");

    // Second offline edit queued locally
    uint64_t seq3  = 0;
    int append_ok3 = eduroute_wal_append_unacked (engine, 12, 2, 4500, text_draft1, text_draft2, &seq3);
    TEST_ASSERT (append_ok3 == 1, "Offline unacked revision 2 appended to disk WAL");
    TEST_ASSERT (seq3 == 3, "Sequence 3 assigned to offline revision 2");

    // Two unacknowledged entries should be sitting in WAL ring buffer
    TEST_ASSERT (eduroute_wal_pending_count (engine) == 2, "WAL pending count is 2 unacknowledged records");
    TEST_ASSERT (eduroute_wal_last_sequence (engine) == 3, "WAL last sequence is 3");
    TEST_ASSERT (eduroute_wal_acknowledged_sequence (engine) == 1, "WAL acknowledged sequence remains 1");

    // Sudden power cut / process termination while offline
    eduroute_destroy (engine);

    // Engine boots back up after machine restarts
    EduRouteEngine *recovered_eng = eduroute_create (50, 10, wal_path);
    TEST_ASSERT (recovered_eng != nullptr, "Reopened EduRouteEngine from disk files");
    TEST_ASSERT (eduroute_wal_pending_count (recovered_eng) == 2,
                 "Engine recovered 2 unacknowledged records from disk WAL");
    TEST_ASSERT (eduroute_wal_last_sequence (recovered_eng) == 3, "Recovered last sequence is 3");
    TEST_ASSERT (eduroute_wal_acknowledged_sequence (recovered_eng) == 1, "Recovered acknowledged sequence is 1");

    // Verify oldest pending unacked record metadata
    uint64_t rec_seq = 0, rec_ts = 0;
    uint32_t rec_sid = 0;
    size_t rec_orig = 0, rec_comp_b = 0, rec_deltas = 0;
    int peek_ok =
            eduroute_wal_peek_next (recovered_eng, &rec_seq, &rec_sid, &rec_ts, &rec_orig, &rec_comp_b, &rec_deltas);
    TEST_ASSERT (peek_ok == 1, "Peek next unacknowledged record succeeded");
    TEST_ASSERT (rec_seq == 2, "Oldest unacknowledged record is sequence 2");
    TEST_ASSERT (rec_sid == 12, "Unacknowledged record belongs to student 12");
    TEST_ASSERT (rec_deltas == 1, "Unacknowledged record contains 1 compressed delta");

    // Decompress and replay seq 2 onto server state
    char rec_buf[512]{};
    size_t rec_len  = 0;
    int recover_ok1 = eduroute_wal_recover_and_apply_next (recovered_eng, &rec_seq, &rec_sid, rec_buf, sizeof (rec_buf),
                                                           &rec_len);
    TEST_ASSERT (recover_ok1 == 1, "Recovered sequence 2 from disk");
    TEST_ASSERT (rec_seq == 2, "Recovered record sequence is 2");
    TEST_ASSERT (rec_sid == 12, "Recovered record student ID is 12");
    TEST_ASSERT (rec_len == std::strlen (text_draft1), "Decompressed text length matches draft 1");
    TEST_ASSERT (std::memcmp (rec_buf, text_draft1, rec_len) == 0, "Decompressed text matches draft 1 exactly");
    TEST_ASSERT (eduroute_wal_pending_count (recovered_eng) == 1, "Pending count decreased to 1");
    TEST_ASSERT (eduroute_wal_acknowledged_sequence (recovered_eng) == 2, "Acknowledged sequence advanced to 2");

    // Decompress and replay seq 3 (revision) onto server state
    int recover_ok2 = eduroute_wal_recover_and_apply_next (recovered_eng, &rec_seq, &rec_sid, rec_buf, sizeof (rec_buf),
                                                           &rec_len);
    TEST_ASSERT (recover_ok2 == 1, "Recovered sequence 3 from disk");
    TEST_ASSERT (rec_seq == 3, "Recovered record sequence is 3");
    TEST_ASSERT (rec_sid == 12, "Recovered record student ID is 12");
    TEST_ASSERT (rec_len == std::strlen (text_draft2), "Decompressed text length matches revision 2");
    TEST_ASSERT (std::memcmp (rec_buf, text_draft2, rec_len) == 0, "Decompressed text matches revision 2 exactly");
    TEST_ASSERT (eduroute_wal_pending_count (recovered_eng) == 0,
                 "Pending count decreased to 0 (all disk records synced)");
    TEST_ASSERT (eduroute_wal_acknowledged_sequence (recovered_eng) == 3, "Acknowledged sequence advanced to 3");

    eduroute_destroy (recovered_eng);

    std::remove (wal_path);
    std::remove (dag_path);
    std::remove (ring_wal_path);
}

// -----------------------------------------------------------------------------
// Main Test Runner
// -----------------------------------------------------------------------------
int main () {
    std::printf ("===============================================================\n");
    std::printf ("       EduRoute Engine Extensive Unit Test Suite    \n");
    std::printf ("===============================================================\n\n");

    // Division 0 / Core Primitives
    test_core_arena ();
    test_core_dyn_array ();
    test_core_hash_map ();
    test_core_dsu ();
    test_core_string_view ();
    test_core_bit_stream ();

    // Division 2 / Anti-Cheat
    test_anticheat_kmer_winnow ();
    test_anticheat_anomaly_stats ();
    test_anticheat_collusion_graph ();

    // Division 3 / Adaptive Ranking
    test_rank_fenwick_score ();
    test_rank_order_stat_avl ();
    test_rank_irt_selector ();

    // Division 1 / Sync Engine
    test_sync_merkle_dag ();
    test_sync_huffman_delta ();
    test_sync_wal_ring ();

    // C ABI & Integration
    test_c_api_integration ();
    test_c_api_merkle_dag ();
    test_c_api_offline_recovery_compression ();

    std::printf ("\n===============================================================\n");
    std::printf (" Test Results: %d/%d assertions PASSED (%d failed) across %d suites\n",
                 g_total_assertions - g_failed_assertions, g_total_assertions, g_failed_assertions, g_suites_run);
    std::printf ("===============================================================\n");

    return (g_failed_assertions == 0) ? 0 : 1;
}
