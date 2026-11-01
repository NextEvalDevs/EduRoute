#include "eduroute_c_api.h"

#include <cstdio>
#include <cstring>

#include "adaptive_rank/fenwick_score.hpp"
#include "adaptive_rank/irt_selector.hpp"
#include "adaptive_rank/order_stat_avl.hpp"

// Bring OrderStatAvl into scope from its nested namespace.
namespace adaptive_rank {
    using OrderStatAvl = core::adaptive_rank::OrderStatAvl;
} // namespace adaptive_rank
#include "anticheat/anomaly_stats.hpp"
#include "anticheat/collusion_graph.hpp"
#include "anticheat/kmer_winnow.hpp"
#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"
#include "core/types.hpp"
#include "sync_engine/huffman_delta.hpp"
#include "sync_engine/merkle_dag.hpp"
#include "sync_engine/wal_ring.hpp"

// Internal student state tracked in persistent arena
struct StudentState {
        core::Hash64 dag_head;               // latest Merkle node hash for this student
        core::u32 total_time_ms;             // running sum of answer times
        core::i32 scaled_score;              // scaled 0..10000 score
        core::LeaderboardKey lb_key;         // composite sort key in AVL tree
        bool in_avl;                         // is student currently on the leaderboard
        adaptive_rank::ExamSession *session; // per-student adaptive session
};

// Main engine context - holds arenas and subsystem instances
struct EduRouteEngine {
        // Arenas: persistent holds exam state, scratch is wiped per packet
        core::Arena pers_arena;
        core::Arena scratch_arena;

        // Subsystems:
        // Div 1 - Merkle history & disk WAL ring
        sync_engine::MerkleDAG merkle_dag;
        sync_engine::WriteAheadLog wal;

        // Compression counters for dashboard telemetry
        core::usize total_original_bytes;
        core::usize total_compressed_bytes;
        core::usize total_deltas_compressed;

        // Div 3 - Adaptive testing, Fenwick percentile, and AVL leaderboard
        adaptive_rank::QuestionBank question_bank;
        adaptive_rank::FenwickScore fenwick;
        adaptive_rank::OrderStatAvl avl;

        // Div 2 - Anti-cheat detection (winnowing, timing anomalies, collusion DSU)
        core::anticheat::KmerWinnow kmer;
        core::anticheat::AnomalyStats anomaly;
        core::anticheat::CollusionGraph collusion;

        // Student table
        StudentState *students;
        core::u32 max_students;
        core::u32 max_questions;

        // Raw append log
        FILE *wal_fp;

        EduRouteEngine (core::usize pers_bytes, core::usize scratch_bytes, core::u32 max_s, core::u32 max_q,
                        const char *wal_path) :
            pers_arena (pers_bytes), scratch_arena (scratch_bytes), merkle_dag (pers_arena), wal (pers_arena),
            total_original_bytes (0), total_compressed_bytes (0), total_deltas_compressed (0),
            kmer (pers_arena, 32768u), max_students (max_s), max_questions (max_q), wal_fp (nullptr) {
            students = nullptr;
            if (wal_path && wal_path[0] != '\0') {
                wal_fp = std::fopen (wal_path, "ab");
                char dag_path[512];
                std::snprintf (dag_path, sizeof (dag_path), "%s.dag", wal_path);
                merkle_dag.open (dag_path);
                char wal_ring_path[512];
                std::snprintf (wal_ring_path, sizeof (wal_ring_path), "%s.wal", wal_path);
                wal.open (wal_ring_path);
            }
        }

        ~EduRouteEngine () {
            if (wal_fp) {
                std::fclose (wal_fp);
                wal_fp = nullptr;
            }
            wal.close ();
            merkle_dag.close ();
        }
};

namespace {

    // FNV-1a hash over raw memory block
    static core::Hash64 fnv1a (const void *data, core::usize len) noexcept {
        const auto *bytes = static_cast<const core::u8 *> (data);
        core::Hash64 h    = 14695981039346656037ULL;
        for (core::usize i = 0; i < len; ++i) {
            h ^= static_cast<core::Hash64> (bytes[i]);
            h *= 1099511628211ULL;
        }
        return h;
    }

    // Packet authentication: 16-bit MAC tag computed over header (minus mac field) + payload
    static core::u16 compute_mac16 (const EduWireHeader *hdr, const core::u8 *payload, core::u16 payload_len) noexcept {
        constexpr core::usize HDR_NO_MAC = sizeof (EduWireHeader) - sizeof (core::u16);
        core::Hash64 h                   = fnv1a (hdr, HDR_NO_MAC);

        const auto *pb = static_cast<const core::u8 *> (static_cast<const void *> (payload));
        for (core::u16 i = 0; i < payload_len; ++i) {
            h ^= static_cast<core::Hash64> (pb[i]);
            h *= 1099511628211ULL;
        }
        return static_cast<core::u16> (h & 0xFFFFu);
    }

    static bool valid_student (const EduRouteEngine *eng, core::u32 sid) noexcept { return sid < eng->max_students; }

    static EduIngestResult make_error_result (core::u8 status) noexcept {
        EduIngestResult r{};
        r.status_code      = status;
        r.next_question_id = 0xFFFFu;
        return r;
    }

} // anonymous namespace

extern "C" EduRouteEngine *eduroute_create (uint32_t max_students, uint32_t max_questions, const char *wal_filepath) {
    if (max_students == 0 || max_questions == 0) return nullptr;

    // Arena budget estimation:
    // - ~4KB per student for adaptive ExamSession + StudentState struct
    // - question bank, Fenwick tree, and AVL tree tables
    // - ~8MB for winnowing index and collusion graph
    // - 1MB extra safety buffer
    const core::usize per_student_bytes = 4096u + sizeof (StudentState) + 64u;
    const core::usize bank_bytes        = adaptive_rank::QuestionBank::required_bytes (max_questions);
    const core::usize fenwick_bytes     = adaptive_rank::FenwickScore::required_bytes ();
    const core::usize avl_bytes         = adaptive_rank::OrderStatAvl::required_bytes (max_students);
    const core::usize anticheat_bytes   = 8u * 1024u * 1024u;
    const core::usize pers_bytes = bank_bytes + fenwick_bytes + avl_bytes + anticheat_bytes +
                                   static_cast<core::usize> (max_students) * per_student_bytes + 1u * 1024u * 1024u;

    const core::usize scratch_bytes = 2u * 1024u * 1024u; // 2MB scratch buffer per packet

    void *raw = std::malloc (sizeof (EduRouteEngine));
    if (!raw) return nullptr;

    auto *eng = ::new (raw) EduRouteEngine (pers_bytes, scratch_bytes, max_students, max_questions, wal_filepath);

    // Initialize adaptive test & ranking structures
    if (eng->question_bank.init (eng->pers_arena, max_questions) != core::StatusCode::Ok) {
        eng->~EduRouteEngine ();
        std::free (raw);
        return nullptr;
    }
    if (eng->fenwick.init (eng->pers_arena) != core::StatusCode::Ok) {
        eng->~EduRouteEngine ();
        std::free (raw);
        return nullptr;
    }
    if (eng->avl.init (eng->pers_arena, max_students) != core::StatusCode::Ok) {
        eng->~EduRouteEngine ();
        std::free (raw);
        return nullptr;
    }

    // Initialize anti-cheat graphs and anomaly trackers
    if (eng->anomaly.init (eng->pers_arena, static_cast<core::usize> (max_students) * 4u) != core::StatusCode::Ok) {
        eng->~EduRouteEngine ();
        std::free (raw);
        return nullptr;
    }
    if (eng->collusion.init (eng->pers_arena, max_students, static_cast<core::usize> (max_students) * 2u) !=
        core::StatusCode::Ok) {
        eng->~EduRouteEngine ();
        std::free (raw);
        return nullptr;
    }

    // Initialize per-student metadata table in persistent arena
    eng->students = static_cast<StudentState *> (eng->pers_arena.allocate (
            static_cast<core::usize> (max_students) * sizeof (StudentState), alignof (StudentState)));
    if (!eng->students) {
        eng->~EduRouteEngine ();
        std::free (raw);
        return nullptr;
    }
    for (core::u32 i = 0; i < max_students; ++i) {
        StudentState &s = eng->students[i];
        s.dag_head      = core::ROOT_DAG_HASH;
        s.total_time_ms = 0u;
        s.scaled_score  = 0;
        s.lb_key        = core::LeaderboardKey{0, 0u, i};
        s.in_avl        = false;
        s.session       = nullptr;
    }

    return eng;
}

extern "C" void eduroute_destroy (EduRouteEngine *engine) {
    if (!engine) return;
    engine->~EduRouteEngine ();
    std::free (engine);
}

// ============================================================================
//  Question registration
// ============================================================================

extern "C" void eduroute_register_question (EduRouteEngine *engine, uint16_t question_id, uint8_t topic_id,
                                            float difficulty_beta, uint32_t expected_time_ms,
                                            uint8_t correct_mcq_option) {
    if (!engine) return;
    (void) expected_time_ms; // stored in ExamSession timing; not used in bank directly

    // Linear map: scale exam difficulty (600..1800) to standard IRT range (-4.0..+4.0)
    // 1200 is neutral baseline (theta 0), 150 points ~= 1 standard deviation
    const double d_irt = (static_cast<double> (difficulty_beta) - 1200.0) / 150.0;

    engine->question_bank.add_calibrated (static_cast<core::QuestionId> (question_id),
                                          static_cast<core::TopicId> (topic_id), d_irt, correct_mcq_option,
                                          1.0 /* discrimination parameter */);
}

// ============================================================================
//  Wire packet builder (client/simulator helper)
// ============================================================================

extern "C" size_t eduroute_build_wire_packet (EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                              uint8_t action_type, uint8_t mcq_option, uint32_t monotonic_dt_ms,
                                              const char *new_full_text, uint32_t new_text_len, uint8_t *out_wire_buf,
                                              size_t out_buf_cap) {
    if (!engine || !out_wire_buf) return 0u;

    const core::usize total = sizeof (EduWireHeader) + static_cast<core::usize> (new_text_len);
    if (total > out_buf_cap) return 0u;

    // Look up the previous DAG head for this student.
    const core::Hash64 parent_hash =
            valid_student (engine, student_id) ? engine->students[student_id].dag_head : core::ROOT_DAG_HASH;

    // Compute node hash: FNV-1a of (parent_hash | student | question | text).
    core::Hash64 node_hash = parent_hash;
    node_hash ^= fnv1a (&student_id, sizeof (student_id));
    node_hash ^= fnv1a (&question_id, sizeof (question_id));
    if (new_full_text && new_text_len > 0) { node_hash ^= fnv1a (new_full_text, new_text_len); }

    EduWireHeader hdr{};
    hdr.parent_hash     = parent_hash;
    hdr.node_hash       = node_hash;
    hdr.student_id      = student_id;
    hdr.question_id     = question_id;
    hdr.action_type     = action_type;
    hdr.mcq_option      = mcq_option;
    hdr.monotonic_dt_ms = monotonic_dt_ms;
    hdr.payload_len     = static_cast<uint16_t> (new_text_len);
    hdr.mac_tag16       = 0u;

    // Copy text payload (raw, no compression in this helper).
    if (new_full_text && new_text_len > 0) {
        std::memcpy (out_wire_buf + sizeof (EduWireHeader), new_full_text, new_text_len);
    }

    hdr.mac_tag16 = compute_mac16 (&hdr, out_wire_buf + sizeof (EduWireHeader), static_cast<uint16_t> (new_text_len));
    std::memcpy (out_wire_buf, &hdr, sizeof (EduWireHeader));
    return total;
}

// ============================================================================
//  Core hot-path: eduroute_ingest_packet
// ============================================================================

extern "C" EduIngestResult eduroute_ingest_packet (EduRouteEngine *engine, const uint8_t *wire_packet,
                                                   size_t packet_len, uint64_t server_recv_ms) {
    if (!engine || !wire_packet || packet_len < sizeof (EduWireHeader)) {
        return make_error_result (static_cast<core::u8> (core::StatusCode::CorruptedPayload));
    }

    // 1. Validate header & check MAC authentication tag
    EduWireHeader hdr{};
    std::memcpy (&hdr, wire_packet, sizeof (EduWireHeader));

    const core::u8 *payload     = wire_packet + sizeof (EduWireHeader);
    const core::u16 payload_len = hdr.payload_len;

    if (sizeof (EduWireHeader) + static_cast<core::usize> (payload_len) > packet_len) {
        return make_error_result (static_cast<core::u8> (core::StatusCode::CorruptedPayload));
    }

    const core::u16 expected_mac = compute_mac16 (&hdr, payload, payload_len);
    if (expected_mac != hdr.mac_tag16) {
        return make_error_result (static_cast<core::u8> (core::StatusCode::HashChainMismatch));
    }

    const core::StudentId sid  = hdr.student_id;
    const core::QuestionId qid = hdr.question_id;

    if (!valid_student (engine, sid)) {
        return make_error_result (static_cast<core::u8> (core::StatusCode::InvalidState));
    }

    StudentState &st = engine->students[sid];

    // 2. Merkle chain check: parent must match this student's latest node hash
    if (hdr.parent_hash != st.dag_head && st.dag_head != core::ROOT_DAG_HASH) {
        // (first submission always starts from ROOT_DAG_HASH)
        return make_error_result (static_cast<core::u8> (core::StatusCode::HashChainMismatch));
    }

    st.dag_head = hdr.node_hash;

    // 3. Raw WAL dump for full wire replay
    if (engine->wal_fp) { std::fwrite (wire_packet, 1u, sizeof (EduWireHeader) + payload_len, engine->wal_fp); }

    const auto action = static_cast<core::ActionType> (hdr.action_type);

    // 4. Update Merkle DAG history & compute Huffman delta for WAL ring
    {
        core::StringView dag_answer;
        char mcq_str[2] = {0, 0};
        if (action == core::ActionType::McqSelect) {
            if (hdr.mcq_option < 4) {
                mcq_str[0] = static_cast<char> ('A' + hdr.mcq_option);
                dag_answer = core::StringView (mcq_str, 1);
            }
        } else if (payload_len > 0) {
            dag_answer =
                    core::StringView (reinterpret_cast<const char *> (payload), static_cast<core::usize> (payload_len));
        }

        // Compress delta against prior answer snapshot
        if (engine->wal.is_open ()) {
            sync_engine::MerkleNode prev_node{};
            core::StringView prev_ans{};
            if (engine->merkle_dag.latest (sid, qid, prev_node)) { prev_ans = prev_node.answer; }

            sync_engine::TextDeltaArray deltas (engine->scratch_arena);
            sync_engine::generate_text_delta (qid, prev_ans, dag_answer, deltas);

            if (deltas.size () > 0) {
                sync_engine::CompressedTextDelta comp =
                        sync_engine::compress_text_delta (engine->scratch_arena, deltas);
                if (comp.valid) {
                    core::u64 seq              = 0;
                    const core::TimestampMs ts = server_recv_ms ? server_recv_ms : hdr.monotonic_dt_ms;
                    if (engine->wal.append (sid, ts, comp, &seq)) {
                        engine->total_original_bytes += comp.original_size;
                        engine->total_compressed_bytes += comp.bytes.size ();
                        engine->total_deltas_compressed += 1;
                        engine->wal.acknowledge (seq);
                    }
                }
            }
        }

        engine->merkle_dag.append (sid, qid, hdr.monotonic_dt_ms, dag_answer, nullptr);
    }

    // Wipe scratch arena for clean working memory
    engine->scratch_arena.reset ();

    // Ensure student has an active adaptive session (first packet bootstrap)
    if (st.session == nullptr && engine->question_bank.ready () && engine->question_bank.size () > 0u) {
        void *sess_mem =
                engine->pers_arena.allocate (sizeof (adaptive_rank::ExamSession), alignof (adaptive_rank::ExamSession));
        if (sess_mem) {
            auto *sess = ::new (sess_mem) adaptive_rank::ExamSession ();
            adaptive_rank::SelectorConfig cfg{};
            cfg.max_questions = 20u;
            cfg.min_questions = 3u;
            cfg.se_stop       = 0.3;
            cfg.seed          = static_cast<core::u64> (sid) * 6364136223846793005ULL + 1442695040888963407ULL;
            if (sess->init (engine->pers_arena, engine->question_bank, sid, cfg) == core::StatusCode::Ok) {
                st.session = sess;
            }
        }
    }

    // Anti-cheat signal 1: time-synchronization spike check (|dt_a - dt_b| < 500ms)
    {
        core::Array<core::anticheat::StatAnomalyMatch> stat_hits (engine->scratch_arena, 16u);
        engine->anomaly.record_dt (sid, qid, hdr.monotonic_dt_ms, engine->scratch_arena, stat_hits);

        bool dummy_flag = false;
        for (core::usize i = 0; i < stat_hits.size (); ++i) {
            const auto &m = stat_hits[i];
            engine->collusion.update_stat (sid, m.other_student, m.time_score, 0.0f, dummy_flag);
        }
    }

    // Anti-cheat signal 2: text winnowing fingerprint match on essays / code
    if (action == core::ActionType::TextDelta || action == core::ActionType::FinalSubmit) {
        core::StringView text_sv (reinterpret_cast<const char *> (payload), static_cast<core::usize> (payload_len));

        core::Array<core::Hash64> fps (engine->scratch_arena, 64u);
        core::Array<core::anticheat::TextSimMatch> text_hits (engine->scratch_arena, 16u);

        core::anticheat::KmerWinnow::extract_fingerprints (text_sv, engine->scratch_arena, fps);
        engine->kmer.index_and_match (sid, qid, fps, engine->scratch_arena, text_hits);

        bool dummy_flag = false;
        for (core::usize i = 0; i < text_hits.size (); ++i) {
            const auto &m = text_hits[i];
            engine->collusion.update_text (sid, m.other_student, m.cont_score, dummy_flag);
        }
    }

    // Anti-cheat signal 3: rare shared wrong MCQ options
    bool mcq_was_correct = false;
    if (action == core::ActionType::McqSelect || action == core::ActionType::FinalSubmit) {
        const core::u8 chosen            = hdr.mcq_option;
        const adaptive_rank::Question *q = engine->question_bank.find (qid);
        const core::u8 correct           = q ? q->correct_option : 0xFFu;

        if (q != nullptr) {
            mcq_was_correct = (chosen == correct);

            core::Array<core::anticheat::StatAnomalyMatch> err_hits (engine->scratch_arena, 16u);
            engine->anomaly.record_mcq (sid, qid, chosen, correct, engine->scratch_arena, err_hits);

            bool dummy_flag = false;
            for (core::usize i = 0; i < err_hits.size (); ++i) {
                const auto &m = err_hits[i];
                engine->collusion.update_stat (sid, m.other_student, 0.0f, m.err_score, dummy_flag);
            }
        }
    }

    // Update IRT ability rating, Fenwick percentiles, and AVL leaderboard
    core::i32 new_score       = st.scaled_score;
    core::f64 new_theta       = 0.0;
    core::QuestionId next_qid = core::INVALID_QUESTION_ID;

    if (st.session != nullptr && !st.session->finished ()) {
        if (action == core::ActionType::McqSelect || action == core::ActionType::FinalSubmit) {
            const core::i32 old_score = st.scaled_score;
            st.session->submit_option (qid, hdr.mcq_option, hdr.monotonic_dt_ms, nullptr);

            new_score        = st.session->scaled_score ();
            new_theta        = st.session->ability ();
            st.scaled_score  = new_score;
            st.total_time_ms = st.session->total_time_ms ();

            // Adjust student position in Fenwick tree
            if (st.in_avl) {
                engine->fenwick.update (old_score, new_score);
            } else {
                engine->fenwick.add (new_score);
            }

            // Adjust student key in Order-Statistic AVL tree
            const core::LeaderboardKey new_key = st.session->leaderboard_key ();
            if (st.in_avl) {
                engine->avl.update (st.lb_key, new_key);
            } else {
                engine->avl.insert (new_key);
                st.in_avl = true;
            }
            st.lb_key = new_key;
        }

        // Fetch next adaptive question if exam is still in progress
        if (!st.session->finished ()) { st.session->next_question (&next_qid); }
    }

    // Check collusion status via DSU
    const bool is_flagged      = engine->collusion.is_flagged (sid);
    const core::u32 cluster_rt = engine->collusion.cluster_root (sid);

    // Build synchronous client response
    EduIngestResult res{};
    res.status_code      = static_cast<core::u8> (core::StatusCode::Ok);
    res.is_flagged_ring  = is_flagged ? 1u : 0u;
    res.next_question_id = static_cast<uint16_t> (next_qid);
    res.current_score    = new_score;
    res.updated_theta    = static_cast<float> (new_theta);
    res.cluster_root_id  = cluster_rt;

    // Fast O(log N) rank and percentile queries
    if (st.in_avl) {
        core::u32 rank_out = 0u;
        engine->avl.rank_of (st.lb_key, &rank_out);
        res.live_rank       = rank_out;
        res.live_percentile = static_cast<float> (engine->fenwick.percentile (new_score));
    } else {
        res.live_rank       = engine->avl.size () + 1u;
        res.live_percentile = 0.0f;
    }

    (void) server_recv_ms;
    (void) mcq_was_correct;
    return res;
}

// ============================================================================
//  Telemetry / Dashboard APIs
// ============================================================================

extern "C" size_t eduroute_get_top_k (EduRouteEngine *engine, EduLeaderboardRow *out_rows, size_t k) {
    if (!engine || !out_rows || k == 0u) return 0u;

    // Collect up to k best keys from the AVL tree.
    const core::u32 actual =
            engine->avl.size () < static_cast<core::u32> (k) ? engine->avl.size () : static_cast<core::u32> (k);
    if (actual == 0u) return 0u;

    // Allocate a temporary buffer in the scratch arena.
    engine->scratch_arena.reset ();
    auto *keys = static_cast<core::LeaderboardKey *> (engine->scratch_arena.allocate (
            static_cast<core::usize> (actual) * sizeof (core::LeaderboardKey), alignof (core::LeaderboardKey)));
    if (!keys) return 0u;

    const core::u32 written = engine->avl.top_k (actual, keys);
    for (core::u32 i = 0; i < written; ++i) {
        const core::StudentId sid = keys[i].student_id;
        out_rows[i].rank          = i + 1u;
        out_rows[i].student_id    = sid;
        out_rows[i].scaled_score  = keys[i].scaled_score;
        out_rows[i].total_time_ms = keys[i].total_time_ms;
        out_rows[i].is_flagged    = engine->collusion.is_flagged (sid) ? 1u : 0u;

        // Lookup percentile directly from Fenwick prefix sums
        out_rows[i].percentile = static_cast<float> (engine->fenwick.percentile (keys[i].scaled_score));

        // Current theta ability rating
        if (valid_student (engine, sid) && engine->students[sid].session != nullptr) {
            out_rows[i].theta_ability = static_cast<float> (engine->students[sid].session->ability ());
        } else {
            out_rows[i].theta_ability = 0.0f;
        }
    }
    return static_cast<size_t> (written);
}

extern "C" size_t eduroute_get_collusion_edges (EduRouteEngine *engine, EduCollusionEdge *out_edges, size_t max_edges) {
    if (!engine || !out_edges || max_edges == 0u) return 0u;

    // Allocate scratch edge buffer
    engine->scratch_arena.reset ();
    auto *buf = static_cast<core::anticheat::CollusionEdge *> (engine->scratch_arena.allocate (
            max_edges * sizeof (core::anticheat::CollusionEdge), alignof (core::anticheat::CollusionEdge)));
    if (!buf) return 0u;

    const core::usize found = engine->collusion.get_confirmed_edges (buf, max_edges);

    for (core::usize i = 0; i < found; ++i) {
        out_edges[i].student_a        = buf[i].a;
        out_edges[i].student_b        = buf[i].b;
        out_edges[i].cluster_root     = buf[i].cluster_root;
        out_edges[i].combined_weight  = buf[i].combined_weight;
        out_edges[i].peak_text_sim    = buf[i].peak_text_sim;
        out_edges[i].accum_time_score = buf[i].accum_time_score;
        out_edges[i].accum_err_score  = buf[i].accum_err_score;
        out_edges[i].collision_count  = buf[i].collision_count;
    }
    return static_cast<size_t> (found);
}

extern "C" size_t eduroute_get_arena_Memory_used (const EduRouteEngine *engine) {
    if (!engine) return 0u;
    return engine->pers_arena.used () + engine->scratch_arena.used ();
}

// Merkle DAG telemetry queries
extern "C" size_t eduroute_get_dag_node_count (const EduRouteEngine *engine) {
    if (!engine) return 0u;
    return engine->merkle_dag.node_count ();
}

extern "C" int eduroute_get_dag_latest (const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                        uint64_t *out_parent_hash, uint64_t *out_node_hash, char *out_answer_buf,
                                        size_t buf_cap, size_t *out_answer_len) {
    if (!engine) return 0;
    sync_engine::MerkleNode node{};
    if (!engine->merkle_dag.latest (static_cast<core::StudentId> (student_id),
                                    static_cast<core::QuestionId> (question_id), node)) {
        return 0;
    }
    if (out_parent_hash) *out_parent_hash = node.parent_hash;
    if (out_node_hash) *out_node_hash = node.node_hash;
    if (out_answer_len) *out_answer_len = node.answer.size ();
    if (out_answer_buf && buf_cap > 0) {
        const core::usize to_copy = node.answer.size () < buf_cap ? node.answer.size () : buf_cap - 1;
        if (node.answer.data () && to_copy > 0) { std::memcpy (out_answer_buf, node.answer.data (), to_copy); }
        out_answer_buf[to_copy] = '\0';
    }
    return 1;
}

extern "C" size_t eduroute_get_dag_history_size (const EduRouteEngine *engine, uint32_t student_id,
                                                 uint16_t question_id) {
    if (!engine) return 0u;
    return engine->merkle_dag.history_size (static_cast<core::StudentId> (student_id),
                                            static_cast<core::QuestionId> (question_id));
}

// WAL disk persistence & compression status
extern "C" size_t eduroute_wal_pending_count (const EduRouteEngine *engine) {
    if (!engine || !engine->wal.is_open ()) return 0u;
    return engine->wal.pending_count ();
}

extern "C" uint64_t eduroute_wal_last_sequence (const EduRouteEngine *engine) {
    if (!engine || !engine->wal.is_open ()) return 0u;
    return engine->wal.last_sequence ();
}

extern "C" uint64_t eduroute_wal_acknowledged_sequence (const EduRouteEngine *engine) {
    if (!engine || !engine->wal.is_open ()) return 0u;
    return engine->wal.acknowledged_sequence ();
}

extern "C" int eduroute_wal_acknowledge (EduRouteEngine *engine, uint64_t sequence) {
    if (!engine || !engine->wal.is_open ()) return 0;
    return engine->wal.acknowledge (sequence) ? 1 : 0;
}

extern "C" int eduroute_wal_peek_next (const EduRouteEngine *engine, uint64_t *out_sequence, uint32_t *out_student_id,
                                       uint64_t *out_timestamp_ms, size_t *out_original_size,
                                       size_t *out_compressed_bytes, size_t *out_delta_count) {
    if (!engine || !engine->wal.is_open ()) return 0;
    const sync_engine::WalRecord *rec = nullptr;
    if (!engine->wal.peek_next (rec) || !rec) return 0;

    if (out_sequence) *out_sequence = rec->sequence;
    if (out_student_id) *out_student_id = rec->student_id;
    if (out_timestamp_ms) *out_timestamp_ms = rec->timestamp_ms;
    if (out_original_size) *out_original_size = rec->compressed.original_size;
    if (out_compressed_bytes) *out_compressed_bytes = rec->compressed.bytes.size ();
    if (out_delta_count) *out_delta_count = rec->compressed.delta_count;
    return 1;
}

extern "C" int eduroute_wal_append_unacked (EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                            uint64_t timestamp_ms, const char *old_text, const char *new_text,
                                            uint64_t *out_sequence) {
    if (!engine || !engine->wal.is_open ()) return 0;
    engine->scratch_arena.reset ();
    sync_engine::TextDeltaArray deltas (engine->scratch_arena);
    sync_engine::generate_text_delta (question_id, core::StringView (old_text ? old_text : ""),
                                      core::StringView (new_text ? new_text : ""), deltas);
    if (deltas.size () == 0) return 0;
    sync_engine::CompressedTextDelta comp = sync_engine::compress_text_delta (engine->scratch_arena, deltas);
    if (!comp.valid) return 0;

    core::u64 seq = 0;
    if (!engine->wal.append (student_id, timestamp_ms, comp, &seq)) return 0;

    engine->total_original_bytes += comp.original_size;
    engine->total_compressed_bytes += comp.bytes.size ();
    engine->total_deltas_compressed += 1;

    if (out_sequence) *out_sequence = seq;
    // Don't ACK here so recovery test can demonstrate replaying pending frames
    return 1;
}

extern "C" int eduroute_wal_recover_and_apply_next (EduRouteEngine *engine, uint64_t *out_sequence,
                                                    uint32_t *out_student_id, char *out_text_buf, size_t buf_cap,
                                                    size_t *out_text_len) {
    if (!engine || !engine->wal.is_open ()) return 0;
    const sync_engine::WalRecord *rec = nullptr;
    if (!engine->wal.peek_next (rec) || !rec) return 0;

    if (out_sequence) *out_sequence = rec->sequence;
    if (out_student_id) *out_student_id = rec->student_id;

    engine->scratch_arena.reset ();
    core::Array<core::u8> text_storage (engine->scratch_arena, 4096u);
    sync_engine::TextDeltaArray deltas =
            sync_engine::decompress_text_delta (engine->scratch_arena, rec->compressed, text_storage);

    if (deltas.size () > 0) {
        const auto &d = deltas[0];
        if (out_text_len) *out_text_len = d.new_value.size ();
        if (out_text_buf && buf_cap > 0) {
            const core::usize to_copy = d.new_value.size () < buf_cap ? d.new_value.size () : buf_cap - 1;
            if (to_copy > 0 && d.new_value.data ()) { std::memcpy (out_text_buf, d.new_value.data (), to_copy); }
            out_text_buf[to_copy] = '\0';
        }
    } else {
        if (out_text_len) *out_text_len = 0;
        if (out_text_buf && buf_cap > 0) out_text_buf[0] = '\0';
    }

    // Now mark acknowledged after applying
    engine->wal.acknowledge (rec->sequence);
    return 1;
}

extern "C" size_t eduroute_compress_delta (EduRouteEngine *engine, uint32_t answer_id, const char *old_text,
                                           const char *new_text, size_t *out_original_size, size_t *out_bit_length) {
    if (!engine) return 0u;
    engine->scratch_arena.reset ();
    sync_engine::TextDeltaArray deltas (engine->scratch_arena);
    sync_engine::generate_text_delta (answer_id, core::StringView (old_text ? old_text : ""),
                                      core::StringView (new_text ? new_text : ""), deltas);
    if (deltas.size () == 0) {
        if (out_original_size) *out_original_size = 0u;
        if (out_bit_length) *out_bit_length = 0u;
        return 0u;
    }
    sync_engine::CompressedTextDelta comp = sync_engine::compress_text_delta (engine->scratch_arena, deltas);
    if (!comp.valid) return 0u;

    if (out_original_size) *out_original_size = comp.original_size;
    if (out_bit_length) *out_bit_length = comp.bit_length;
    return comp.bytes.size ();
}

extern "C" void eduroute_get_compression_stats (const EduRouteEngine *engine, size_t *out_total_orig_bytes,
                                                size_t *out_total_comp_bytes, size_t *out_total_deltas,
                                                float *out_avg_savings_pct) {
    if (!engine) return;
    if (out_total_orig_bytes) *out_total_orig_bytes = engine->total_original_bytes;
    if (out_total_comp_bytes) *out_total_comp_bytes = engine->total_compressed_bytes;
    if (out_total_deltas) *out_total_deltas = engine->total_deltas_compressed;
    if (out_avg_savings_pct) {
        if (engine->total_original_bytes > 0 && engine->total_original_bytes >= engine->total_compressed_bytes) {
            *out_avg_savings_pct =
                    static_cast<float> ((1.0 - static_cast<double> (engine->total_compressed_bytes) /
                                                       static_cast<double> (engine->total_original_bytes)) *
                                        100.0);
        } else {
            *out_avg_savings_pct = 0.0f;
        }
    }
}

extern "C" EduIngestResult eduroute_ingest_recovered_answer (EduRouteEngine *engine, uint32_t student_id,
                                                             uint16_t question_id, uint8_t action_type,
                                                             uint8_t mcq_option, uint32_t monotonic_dt_ms,
                                                             const char *full_text, size_t text_len,
                                                             uint64_t server_recv_ms) {
    if (!engine) return make_error_result (static_cast<core::u8> (core::StatusCode::InvalidState));

    engine->scratch_arena.reset ();
    core::usize buf_cap = sizeof (EduWireHeader) + text_len + 64;
    auto *wire_buf      = static_cast<core::u8 *> (engine->scratch_arena.allocate (buf_cap, alignof (core::u64)));
    if (!wire_buf) return make_error_result (static_cast<core::u8> (core::StatusCode::OutOfMemory));

    core::usize packet_sz =
            eduroute_build_wire_packet (engine, student_id, question_id, action_type, mcq_option, monotonic_dt_ms,
                                        full_text, static_cast<uint32_t> (text_len), wire_buf, buf_cap);
    if (packet_sz == 0) return make_error_result (static_cast<core::u8> (core::StatusCode::CorruptedPayload));

    return eduroute_ingest_packet (engine, wire_buf, packet_sz, server_recv_ms);
}
