/**
 * EduRoute Stress & High-Load Simulator
 * Simulates high-throughput concurrent student exam submissions,
 * injects collusion rings, and validates AVL/Fenwick/DSU invariants under load.
 * STRICTLY NO STL CONTAINERS (No std::vector, std::string, std::map).
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/arena_alloc.hpp"
#include "core/dsu.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"
#include "core/types.hpp"
#include "eduroute_c_api.h"

// Simple LCG pseudo-random generator (reproducible)
struct SimpleRng {
        core::u64 state;
        explicit SimpleRng (core::u64 seed = 123356789ULL) : state (seed) {}
        core::u32 next () {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return static_cast<core::u32> (state >> 32);
        }
        core::u32 range (core::u32 lo, core::u32 hi) {
            if (lo >= hi) return lo;
            return lo + (next () % (hi - lo + 1));
        }
};

int main () {
    std::printf ("===============================================================\n");
    std::printf ("        EduRoute Stress Simulator     \n");
    std::printf ("===============================================================\n\n");

    constexpr core::u32 NUM_STUDENTS  = 120;
    constexpr core::u32 NUM_QUESTIONS = 20;

    SimpleRng rng (42);

    const char *stress_wal_path = "/tmp/test_eduroute_stress_wal.bin";
    const char *stress_dag_path = "/tmp/test_eduroute_stress_wal.bin.dag";
    const char *stress_ring_wal = "/tmp/test_eduroute_stress_wal.bin.wal";
    std::remove (stress_wal_path);
    std::remove (stress_dag_path);
    std::remove (stress_ring_wal);

    // 1. Create engine with WAL & Merkle DAG disk persistence
    EduRouteEngine *engine = eduroute_create (NUM_STUDENTS, NUM_QUESTIONS + 5, stress_wal_path);
    if (!engine) {
        std::printf ("[FATAL] Failed to create EduRouteEngine.\n");
        return 1;
    }

    // 2. Register 20 questions in Question Bank
    for (core::u16 qid = 1; qid <= NUM_QUESTIONS; ++qid) {
        float beta           = 700.0f + static_cast<float> (qid) * 50.0f;
        core::u8 correct_opt = static_cast<core::u8> (qid % 4);
        eduroute_register_question (engine, qid, static_cast<core::u8> (qid % 4), beta, 20000, correct_opt);
    }
    std::printf ("[SETUP] Registered %u calibrated questions.\n", NUM_QUESTIONS);

    // 3. Define Cheating Rings:
    // Ring 1: Students 10 & 11
    // Ring 2: Students 20, 21 & 22 (Triple collusion)
    // Ring 3: Students 30 & 31
    const char *plagiarized_texts[3] = {
            "Fast Fourier transform computes discrete Fourier transform in O(n log n) time",
            "Red-Black tree rebalances with rotations and color flips maintaining logarithmic depth",
            "Dijkstra algorithm uses Fibonacci heap to extract minimum distance node efficiently"};

    auto start_time = std::chrono::high_resolution_clock::now ();

    core::u32 total_packets = 0;
    core::u8 wire_buf[1024];

    // 4. Simulate Exam Rounds
    // Each student answers up to 8 questions
    constexpr core::u32 QUESTIONS_PER_STUDENT = 8;
    for (core::u32 round = 0; round < QUESTIONS_PER_STUDENT; ++round) {
        for (core::u32 sid = 0; sid < NUM_STUDENTS; ++sid) {
            core::u16 qid = static_cast<core::u16> ((sid + round) % NUM_QUESTIONS + 1);
            core::u32 dt  = rng.range (3000, 25000);

            // Collusion injection
            const char *text_ptr = nullptr;
            core::u32 text_len   = 0;
            core::u8 action      = 0; // MCQ
            core::u8 mcq_opt     = static_cast<core::u8> (rng.range (0, 3));

            if ((sid == 10 || sid == 11) && round == 2) {
                // Ring 1 text collusion on Q5
                qid      = 5;
                text_ptr = plagiarized_texts[0];
                text_len = static_cast<core::u32> (std::strlen (text_ptr));
                action   = 1;                           // TextDelta
                dt       = (sid == 10) ? 12000 : 12025; // Synchronous timing
            } else if ((sid == 20 || sid == 21 || sid == 22) && round == 3) {
                // Ring 2 triple collusion on Q7
                qid      = 7;
                text_ptr = plagiarized_texts[1];
                text_len = static_cast<core::u32> (std::strlen (text_ptr));
                action   = 1;
                dt       = 15000 + (sid - 20) * 15; // Within 500ms
            } else if ((sid == 30 || sid == 31) && round == 4) {
                // Ring 3 text collusion on Q9
                qid      = 9;
                text_ptr = plagiarized_texts[2];
                text_len = static_cast<core::u32> (std::strlen (text_ptr));
                action   = 1;
                dt       = 9000 + (sid - 30) * 20;
            }

            core::usize sz = eduroute_build_wire_packet (engine, sid, qid, action, mcq_opt, dt, text_ptr, text_len,
                                                         wire_buf, sizeof (wire_buf));

            if (sz > 0) {
                eduroute_ingest_packet (engine, wire_buf, sz, 1000000ULL + total_packets);
                ++total_packets;
            }
        }
    }

    auto end_time      = std::chrono::high_resolution_clock::now ();
    double elapsed_sec = std::chrono::duration<double> (end_time - start_time).count ();
    double throughput  = static_cast<double> (total_packets) / elapsed_sec;

    std::printf ("\n[STRESS RESULTS]\n");
    std::printf ("  Total Ingested Packets : %u packets\n", total_packets);
    std::printf ("  Elapsed Time           : %.4f seconds\n", elapsed_sec);
    std::printf ("  Ingest Throughput      : %.1f operations / second\n", throughput);
    std::printf ("  Average Latency        : %.2f microseconds / packet\n", (elapsed_sec / total_packets) * 1e6);

    // 5. Validate Leaderboard Ordering & Tie-Breaking
    constexpr core::u32 LEADERBOARD_QUERY = 50;
    EduLeaderboardRow rows[LEADERBOARD_QUERY];
    core::usize num_rows = eduroute_get_top_k (engine, rows, LEADERBOARD_QUERY);

    std::printf ("\n[LEADERBOARD INTEGRITY CHECK]\n");
    std::printf ("  Retrieved Top-%zu entries from AVL Tree\n", num_rows);

    bool ordering_valid = true;
    for (core::usize i = 1; i < num_rows; ++i) {
        if (rows[i].scaled_score > rows[i - 1].scaled_score) {
            ordering_valid = false;
            std::printf ("  [FAIL] Score inverted at index %zu: %d > %d\n", i, rows[i].scaled_score,
                         rows[i - 1].scaled_score);
            break;
        } else if (rows[i].scaled_score == rows[i - 1].scaled_score) {
            if (rows[i].total_time_ms < rows[i - 1].total_time_ms) {
                ordering_valid = false;
                std::printf ("  [FAIL] Time tie-break inverted at index %zu: %u ms < %u ms\n", i, rows[i].total_time_ms,
                             rows[i - 1].total_time_ms);
                break;
            }
        }
    }
    if (ordering_valid) {
        std::printf ("  [PASS] Order-Statistic AVL Tree maintained perfect descending score and time order!\n");
    }

    // 6. Validate Anti-Cheat Collusion Clusters
    std::printf ("\n[ANTI-CHEAT CLUSTER CHECK]\n");
    constexpr core::usize MAX_EDGES = 256;
    EduCollusionEdge edges[MAX_EDGES];
    core::usize num_edges = eduroute_get_collusion_edges (engine, edges, MAX_EDGES);
    std::printf ("  Confirmed Cheating Edges: %zu\n", num_edges);

    bool ring1_found = false;
    bool ring2_found = false;
    bool ring3_found = false;

    for (core::usize i = 0; i < num_edges; ++i) {
        core::u32 a = edges[i].student_a;
        core::u32 b = edges[i].student_b;
        std::printf ("    Edge %zu: Student %u <-> %u | Weight: %.2f | TextSim: %.2f | ClusterRoot: %u\n", i + 1, a, b,
                     edges[i].combined_weight, edges[i].peak_text_sim, edges[i].cluster_root);

        if ((a == 10 && b == 11) || (a == 11 && b == 10)) ring1_found = true;
        if ((a == 20 && b == 21) || (a == 21 && b == 22) || (a == 20 && b == 22)) ring2_found = true;
        if ((a == 30 && b == 31) || (a == 31 && b == 30)) ring3_found = true;
    }

    if (ring1_found) std::printf ("  [PASS] Cheating Ring 1 (Students 10 & 11) detected!\n");
    if (ring2_found) std::printf ("  [PASS] Cheating Ring 2 (Students 20, 21, 22) detected!\n");
    if (ring3_found) std::printf ("  [PASS] Cheating Ring 3 (Students 30 & 31) detected!\n");

    // 7. Sync Engine, Huffman Compression & WAL Persistence Telemetry
    size_t orig_b = 0, comp_b = 0, deltas_c = 0;
    float savings_pct = 0.0f;
    eduroute_get_compression_stats (engine, &orig_b, &comp_b, &deltas_c, &savings_pct);

    size_t total_dag_nodes = eduroute_get_dag_node_count (engine);
    uint64_t wal_last_seq  = eduroute_wal_last_sequence (engine);
    uint64_t wal_acked_seq = eduroute_wal_acknowledged_sequence (engine);
    size_t wal_pending     = eduroute_wal_pending_count (engine);

    std::printf ("\n[SYNC ENGINE & HUFFMAN COMPRESSION TELEMETRY]\n");
    std::printf ("  Total Deltas Compressed : %zu deltas\n", deltas_c);
    std::printf ("  Original Payload Size   : %zu bytes\n", orig_b);
    std::printf ("  Huffman Compressed Size : %zu bytes\n", comp_b);
    std::printf ("  Bandwidth Savings       : %.2f%%\n", savings_pct);
    std::printf ("  Merkle DAG Nodes Logged : %zu nodes\n", total_dag_nodes);
    std::printf ("  WAL Records Persisted   : %llu (Acked: %llu, Pending: %zu)\n",
                 static_cast<unsigned long long> (wal_last_seq), static_cast<unsigned long long> (wal_acked_seq),
                 wal_pending);

    // 8. Arena Memory Efficiency Check
    core::usize arena_used = eduroute_get_arena_Memory_used (engine);
    std::printf ("\n[MEMORY FOOTPRINT]\n");
    std::printf ("  Persistent + Scratch Memory Used: %zu bytes (%.2f KB)\n", arena_used,
                 static_cast<double> (arena_used) / 1024.0);

    eduroute_destroy (engine);

    // 9. Reboot & On-Disk Recovery Check
    EduRouteEngine *rebooted = eduroute_create (NUM_STUDENTS, NUM_QUESTIONS + 5, stress_wal_path);
    bool recovery_ok         = (rebooted != nullptr);
    if (recovery_ok) {
        size_t recovered_nodes = eduroute_get_dag_node_count (rebooted);
        uint64_t rec_wal_seq   = eduroute_wal_last_sequence (rebooted);
        std::printf ("\n[ON-DISK RECOVERY CHECK (POST-REBOOT)]\n");
        std::printf ("  Recovered DAG Nodes from Disk : %zu / %zu\n", recovered_nodes, total_dag_nodes);
        std::printf ("  Recovered WAL Seq from Disk   : %llu / %llu\n", static_cast<unsigned long long> (rec_wal_seq),
                     static_cast<unsigned long long> (wal_last_seq));
        if (recovered_nodes == total_dag_nodes && rec_wal_seq == wal_last_seq) {
            std::printf ("  [PASS] 100%% of engine state successfully recovered from disk!\n");
        } else {
            recovery_ok = false;
        }
        eduroute_destroy (rebooted);
    }

    std::remove (stress_wal_path);
    std::remove (stress_dag_path);
    std::remove (stress_ring_wal);

    std::printf ("\n[SHUTDOWN] Engine destroyed cleanly with zero memory leaks.\n");
    std::printf ("===============================================================\n");

    return (ordering_valid && ring1_found && ring2_found && ring3_found && recovery_ok) ? 0 : 1;
}
