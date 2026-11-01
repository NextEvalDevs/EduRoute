# EduRoute API Reference

Complete specification of EduRoute's C Application Binary Interface (C ABI), Python Bridge, and Web REST API.

---

## 1. C ABI Data Structures (`include/eduroute_c_api.h`)

### `EduWireHeader` (32 Bytes, `#pragma pack(push, 1)`)
The wire packet header prepended to every incoming payload sent to the engine.

| Field | Type | Offset | Description |
|---|---|---|---|
| `parent_hash` | `uint64_t` | 0 | FNV-1a hash of the student's previous DAG node state. Must match engine DAG head. |
| `node_hash` | `uint64_t` | 8 | Deterministic FNV-1a hash of the current state transition. |
| `student_id` | `uint32_t` | 16 | Unique student identifier (`0 .. max_students - 1`). |
| `question_id` | `uint16_t` | 20 | Unique question identifier (`0 .. max_questions - 1`). |
| `action_type` | `uint8_t` | 22 | Action code: `0`=MCQ, `1`=TextDelta, `2`=FocusLost, `3`=FinalSubmit, `4`=Heartbeat. |
| `mcq_option` | `uint8_t` | 23 | Selected option index (`0..3`), or `0xFF` for descriptive text submissions. |
| `monotonic_dt_ms` | `uint32_t` | 24 | Monotonic client elapsed time on question in milliseconds. |
| `payload_len` | `uint16_t` | 28 | Byte count of the payload directly following this header. |
| `mac_tag16` | `uint16_t` | 30 | Lower 16 bits of keyed FNV-1a MAC calculated over header (bytes 0..29) + payload. |

---

### `EduIngestResult` (24 Bytes)
The synchronous response returned immediately upon ingesting a wire packet.

| Field | Type | Description |
|---|---|---|
| `status_code` | `uint8_t` | Return code: `0` = Ok, `1` = InvalidState, `2` = CorruptedPayload, `3` = HashChainMismatch, `4` = OutOfMemory. |
| `is_flagged_ring` | `uint8_t` | `1` if student is currently part of an active DSU cheating cluster ($W \ge 3.0$), else `0`. |
| `next_question_id`| `uint16_t` | Next question ID selected by IRT Fisher information (`0xFFFF` if exam completed). |
| `current_score` | `int32_t` | Current scaled exam score in range `[0 .. 10000]`. |
| `live_rank` | `uint32_t` | Exact 1-based rank from Order-Statistic AVL Tree. |
| `live_percentile`| `float` | Exact percentile rank (`0.0 .. 100.0`) from Fenwick Tree. |
| `updated_theta` | `float` | Updated student ability rating from Item Response Theory model. |
| `cluster_root_id` | `uint32_t` | Root student ID of the DSU collusion cluster (equals `student_id` if unflagged). |

---

### `EduLeaderboardRow` (28 Bytes)
Represents a single student row queried from the Order-Statistic AVL tree.

| Field | Type | Description |
|---|---|---|
| `rank` | `uint32_t` | 1-based exact leaderboard position. |
| `student_id` | `uint32_t` | Student identifier. |
| `scaled_score` | `int32_t` | Scaled score (`0 .. 10000`). |
| `total_time_ms`| `uint32_t` | Monotonic cumulative elapsed exam time. |
| `percentile` | `float` | Fenwick percentile (`0.0 .. 100.0`). |
| `theta_ability`| `float` | Latent ability rating $\theta$. |
| `is_flagged` | `uint8_t` | `1` if flagged for collusion, else `0`. |

---

### `EduCollusionEdge` (32 Bytes)
Represents a confirmed cheating link between two students.

| Field | Type | Description |
|---|---|---|
| `student_a` | `uint32_t` | Student ID of node A. |
| `student_b` | `uint32_t` | Student ID of node B. |
| `cluster_root` | `uint32_t` | Root representative ID in the Disjoint Set Union forest. |
| `combined_weight`| `float` | Combined edge weight $W = W_{\text{text}} + W_{\text{time}} + W_{\text{error}}$ (threshold $\ge 3.0$). |
| `peak_text_sim` | `float` | Peak K-mer winnowing containment similarity $[0.0 .. 1.0]$. |
| `accum_time_score`| `float` | Accumulated temporal coincidence score. |
| `accum_err_score` | `float` | Accumulated shared rare-error score. |
| `collision_count` | `uint16_t` | Number of suspicious question collisions observed. |

---

## 2. C ABI Function Reference

### Engine Lifecycle & Setup

```c
EduRouteEngine *eduroute_create(uint32_t max_students, uint32_t max_questions, const char *wal_filepath);
```
- **Description**: Allocates and initializes an `EduRouteEngine` instance with persistent and scratch bump arenas. If `wal_filepath` is non-null, opens `.wal` (Write-Ahead Log) and `.dag` (Merkle DAG) backing files on disk, automatically replaying any uncommitted records.
- **Complexity**: $O(\text{max\_questions} + \text{max\_students})$.

```c
void eduroute_destroy(EduRouteEngine *engine);
```
- **Description**: Closes all on-disk backing files (`wal_fp`, `wal`, `merkle_dag`), frees arena memory, and destructs the engine.
- **Complexity**: $O(1)$.

```c
void eduroute_register_question(EduRouteEngine *engine, uint16_t question_id, uint8_t topic_id,
                                float difficulty_beta, uint32_t expected_time_ms, uint8_t correct_mcq_option);
```
- **Description**: Registers a calibrated question into the Item Response Theory Question Bank.
- **Parameters**:
  - `difficulty_beta`: Difficulty rating in range `[600.0 .. 1800.0]`. Linear calibration maps this to $d_{irt} \in [-4.0, +4.0]$.
  - `expected_time_ms`: Benchmark solving time in milliseconds.
  - `correct_mcq_option`: Index `0..3` of correct answer key.

---

### Hot-Path Wire Processing

```c
size_t eduroute_build_wire_packet(EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                  uint8_t action_type, uint8_t mcq_option, uint32_t monotonic_dt_ms,
                                  const char *new_full_text, uint32_t new_text_len,
                                  uint8_t *out_wire_buf, size_t out_buf_cap);
```
- **Description**: Client helper that populates an `EduWireHeader`, sets `parent_hash` to the student's active DAG head, computes `node_hash` and `mac_tag16`, copies the payload, and returns total packet byte length.

```c
EduIngestResult eduroute_ingest_packet(EduRouteEngine *engine, const uint8_t *wire_packet,
                                       size_t packet_len, uint64_t server_recv_ms);
```
- **Description**: Primary hot-path entry point. Validates MAC-16 and DAG chain, Huffman-compresses answer deltas, persists to disk WAL and Merkle DAG, executes anti-cheat analysis, grades answer, updates AVL tree and Fenwick tree, and selects the next adaptive question.
- **Complexity**: $< 50\ \mu\text{s}$ execution time.

```c
EduIngestResult eduroute_ingest_recovered_answer(EduRouteEngine *engine, uint32_t student_id,
                                                 uint16_t question_id, uint8_t action_type,
                                                 uint8_t mcq_option, uint32_t monotonic_dt_ms,
                                                 const char *full_text, size_t text_len,
                                                 uint64_t server_recv_ms);
```
- **Description**: Direct ingestion function for processing answers recovered from client disk storage after an offline network interruption. Automatically constructs wire frame and runs full pipeline.

---

### Division 1: Sync Engine, WAL & Huffman Compression

```c
size_t eduroute_get_dag_node_count(const EduRouteEngine *engine);
```
- **Description**: Returns total count of cryptographic answer snapshots logged in the Merkle DAG on disk.

```c
int eduroute_get_dag_latest(const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                            uint64_t *out_parent_hash, uint64_t *out_node_hash,
                            char *out_answer_buf, size_t buf_cap, size_t *out_answer_len);
```
- **Description**: Retrieves the most recent answer snapshot and hashes for `(student_id, question_id)`. Returns `1` if found, `0` otherwise.

```c
size_t eduroute_get_dag_history_size(const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id);
```
- **Description**: Returns count of retained history snapshots for a student and question (up to 5).

```c
size_t eduroute_wal_pending_count(const EduRouteEngine *engine);
```
- **Description**: Returns count of unacknowledged WAL records currently pending on disk.

```c
uint64_t eduroute_wal_last_sequence(const EduRouteEngine *engine);
uint64_t eduroute_wal_acknowledged_sequence(const EduRouteEngine *engine);
```
- **Description**: Returns the latest assigned sequence number and the highest committed ACK sequence.

```c
int eduroute_wal_acknowledge(EduRouteEngine *engine, uint64_t sequence);
```
- **Description**: Appends a `RECORD_ACK` frame to disk for all sequences $\le \text{sequence}$. Returns `1` on success.

```c
int eduroute_wal_peek_next(const EduRouteEngine *engine, uint64_t *out_sequence,
                           uint32_t *out_student_id, uint64_t *out_timestamp_ms,
                           size_t *out_original_size, size_t *out_compressed_bytes,
                           size_t *out_delta_count);
```
- **Description**: Inspects the oldest pending unacknowledged WAL record recovered from disk without committing.

```c
int eduroute_wal_append_unacked(EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
                                uint64_t timestamp_ms, const char *old_text, const char *new_text,
                                uint64_t *out_sequence);
```
- **Description**: Encodes a Huffman delta between `old_text` and `new_text` and appends an unacknowledged record to disk WAL for failure recovery testing.

```c
int eduroute_wal_recover_and_apply_next(EduRouteEngine *engine, uint64_t *out_sequence,
                                        uint32_t *out_student_id, char *out_text_buf,
                                        size_t buf_cap, size_t *out_text_len);
```
- **Description**: Replays the oldest unacknowledged WAL record from disk, decompresses its Huffman delta, writes restored answer text to `out_text_buf`, and acknowledges the sequence.

```c
size_t eduroute_compress_delta(EduRouteEngine *engine, uint32_t answer_id,
                               const char *old_text, const char *new_text,
                               size_t *out_original_size, size_t *out_bit_length);
```
- **Description**: Compresses the delta between two text states using dynamic Huffman coding. Returns compressed byte length.

```c
void eduroute_get_compression_stats(const EduRouteEngine *engine,
                                    size_t *out_total_orig_bytes,
                                    size_t *out_total_comp_bytes,
                                    size_t *out_total_deltas,
                                    float *out_avg_savings_pct);
```
- **Description**: Returns cumulative metrics on bytes ingested before vs after Huffman compression.

---

### Telemetry & Query APIs

```c
size_t eduroute_get_top_k(EduRouteEngine *engine, EduLeaderboardRow *out_rows, size_t k);
```
- **Description**: Queries the top $K$ students ordered by score ($\downarrow$), time ($\uparrow$), and student ID ($\uparrow$) from the Order-Statistic AVL tree.
- **Complexity**: $O(K + \log N)$.

```c
size_t eduroute_get_collusion_edges(EduRouteEngine *engine, EduCollusionEdge *out_edges, size_t max_edges);
```
- **Description**: Queries confirmed collusion edges where combined weight $W \ge 3.0$.

```c
size_t eduroute_get_arena_Memory_used(const EduRouteEngine *engine);
```
- **Description**: Returns combined byte count used across persistent and scratch bump arenas.

---
