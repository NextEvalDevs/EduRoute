/*
 * Eduroute: Sem Project, Dept. of CSE
 *
 * Copyright (C) 2026 Team NextEval @ NITT
 * Authors:
 *   Raj <106125091@nitt.edu>
 *   Raswanth <raswanthvenkatesan2007@gmail.com>
 *   Rithimoni <rithimonisubaramanian@gmail.com>
 *
 * Eduroute is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the C++ EduRouteEngine instance (owns Persistent & Scratch Arenas)
typedef struct EduRouteEngine EduRouteEngine;

// -----------------------------------------------------------------------------
// Wire Packet Header (32 Bytes, Packed, Zero Padding)
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
// Synchronous Response Returned on Every Packet Ingest (24 Bytes)
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
// Export Structs for Python Flask Dashboard (Leaderboard & Collusion Graph)
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
// Lifecycle & Setup API
// -----------------------------------------------------------------------------
EduRouteEngine *eduroute_create (uint32_t max_students, uint32_t max_questions, const char *wal_filepath);

void eduroute_destroy (EduRouteEngine *engine);

// Registers a question into Div 3's IRT Question Bank & stores answer key
void eduroute_register_question (EduRouteEngine *engine, uint16_t question_id, uint8_t topic_id,
                                 float difficulty_beta, // 600.0 .. 1800.0
                                 uint32_t expected_time_ms, uint8_t correct_mcq_option);

// -----------------------------------------------------------------------------
// Core Hot-Path API (Div 1 -> Div 2 -> Div 3)
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
// Telemetry & Dashboard Query API
// -----------------------------------------------------------------------------
size_t eduroute_get_top_k (EduRouteEngine *engine, EduLeaderboardRow *out_rows, size_t k);

size_t eduroute_get_collusion_edges (EduRouteEngine *engine, EduCollusionEdge *out_edges, size_t max_edges);

size_t eduroute_get_arena_Memory_used (const EduRouteEngine *engine);

// -----------------------------------------------------------------------------
// Sync Engine & Merkle DAG API (Division 1)
// -----------------------------------------------------------------------------

// returns the DAG Node Count in the engine DAG
size_t eduroute_get_dag_node_count (const EduRouteEngine *engine);

int eduroute_get_dag_latest (const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                             uint64_t *out_parent_hash, uint64_t *out_node_hash, char *out_answer_buf, size_t buf_cap,
                             size_t *out_answer_len);

size_t eduroute_get_dag_history_size (const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id);

// -----------------------------------------------------------------------------
// Sync Engine, WAL & Huffman Compression API (Division 1)
// -----------------------------------------------------------------------------

// WAL status and disk persistence
size_t eduroute_wal_pending_count (const EduRouteEngine *engine);
uint64_t eduroute_wal_last_sequence (const EduRouteEngine *engine);
uint64_t eduroute_wal_acknowledged_sequence (const EduRouteEngine *engine);
int eduroute_wal_acknowledge (EduRouteEngine *engine, uint64_t sequence);

// Peek oldest pending unacknowledged WAL record recovered from disk
int eduroute_wal_peek_next (const EduRouteEngine *engine, uint64_t *out_sequence, uint32_t *out_student_id,
                            uint64_t *out_timestamp_ms, size_t *out_original_size, size_t *out_compressed_bytes,
                            size_t *out_delta_count);

// Explicitly append an unacknowledged WAL record (used for offline test & simulation)
int eduroute_wal_append_unacked (EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                 uint64_t timestamp_ms, const char *old_text, const char *new_text,
                                 uint64_t *out_sequence);

// Decompress oldest pending WAL record recovered from disk, copy answer text, and acknowledge
int eduroute_wal_recover_and_apply_next (EduRouteEngine *engine, uint64_t *out_sequence, uint32_t *out_student_id,
                                         char *out_text_buf, size_t buf_cap, size_t *out_text_len);

// Compress delta between two text states using Huffman encoding, returning compressed byte length
size_t eduroute_compress_delta (EduRouteEngine *engine, uint32_t answer_id, const char *old_text, const char *new_text,
                                size_t *out_original_size, size_t *out_bit_length);

// Cumulative compression telemetry (bytes before vs after Huffman compression)
void eduroute_get_compression_stats (const EduRouteEngine *engine, size_t *out_total_orig_bytes,
                                     size_t *out_total_comp_bytes, size_t *out_total_deltas,
                                     float *out_avg_savings_pct);

// Ingest an offline-recovered answer directly with delta compression and WAL persistence
EduIngestResult eduroute_ingest_recovered_answer (EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                                  uint8_t action_type, uint8_t mcq_option, uint32_t monotonic_dt_ms,
                                                  const char *full_text, size_t text_len, uint64_t server_recv_ms);

#ifdef __cplusplus
}
#endif
