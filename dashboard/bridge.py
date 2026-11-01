"""
EduRoute C-ABI ctypes Bridge
Strictly interfaces with libeduroute.so via extern "C" ABI defined in include/eduroute_c_api.h
"""

import os
import sys
import ctypes
import threading
from typing import List, Dict, Optional, Tuple

# -----------------------------------------------------------------------------
# 1. Load C-ABI Shared Library (with automatic ASan detection)
# -----------------------------------------------------------------------------
def _find_lib_path() -> str:
    current_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(current_dir, "libeduroute.so"),
        os.path.join(current_dir, "..", "dashboard", "libeduroute.so"),
        os.path.join(current_dir, "..", "cmake-build-release", "dashboard", "libeduroute.so"),
        os.path.join(current_dir, "..", "cmake-build-debug", "dashboard", "libeduroute.so"),
    ]
    for p in candidates:
        if os.path.exists(p):
            return os.path.abspath(p)
    raise FileNotFoundError(f"Could not locate libeduroute.so. Checked: {candidates}")

_lib_path = _find_lib_path()

# If libeduroute.so was compiled with ASan (Debug mode), pre-load libasan.so to avoid crash
if "libasan" not in os.environ.get("LD_PRELOAD", ""):
    try:
        with open(_lib_path, "rb") as _f:
            _header = _f.read(500000)
            if b"__asan_init" in _header:
                import subprocess
                _res = subprocess.run(["gcc", "-print-file-name=libasan.so"], capture_output=True, text=True)
                _asan_path = _res.stdout.strip()
                if os.path.exists(_asan_path):
                    _env = dict(os.environ)
                    _env["LD_PRELOAD"] = _asan_path
                    os.execve(sys.executable, [sys.executable] + sys.argv, _env)
    except Exception:
        pass

_lib = ctypes.CDLL(_lib_path)

# -----------------------------------------------------------------------------
# 2. C-ABI ctypes Structures (Packed & Aligned exactly to C definitions)
# -----------------------------------------------------------------------------
# #pragma pack(push, 1) - 32 Bytes
class EduWireHeader(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("parent_hash", ctypes.c_uint64),
        ("node_hash", ctypes.c_uint64),
        ("student_id", ctypes.c_uint32),
        ("question_id", ctypes.c_uint16),
        ("action_type", ctypes.c_uint8),       # 0=MCQ, 1=TextDelta, 2=FocusLost, 3=FinalSubmit, 4=Heartbeat
        ("mcq_option", ctypes.c_uint8),        # 0..3 for MCQ, 0xFF if text-only
        ("monotonic_dt_ms", ctypes.c_uint32),
        ("payload_len", ctypes.c_uint16),
        ("mac_tag16", ctypes.c_uint16),
    ]

# 24 Bytes Synchronous Response
class EduIngestResult(ctypes.Structure):
    _fields_ = [
        ("status_code", ctypes.c_uint8),       # 0 = Ok
        ("is_flagged_ring", ctypes.c_uint8),   # 1 if student in DSU cheating cluster
        ("next_question_id", ctypes.c_uint16), # Adaptive IRT next question (0xFFFF if none)
        ("current_score", ctypes.c_int32),     # Scaled score (0 .. 10000)
        ("live_rank", ctypes.c_uint32),        # 1-based exact rank from AVL Tree
        ("live_percentile", ctypes.c_float),   # 0.0 .. 100.0 from Fenwick Tree
        ("updated_theta", ctypes.c_float),     # Student IRT ability rating
        ("cluster_root_id", ctypes.c_uint32),  # DSU root ID
    ]

# Export Struct for Flask Leaderboard (28 Bytes)
class EduLeaderboardRow(ctypes.Structure):
    _fields_ = [
        ("rank", ctypes.c_uint32),
        ("student_id", ctypes.c_uint32),
        ("scaled_score", ctypes.c_int32),
        ("total_time_ms", ctypes.c_uint32),
        ("percentile", ctypes.c_float),
        ("theta_ability", ctypes.c_float),
        ("is_flagged", ctypes.c_uint8),
    ]

# Export Struct for Flask Collusion Graph (32 Bytes)
class EduCollusionEdge(ctypes.Structure):
    _fields_ = [
        ("student_a", ctypes.c_uint32),
        ("student_b", ctypes.c_uint32),
        ("cluster_root", ctypes.c_uint32),
        ("combined_weight", ctypes.c_float),
        ("peak_text_sim", ctypes.c_float),
        ("accum_time_score", ctypes.c_float),
        ("accum_err_score", ctypes.c_float),
        ("collision_count", ctypes.c_uint16),
    ]

# -----------------------------------------------------------------------------
# 3. Bind Function Prototypes
# -----------------------------------------------------------------------------
# EduRouteEngine *eduroute_create (uint32_t max_students, uint32_t max_questions, const char *wal_filepath);
_lib.eduroute_create.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p]
_lib.eduroute_create.restype = ctypes.c_void_p

# void eduroute_destroy (EduRouteEngine *engine);
_lib.eduroute_destroy.argtypes = [ctypes.c_void_p]
_lib.eduroute_destroy.restype = None

# void eduroute_register_question (EduRouteEngine *engine, uint16_t question_id, uint8_t topic_id,
#                                  float difficulty_beta, uint32_t expected_time_ms, uint8_t correct_mcq_option);
_lib.eduroute_register_question.argtypes = [
    ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint8, ctypes.c_float, ctypes.c_uint32, ctypes.c_uint8
]
_lib.eduroute_register_question.restype = None

# size_t eduroute_build_wire_packet (EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
#                                    uint8_t action_type, uint8_t mcq_option, uint32_t monotonic_dt_ms,
#                                    const char *new_full_text, uint32_t new_text_len, uint8_t *out_wire_buf,
#                                    size_t out_buf_cap);
_lib.eduroute_build_wire_packet.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint32,
    ctypes.c_char_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t
]
_lib.eduroute_build_wire_packet.restype = ctypes.c_size_t

# EduIngestResult eduroute_ingest_packet (EduRouteEngine *engine, const uint8_t *wire_packet, size_t packet_len,
#                                         uint64_t server_recv_ms);
_lib.eduroute_ingest_packet.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t, ctypes.c_uint64
]
_lib.eduroute_ingest_packet.restype = EduIngestResult

# size_t eduroute_get_top_k (EduRouteEngine *engine, EduLeaderboardRow *out_rows, size_t k);
_lib.eduroute_get_top_k.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(EduLeaderboardRow), ctypes.c_size_t
]
_lib.eduroute_get_top_k.restype = ctypes.c_size_t

# size_t eduroute_get_collusion_edges (EduRouteEngine *engine, EduCollusionEdge *out_edges, size_t max_edges);
_lib.eduroute_get_collusion_edges.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(EduCollusionEdge), ctypes.c_size_t
]
_lib.eduroute_get_collusion_edges.restype = ctypes.c_size_t

# size_t eduroute_get_arena_Memory_used (const EduRouteEngine *engine);
_lib.eduroute_get_arena_Memory_used.argtypes = [ctypes.c_void_p]
_lib.eduroute_get_arena_Memory_used.restype = ctypes.c_size_t

# size_t eduroute_get_dag_node_count (const EduRouteEngine *engine);
_lib.eduroute_get_dag_node_count.argtypes = [ctypes.c_void_p]
_lib.eduroute_get_dag_node_count.restype = ctypes.c_size_t

# int eduroute_get_dag_latest (const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id,
#                              uint64_t *out_parent_hash, uint64_t *out_node_hash,
#                              char *out_answer_buf, size_t buf_cap, size_t *out_answer_len);
_lib.eduroute_get_dag_latest.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16,
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64),
    ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
]
_lib.eduroute_get_dag_latest.restype = ctypes.c_int

# size_t eduroute_get_dag_history_size (const EduRouteEngine *engine, uint32_t student_id, uint16_t question_id);
_lib.eduroute_get_dag_history_size.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16]
_lib.eduroute_get_dag_history_size.restype = ctypes.c_size_t

# --- Division 1: WAL, Recovery & Huffman Delta Compression C-ABI Bindings ---
# size_t eduroute_wal_pending_count (const EduRouteEngine *engine);
_lib.eduroute_wal_pending_count.argtypes = [ctypes.c_void_p]
_lib.eduroute_wal_pending_count.restype = ctypes.c_size_t

# uint64_t eduroute_wal_last_sequence (const EduRouteEngine *engine);
_lib.eduroute_wal_last_sequence.argtypes = [ctypes.c_void_p]
_lib.eduroute_wal_last_sequence.restype = ctypes.c_uint64

# uint64_t eduroute_wal_acknowledged_sequence (const EduRouteEngine *engine);
_lib.eduroute_wal_acknowledged_sequence.argtypes = [ctypes.c_void_p]
_lib.eduroute_wal_acknowledged_sequence.restype = ctypes.c_uint64

# int eduroute_wal_acknowledge (EduRouteEngine *engine, uint64_t sequence);
_lib.eduroute_wal_acknowledge.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
_lib.eduroute_wal_acknowledge.restype = ctypes.c_int

# int eduroute_wal_peek_next (const EduRouteEngine *engine, uint64_t *out_sequence, ...);
_lib.eduroute_wal_peek_next.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint32),
    ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)
]
_lib.eduroute_wal_peek_next.restype = ctypes.c_int

# int eduroute_wal_append_unacked (EduRouteEngine *engine, ...);
_lib.eduroute_wal_append_unacked.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint64,
    ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint64)
]
_lib.eduroute_wal_append_unacked.restype = ctypes.c_int

# int eduroute_wal_recover_and_apply_next (EduRouteEngine *engine, ...);
_lib.eduroute_wal_recover_and_apply_next.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint32),
    ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
]
_lib.eduroute_wal_recover_and_apply_next.restype = ctypes.c_int

# size_t eduroute_compress_delta (EduRouteEngine *engine, ...);
_lib.eduroute_compress_delta.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)
]
_lib.eduroute_compress_delta.restype = ctypes.c_size_t

# void eduroute_get_compression_stats (const EduRouteEngine *engine, ...);
_lib.eduroute_get_compression_stats.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t),
    ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_float)
]
_lib.eduroute_get_compression_stats.restype = None

# EduIngestResult eduroute_ingest_recovered_answer (EduRouteEngine *engine, ...);
_lib.eduroute_ingest_recovered_answer.argtypes = [
    ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint16, ctypes.c_uint8,
    ctypes.c_uint8, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64
]
_lib.eduroute_ingest_recovered_answer.restype = EduIngestResult

# -----------------------------------------------------------------------------
# 4. EduRouteBridge Wrapper Class
# -----------------------------------------------------------------------------
class EduRouteBridge:
    """Thread-safe high-level wrapper over EduRoute C++ engine ABI."""

    ACTION_MCQ = 0
    ACTION_TEXT_DELTA = 1
    ACTION_FOCUS_LOST = 2
    ACTION_FINAL_SUBMIT = 3
    ACTION_HEARTBEAT = 4

    INVALID_QUESTION_ID = 0xFFFF

    def __init__(self, max_students: int = 500, max_questions: int = 100, wal_path: Optional[str] = None):
        self._lock = threading.Lock()
        wal_bytes = wal_path.encode("utf-8") if wal_path else None
        self._engine = _lib.eduroute_create(max_students, max_questions, wal_bytes)
        if not self._engine:
            raise RuntimeError("Failed to create EduRouteEngine in C++ backend.")
        self.max_students = max_students
        self.max_questions = max_questions

    def __del__(self):
        self.destroy()

    def destroy(self):
        with self._lock:
            if getattr(self, "_engine", None):
                _lib.eduroute_destroy(self._engine)
                self._engine = None

    def register_question(self, question_id: int, topic_id: int, difficulty_beta: float,
                          expected_time_ms: int, correct_mcq_option: int):
        """Registers a question into Div 3 Question Bank."""
        with self._lock:
            if not self._engine:
                raise RuntimeError("Engine destroyed.")
            _lib.eduroute_register_question(
                self._engine,
                ctypes.c_uint16(question_id),
                ctypes.c_uint8(topic_id),
                ctypes.c_float(difficulty_beta),
                ctypes.c_uint32(expected_time_ms),
                ctypes.c_uint8(correct_mcq_option)
            )

    def submit_answer(self, student_id: int, question_id: int, action_type: int,
                      mcq_option: int = 0xFF, monotonic_dt_ms: int = 1000,
                      full_text: Optional[str] = None, server_recv_ms: int = 0) -> Dict:
        """
        Builds a packed wire frame, ingests it through the engine, and returns parsed result.
        """
        with self._lock:
            if not self._engine:
                raise RuntimeError("Engine destroyed.")

            text_bytes = full_text.encode("utf-8") if full_text else None
            text_len = len(text_bytes) if text_bytes else 0

            buf_cap = 65536
            buf = (ctypes.c_uint8 * buf_cap)()

            packet_sz = _lib.eduroute_build_wire_packet(
                self._engine,
                ctypes.c_uint32(student_id),
                ctypes.c_uint16(question_id),
                ctypes.c_uint8(action_type),
                ctypes.c_uint8(mcq_option),
                ctypes.c_uint32(monotonic_dt_ms),
                text_bytes,
                ctypes.c_uint32(text_len),
                buf,
                ctypes.c_size_t(buf_cap)
            )

            if packet_sz == 0:
                raise RuntimeError("Failed to build wire packet in C++ engine.")

            res = _lib.eduroute_ingest_packet(
                self._engine,
                buf,
                ctypes.c_size_t(packet_sz),
                ctypes.c_uint64(server_recv_ms)
            )

            return {
                "status_code": int(res.status_code),
                "is_flagged_ring": bool(res.is_flagged_ring),
                "next_question_id": None if res.next_question_id == self.INVALID_QUESTION_ID else int(res.next_question_id),
                "current_score": int(res.current_score),
                "live_rank": int(res.live_rank),
                "live_percentile": round(float(res.live_percentile), 2),
                "updated_theta": round(float(res.updated_theta), 3),
                "cluster_root_id": int(res.cluster_root_id),
            }

    def get_top_k(self, k: int = 50) -> List[Dict]:
        """Queries AVL tree + Fenwick tree for the live top-k leaderboard."""
        with self._lock:
            if not self._engine:
                return []
            rows = (EduLeaderboardRow * k)()
            count = _lib.eduroute_get_top_k(self._engine, rows, ctypes.c_size_t(k))
            result = []
            for i in range(count):
                row = rows[i]
                result.append({
                    "rank": int(row.rank),
                    "student_id": int(row.student_id),
                    "scaled_score": int(row.scaled_score),
                    "total_time_ms": int(row.total_time_ms),
                    "total_time_s": round(row.total_time_ms / 1000.0, 1),
                    "percentile": round(float(row.percentile), 2),
                    "theta_ability": round(float(row.theta_ability), 3),
                    "is_flagged": bool(row.is_flagged),
                })
            return result

    def get_collusion_edges(self, max_edges: int = 100) -> List[Dict]:
        """Queries the collusion graph for confirmed cheating pairs and DSU clusters."""
        with self._lock:
            if not self._engine:
                return []
            edges = (EduCollusionEdge * max_edges)()
            count = _lib.eduroute_get_collusion_edges(self._engine, edges, ctypes.c_size_t(max_edges))
            result = []
            for i in range(count):
                e = edges[i]
                result.append({
                    "student_a": int(e.student_a),
                    "student_b": int(e.student_b),
                    "cluster_root": int(e.cluster_root),
                    "combined_weight": round(float(e.combined_weight), 2),
                    "peak_text_sim": round(float(e.peak_text_sim), 2),
                    "accum_time_score": round(float(e.accum_time_score), 2),
                    "accum_err_score": round(float(e.accum_err_score), 2),
                    "collision_count": int(e.collision_count),
                })
            return result

    def get_memory_used(self) -> Dict[str, int]:
        """Queries persistent and scratch arena memory usage in bytes."""
        with self._lock:
            if not self._engine:
                return {"used_bytes": 0, "used_kb": 0}
            used = _lib.eduroute_get_arena_Memory_used(self._engine)
            return {
                "used_bytes": int(used),
                "used_kb": round(used / 1024, 2),
                "used_mb": round(used / (1024 * 1024), 2),
            }

    def get_dag_node_count(self) -> int:
        """Queries the total number of DAG nodes recorded in the C++ engine."""
        with self._lock:
            if not self._engine:
                return 0
            return int(_lib.eduroute_get_dag_node_count(self._engine))

    def get_dag_latest(self, student_id: int, question_id: int) -> Optional[Dict]:
        """Queries the latest answer and node hash for a given student and question."""
        with self._lock:
            if not self._engine:
                return None
            parent_hash = ctypes.c_uint64(0)
            node_hash = ctypes.c_uint64(0)
            buf_cap = 65536
            buf = ctypes.create_string_buffer(buf_cap)
            answer_len = ctypes.c_size_t(0)
            ok = _lib.eduroute_get_dag_latest(
                self._engine,
                ctypes.c_uint32(student_id),
                ctypes.c_uint16(question_id),
                ctypes.byref(parent_hash),
                ctypes.byref(node_hash),
                buf,
                ctypes.c_size_t(buf_cap),
                ctypes.byref(answer_len)
            )
            if not ok:
                return None
            return {
                "student_id": student_id,
                "question_id": question_id,
                "parent_hash": int(parent_hash.value),
                "node_hash": int(node_hash.value),
                "answer": buf.value.decode("utf-8", errors="replace"),
                "answer_len": int(answer_len.value),
            }

    def get_dag_history_size(self, student_id: int, question_id: int) -> int:
        """Queries the number of retained history snapshots for a student and question."""
        with self._lock:
            if not self._engine:
                return 0
            return int(_lib.eduroute_get_dag_history_size(
                self._engine,
                ctypes.c_uint32(student_id),
                ctypes.c_uint16(question_id)
            ))

    def get_wal_stats(self) -> Dict:
        """Queries write-ahead log disk status (sequences and pending recovery count)."""
        with self._lock:
            if not self._engine:
                return {"pending_count": 0, "last_sequence": 0, "acknowledged_sequence": 0}
            pending = int(_lib.eduroute_wal_pending_count(self._engine))
            last_seq = int(_lib.eduroute_wal_last_sequence(self._engine))
            acked_seq = int(_lib.eduroute_wal_acknowledged_sequence(self._engine))
            return {
                "pending_count": pending,
                "last_sequence": last_seq,
                "acknowledged_sequence": acked_seq,
            }

    def wal_acknowledge(self, sequence: int) -> bool:
        """Acknowledges a WAL sequence up to the given number."""
        with self._lock:
            if not self._engine:
                return False
            return bool(_lib.eduroute_wal_acknowledge(self._engine, ctypes.c_uint64(sequence)))

    def wal_peek_next(self) -> Optional[Dict]:
        """Peeks the oldest pending unacknowledged WAL record recovered from disk."""
        with self._lock:
            if not self._engine:
                return None
            seq = ctypes.c_uint64(0)
            sid = ctypes.c_uint32(0)
            ts = ctypes.c_uint64(0)
            orig_sz = ctypes.c_size_t(0)
            comp_sz = ctypes.c_size_t(0)
            deltas_c = ctypes.c_size_t(0)
            ok = _lib.eduroute_wal_peek_next(
                self._engine,
                ctypes.byref(seq), ctypes.byref(sid), ctypes.byref(ts),
                ctypes.byref(orig_sz), ctypes.byref(comp_sz), ctypes.byref(deltas_c)
            )
            if not ok:
                return None
            return {
                "sequence": int(seq.value),
                "student_id": int(sid.value),
                "timestamp_ms": int(ts.value),
                "original_size": int(orig_sz.value),
                "compressed_bytes": int(comp_sz.value),
                "delta_count": int(deltas_c.value),
            }

    def wal_append_unacked(self, student_id: int, question_id: int, timestamp_ms: int,
                           old_text: Optional[str], new_text: str) -> Optional[int]:
        """Appends an unacknowledged WAL record for offline simulation testing."""
        with self._lock:
            if not self._engine:
                return None
            old_b = old_text.encode("utf-8") if old_text else b""
            new_b = new_text.encode("utf-8") if new_text else b""
            out_seq = ctypes.c_uint64(0)
            ok = _lib.eduroute_wal_append_unacked(
                self._engine,
                ctypes.c_uint32(student_id),
                ctypes.c_uint16(question_id),
                ctypes.c_uint64(timestamp_ms),
                old_b, new_b,
                ctypes.byref(out_seq)
            )
            return int(out_seq.value) if ok else None

    def wal_recover_and_apply_next(self) -> Optional[Dict]:
        """Decompresses and applies the oldest pending WAL record recovered from disk."""
        with self._lock:
            if not self._engine:
                return None
            seq = ctypes.c_uint64(0)
            sid = ctypes.c_uint32(0)
            buf = ctypes.create_string_buffer(65536)
            text_len = ctypes.c_size_t(0)
            ok = _lib.eduroute_wal_recover_and_apply_next(
                self._engine,
                ctypes.byref(seq), ctypes.byref(sid),
                buf, ctypes.c_size_t(65536),
                ctypes.byref(text_len)
            )
            if not ok:
                return None
            return {
                "sequence": int(seq.value),
                "student_id": int(sid.value),
                "text": buf.value.decode("utf-8", errors="replace"),
                "text_len": int(text_len.value),
            }

    def compress_delta(self, answer_id: int, old_text: Optional[str], new_text: str) -> Dict:
        """Compresses delta between two text states using Huffman coding."""
        with self._lock:
            if not self._engine:
                return {"orig_size": 0, "comp_bytes": 0, "bit_length": 0, "savings_pct": 0.0}
            old_b = old_text.encode("utf-8") if old_text else b""
            new_b = new_text.encode("utf-8") if new_text else b""
            orig_sz = ctypes.c_size_t(0)
            bit_len = ctypes.c_size_t(0)
            comp_sz = _lib.eduroute_compress_delta(
                self._engine,
                ctypes.c_uint32(answer_id),
                old_b, new_b,
                ctypes.byref(orig_sz),
                ctypes.byref(bit_len)
            )
            orig = int(orig_sz.value)
            comp = int(comp_sz)
            bits = int(bit_len.value)
            savings = round(((orig - comp) / orig) * 100.0, 2) if orig > comp and orig > 0 else 0.0
            return {
                "orig_size": orig,
                "comp_bytes": comp,
                "bit_length": bits,
                "savings_pct": savings,
            }

    def get_compression_stats(self) -> Dict:
        """Queries cumulative Huffman delta compression statistics."""
        with self._lock:
            if not self._engine:
                return {"total_orig_bytes": 0, "total_comp_bytes": 0, "total_deltas": 0, "savings_pct": 0.0}
            tot_orig = ctypes.c_size_t(0)
            tot_comp = ctypes.c_size_t(0)
            tot_deltas = ctypes.c_size_t(0)
            savings = ctypes.c_float(0.0)
            _lib.eduroute_get_compression_stats(
                self._engine,
                ctypes.byref(tot_orig), ctypes.byref(tot_comp),
                ctypes.byref(tot_deltas), ctypes.byref(savings)
            )
            return {
                "total_orig_bytes": int(tot_orig.value),
                "total_comp_bytes": int(tot_comp.value),
                "total_deltas": int(tot_deltas.value),
                "savings_pct": round(float(savings.value), 2),
            }

    def submit_recovered_answer(self, student_id: int, question_id: int, action_type: int,
                                mcq_option: int = 0xFF, monotonic_dt_ms: int = 1000,
                                full_text: Optional[str] = None, server_recv_ms: int = 0) -> Dict:
        """Ingests an offline-recovered answer through the C++ engine pipeline."""
        with self._lock:
            if not self._engine:
                raise RuntimeError("Engine destroyed.")
            text_bytes = full_text.encode("utf-8") if full_text else None
            text_len = len(text_bytes) if text_bytes else 0
            res = _lib.eduroute_ingest_recovered_answer(
                self._engine,
                ctypes.c_uint32(student_id),
                ctypes.c_uint16(question_id),
                ctypes.c_uint8(action_type),
                ctypes.c_uint8(mcq_option),
                ctypes.c_uint32(monotonic_dt_ms),
                text_bytes,
                ctypes.c_size_t(text_len),
                ctypes.c_uint64(server_recv_ms)
            )
            return {
                "status_code": int(res.status_code),
                "is_flagged_ring": bool(res.is_flagged_ring),
                "next_question_id": None if res.next_question_id == self.INVALID_QUESTION_ID else int(res.next_question_id),
                "current_score": int(res.current_score),
                "live_rank": int(res.live_rank),
                "live_percentile": round(float(res.live_percentile), 2),
                "updated_theta": round(float(res.updated_theta), 3),
                "cluster_root_id": int(res.cluster_root_id),
            }
