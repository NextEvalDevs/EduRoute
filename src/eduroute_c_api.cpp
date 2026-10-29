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

// ============================================================================
//  Internal per-student state (stored in the persistent arena)
// ============================================================================

struct StudentState {
        core::Hash64 dag_head;               // FNV-1a hash of last accepted DAG node
        core::u32 total_time_ms;             // monotonic accumulated answer time
        core::i32 scaled_score;              // last computed scaled score
        core::LeaderboardKey lb_key;         // key currently held in the AVL tree
        bool in_avl;                         // whether a key is currently in the AVL
        adaptive_rank::ExamSession *session; // heap pointer into pers_arena
};

// ============================================================================
//  EduRouteEngine
// ============================================================================

struct EduRouteEngine {
        // --- Arenas ---------------------------------------------------------------
        core::Arena pers_arena;    // persistent: lives for the whole exam
        core::Arena scratch_arena; // scratch: reset after each packet

        // --- Div 3: Adaptive Ranking ----------------------------------------------
        adaptive_rank::QuestionBank question_bank;
        adaptive_rank::FenwickScore fenwick;
        adaptive_rank::OrderStatAvl avl;

        // --- Div 2: Anti-Cheat ---------------------------------------------------
        core::anticheat::KmerWinnow kmer;
        core::anticheat::AnomalyStats anomaly;
        core::anticheat::CollusionGraph collusion;

        // --- Student table -------------------------------------------------------
        StudentState *students; // array[max_students] in pers_arena
        core::u32 max_students;
        core::u32 max_questions;

        // --- WAL (simple append-only log) ----------------------------------------
        FILE *wal_fp;

        // Constructor: forward to the two arenas
        EduRouteEngine (core::usize pers_bytes, core::usize scratch_bytes, core::u32 max_s, core::u32 max_q,
                        const char *wal_path) :
            pers_arena (pers_bytes), scratch_arena (scratch_bytes), kmer (pers_arena, 32768u), max_students (max_s),
            max_questions (max_q), wal_fp (nullptr) {
            students = nullptr;
            if (wal_path && wal_path[0] != '\0') { wal_fp = std::fopen (wal_path, "ab"); }
        }

        ~EduRouteEngine () {
            if (wal_fp) {
                std::fclose (wal_fp);
                wal_fp = nullptr;
            }
        }
};

// ============================================================================
//  Helpers
// ============================================================================

namespace {

    // Simple FNV-1a over a byte buffer (same algorithm as core::fnv1a_hash_bytes)
    static core::Hash64 fnv1a (const void *data, core::usize len) noexcept {
        const auto *bytes = static_cast<const core::u8 *> (data);
        core::Hash64 h    = 14695981039346656037ULL;
        for (core::usize i = 0; i < len; ++i) {
            h ^= static_cast<core::Hash64> (bytes[i]);
            h *= 1099511628211ULL;
        }
        return h;
    }

    // Compute the simple MAC16: lower 16 bits of FNV-1a over (header_without_mac | payload).
    static core::u16 compute_mac16 (const EduWireHeader *hdr, const core::u8 *payload, core::u16 payload_len) noexcept {
        // Hash header fields excluding mac_tag16 field itself (last 2 bytes of the 32-byte header).
        constexpr core::usize HDR_NO_MAC = sizeof (EduWireHeader) - sizeof (core::u16);
        core::Hash64 h                   = fnv1a (hdr, HDR_NO_MAC);
        // Continue hashing over the payload.
        const auto *pb = static_cast<const core::u8 *> (static_cast<const void *> (payload));
        for (core::u16 i = 0; i < payload_len; ++i) {
            h ^= static_cast<core::Hash64> (pb[i]);
            h *= 1099511628211ULL;
        }
        return static_cast<core::u16> (h & 0xFFFFu);
    }

    // Clamp a student ID to a valid range.
    static bool valid_student (const EduRouteEngine *eng, core::u32 sid) noexcept { return sid < eng->max_students; }

    // Return a zeroed-out ingest result with the given status.
    static EduIngestResult make_error_result (core::u8 status) noexcept {
        EduIngestResult r{};
        r.status_code      = status;
        r.next_question_id = 0xFFFFu;
        return r;
    }

} // anonymous namespace

// ============================================================================
//  Lifecycle
// ============================================================================

extern "C" EduRouteEngine *eduroute_create (uint32_t max_students, uint32_t max_questions, const char *wal_filepath) {
    if (max_students == 0 || max_questions == 0) return nullptr;

    // Estimate arena sizes:
    //   - per student:  ExamSession (depends on bank; use 4 KB each as upper bound)
    //                   StudentState (fixed 48 bytes)
    //   - shared:  QuestionBank, Fenwick, AVL, AntiCheat indexes
    const core::usize per_student_bytes = 4096u + sizeof (StudentState) + 64u;
    const core::usize bank_bytes        = adaptive_rank::QuestionBank::required_bytes (max_questions);
    const core::usize fenwick_bytes     = adaptive_rank::FenwickScore::required_bytes ();
    const core::usize avl_bytes         = adaptive_rank::OrderStatAvl::required_bytes (max_students);
    const core::usize anticheat_bytes   = 8u * 1024u * 1024u; // 8 MB for indexes & graph
    const core::usize pers_bytes        = bank_bytes + fenwick_bytes + avl_bytes + anticheat_bytes +
                                          static_cast<core::usize> (max_students) * per_student_bytes +
                                          1u * 1024u * 1024u; // 1 MB slack

    const core::usize scratch_bytes = 2u * 1024u * 1024u; // 2 MB scratch per packet

    // Allocate the engine itself from the OS (not from an arena).
    void *raw = std::malloc (sizeof (EduRouteEngine));
    if (!raw) return nullptr;

    auto *eng = ::new (raw) EduRouteEngine (pers_bytes, scratch_bytes, max_students, max_questions, wal_filepath);

    // Init Div 3 structures.
    // QuestionBank::init needs the arena and capacity
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

    // Init Div 2 structures.
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

    // Allocate the student state table.
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

    // Map the difficulty from [600,1800] range described in InternalDocs to [-4, +4]
    // using the linear mapping:  d_irt = (beta - 1200) / 150.0
    const double d_irt = (static_cast<double> (difficulty_beta) - 1200.0) / 150.0;

    // Use the public add_calibrated API which derives tier from difficulty.
    engine->question_bank.add_calibrated (static_cast<core::QuestionId> (question_id),
                                          static_cast<core::TopicId> (topic_id), d_irt, correct_mcq_option,
                                          1.0 /* default discrimination */);
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

    // ---- 1. Parse & verify header -------------------------------------------
    EduWireHeader hdr{};
    std::memcpy (&hdr, wire_packet, sizeof (EduWireHeader));

    const core::u8 *payload     = wire_packet + sizeof (EduWireHeader);
    const core::u16 payload_len = hdr.payload_len;

    if (sizeof (EduWireHeader) + static_cast<core::usize> (payload_len) > packet_len) {
        return make_error_result (static_cast<core::u8> (core::StatusCode::CorruptedPayload));
    }

    // Verify MAC.
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

    // ---- 2. Verify hash chain against DAG head ------------------------------
    if (hdr.parent_hash != st.dag_head && st.dag_head != core::ROOT_DAG_HASH) {
        // Allow the very first submission (dag_head == ROOT_DAG_HASH).
        return make_error_result (static_cast<core::u8> (core::StatusCode::HashChainMismatch));
    }

    // Advance the DAG head.
    st.dag_head = hdr.node_hash;

    // ---- 3. WAL append (raw frame) ------------------------------------------
    if (engine->wal_fp) { std::fwrite (wire_packet, 1u, sizeof (EduWireHeader) + payload_len, engine->wal_fp); }

    // ---- 4. Reset scratch arena ---------------------------------------------
    engine->scratch_arena.reset ();

    const auto action = static_cast<core::ActionType> (hdr.action_type);

    // ---- 5. Ensure the student has an active ExamSession -------------------
    if (st.session == nullptr && engine->question_bank.ready () && engine->question_bank.size () > 0u) {
        // Allocate the ExamSession object in the persistent arena.
        void *sess_mem = engine->pers_arena.allocate (sizeof (adaptive_rank::ExamSession), alignof (adaptive_rank::ExamSession));
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

    // ---- 6. Div 2: Time-sync anomaly detection ------------------------------
    {
        core::Array<core::anticheat::StatAnomalyMatch> stat_hits (engine->scratch_arena, 16u);
        engine->anomaly.record_dt (sid, qid, hdr.monotonic_dt_ms, engine->scratch_arena, stat_hits);

        bool dummy_flag = false;
        for (core::usize i = 0; i < stat_hits.size (); ++i) {
            const auto &m = stat_hits[i];
            engine->collusion.update_stat (sid, m.other_student, m.time_score, 0.0f, dummy_flag);
        }
    }

    // ---- 7. Div 2: Text-similarity fingerprinting (for TextDelta/FinalSubmit)
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

    // ---- 8. Div 2: MCQ rare-error detection ---------------------------------
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

    // ---- 9. Div 3: Grade and update IRT/Fenwick/AVL -------------------------
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

            // Update Fenwick.
            if (st.in_avl) {
                engine->fenwick.update (old_score, new_score);
            } else {
                engine->fenwick.add (new_score);
            }

            // Update AVL.
            const core::LeaderboardKey new_key = st.session->leaderboard_key ();
            if (st.in_avl) {
                engine->avl.update (st.lb_key, new_key);
            } else {
                engine->avl.insert (new_key);
                st.in_avl = true;
            }
            st.lb_key = new_key;
        }

        // Serve next question.
        if (!st.session->finished ()) { st.session->next_question (&next_qid); }
    }

    // ---- 10. Collusion flags ------------------------------------------------
    const bool is_flagged      = engine->collusion.is_flagged (sid);
    const core::u32 cluster_rt = engine->collusion.cluster_root (sid);

    // ---- 11. Build response -------------------------------------------------
    EduIngestResult res{};
    res.status_code      = static_cast<core::u8> (core::StatusCode::Ok);
    res.is_flagged_ring  = is_flagged ? 1u : 0u;
    res.next_question_id = static_cast<uint16_t> (next_qid);
    res.current_score    = new_score;
    res.updated_theta    = static_cast<float> (new_theta);
    res.cluster_root_id  = cluster_rt;

    // Rank and percentile from Fenwick/AVL.
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

        // Percentile from Fenwick.
        out_rows[i].percentile = static_cast<float> (engine->fenwick.percentile (keys[i].scaled_score));

        // IRT ability from session.
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

    // Reuse a scratch buffer of CollusionEdge.
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
