#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the C++ EduRouteEngine instance (owns Persistent & Scratch Arenas)
typedef struct EduRouteEngine EduRouteEngine;

// -----------------------------------------------------------------------------
// 1. Wire Packet Header (32 Bytes, Packed, Zero Padding)
// -----------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct EduWireHeader {
        uint64_t parent_hash;     // Previous DAG node hash for (student_id, question_id)
        uint64_t node_hash;       // FNV-1a hash of this state transition
        uint32_t student_id;      // 0 .. max_students - 1
        uint16_t question_id;     // 0 .. max_questions - 1
        uint8_t action_type;      // 0=MCQ, 1=TextDelta, 2=FocusLost, 3=FinalSubmit, 4=Heartbeat
        uint8_t mcq_option;       // 0..3 for MCQ, 0xFF if text-only
        uint32_t monotonic_dt_ms; // Client monotonic elapsed time in ms
        uint16_t payload_len;     // Byte length of compressed delta following this header
        uint16_t mac_tag16;       // Lower 16 bits of keyed FNV-1a/Poly MAC over header+payload
} EduWireHeader;
#pragma pack(pop)

// -----------------------------------------------------------------------------
// 2. Synchronous Response Returned on Every Packet Ingest (24 Bytes)
// -----------------------------------------------------------------------------
typedef struct EduIngestResult {
        uint8_t status_code;       // 0 = Ok (matches eduroute::StatusCode)
        uint8_t is_flagged_ring;   // 1 if student is currently in a DSU cheating cluster
        uint16_t next_question_id; // Adaptive IRT next question (0xFFFF if none)
        int32_t current_score;     // Scaled score (0 .. 10000)
        uint32_t live_rank;        // 1-based exact rank from Order-Statistic AVL Tree
        float live_percentile;     // 0.0 .. 100.0 from Fenwick Tree
        float updated_theta;       // Updated student IRT ability rating (e.g., 1000.0)
        uint32_t cluster_root_id;  // DSU root ID if flagged, else student_id
} EduIngestResult;

// -----------------------------------------------------------------------------
// 3. Export Structs for Python Flask Dashboard (Leaderboard & Collusion Graph)
// -----------------------------------------------------------------------------
typedef struct EduLeaderboardRow {
        uint32_t rank;
        uint32_t student_id;
        int32_t scaled_score;
        uint32_t total_time_ms;
        float percentile;
        float theta_ability;
        uint8_t is_flagged;
} EduLeaderboardRow;

typedef struct EduCollusionEdge {
        uint32_t student_a;
        uint32_t student_b;
        uint32_t cluster_root;
        float combined_weight;
        float peak_text_sim;
        float accum_time_score;
        float accum_err_score;
        uint16_t collision_count;
} EduCollusionEdge;

// -----------------------------------------------------------------------------
// 4. Lifecycle & Setup API
// -----------------------------------------------------------------------------
EduRouteEngine *eduroute_create (uint32_t max_students, uint32_t max_questions, const char *wal_filepath);

void eduroute_destroy (EduRouteEngine *engine);

// Registers a question into Div 3's IRT Question Bank & stores answer key
void eduroute_register_question (EduRouteEngine *engine, uint16_t question_id, uint8_t topic_id,
                                 float difficulty_beta, // 600.0 .. 1800.0
                                 uint32_t expected_time_ms, uint8_t correct_mcq_option);

// -----------------------------------------------------------------------------
// 5. Core Hot-Path API (Div 1 -> Div 2 -> Div 3)
// -----------------------------------------------------------------------------

// Helper for clients/simulator: packs & encrypts a delta into a raw wire frame
size_t eduroute_build_wire_packet (EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                   uint8_t action_type, uint8_t mcq_option, uint32_t monotonic_dt_ms,
                                   const char *new_full_text, uint32_t new_text_len, uint8_t *out_wire_buf,
                                   size_t out_buf_cap);

// Ingests a raw wire frame, runs WAL + DAG + AntiCheat + IRT/AVL/Fenwick in < 50 microseconds
EduIngestResult eduroute_ingest_packet (EduRouteEngine *engine, const uint8_t *wire_packet, size_t packet_len,
                                        uint64_t server_recv_ms);

// -----------------------------------------------------------------------------
// 6. Telemetry & Dashboard Query API
// -----------------------------------------------------------------------------
size_t eduroute_get_top_k (EduRouteEngine *engine, EduLeaderboardRow *out_rows, size_t k);

size_t eduroute_get_collusion_edges (EduRouteEngine *engine, EduCollusionEdge *out_edges, size_t max_edges);

size_t eduroute_get_arena_Memory_used (const EduRouteEngine *engine);

#ifdef __cplusplus
}
#endif
