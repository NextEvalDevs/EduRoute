"""
EduRoute Web Application

Lightweight Flask interface wired directly to libeduroute.so via ctypes.
Serves the adaptive test view, real-time leaderboard, and anti-cheat monitor
with minimal asset payload so it runs smoothly even on low-end hardware.
"""

import os
import sys
import time
import random
from flask import Flask, render_template, request, redirect, url_for, jsonify

from bridge import EduRouteBridge
from questions import QUESTION_BANK, TOPICS, get_question_by_id

app = Flask(__name__)
app.secret_key = "eduroute_local_secret_key"

# -----------------------------------------------------------------------------
# Global C++ Engine Initialization
# -----------------------------------------------------------------------------
WAL_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "eduroute_wal.bin")
engine = EduRouteBridge(max_students=500, max_questions=len(QUESTION_BANK) + 10, wal_path=WAL_FILE)

# Seed C++ question bank with our curriculum
for q in QUESTION_BANK:
    engine.register_question(
        question_id=q["id"],
        topic_id=q["topic_id"],
        difficulty_beta=float(q["difficulty_beta"]),
        expected_time_ms=int(q["expected_time_ms"]),
        correct_mcq_option=int(q["correct_mcq_option"])
    )

# Minimal session cache for the web UI layer (routing & question index).
# The C++ core retains authoritative state for scoring, ranking, and anti-cheat.
STUDENT_SESSIONS = {}

def get_or_create_student_session(student_id: int) -> dict:
    if student_id not in STUDENT_SESSIONS:
        STUDENT_SESSIONS[student_id] = {
            "student_id": student_id,
            "current_question_id": 1,  # initial question to present
            "current_score": 0,
            "live_rank": 1,
            "live_percentile": 0.0,
            "updated_theta": 0.0,
            "is_flagged": False,
            "cluster_root_id": student_id,
            "answered_count": 0,
            "completed": False
        }
    return STUDENT_SESSIONS[student_id]

# -----------------------------------------------------------------------------
# UI & Exam Routes
# -----------------------------------------------------------------------------

@app.route("/")
def index():
    mem = engine.get_memory_used()
    leaderboard = engine.get_top_k(50)
    edges = engine.get_collusion_edges(50)
    comp_stats = engine.get_compression_stats()
    wal_stats = engine.get_wal_stats()
    dag_nodes = engine.get_dag_node_count()
    return render_template(
        "index.html",
        bank_size=len(QUESTION_BANK),
        top_students=leaderboard,
        confirmed_edges=edges,
        mem=mem,
        comp_stats=comp_stats,
        wal_stats=wal_stats,
        dag_nodes=dag_nodes
    )

@app.route("/exam", methods=["GET"])
def exam():
    sid_str = request.args.get("student_id", "1")
    try:
        student_id = int(sid_str)
    except ValueError:
        student_id = 1

    session_data = get_or_create_student_session(student_id)

    if session_data.get("completed"):
        return redirect(url_for("exam_completed", student_id=student_id))

    qid = session_data.get("current_question_id", 1)
    q_data = get_question_by_id(qid)
    if not q_data:
        # Fallback to first available question if none found
        q_data = QUESTION_BANK[0]
        session_data["current_question_id"] = q_data["id"]

    topic_name = TOPICS.get(q_data["topic_id"], "General")
    comp_stats = engine.get_compression_stats()
    wal_stats = engine.get_wal_stats()

    return render_template(
        "exam.html",
        student_id=student_id,
        question=q_data,
        questions_bank=QUESTION_BANK,
        topics_dict=TOPICS,
        topic_name=topic_name,
        current_score=session_data["current_score"],
        live_rank=session_data["live_rank"],
        live_percentile=session_data["live_percentile"],
        updated_theta=session_data["updated_theta"],
        is_flagged=session_data["is_flagged"],
        comp_stats=comp_stats,
        wal_stats=wal_stats,
        error=request.args.get("error")
    )

@app.route("/exam/submit", methods=["POST"])
def exam_submit():
    try:
        student_id = int(request.form.get("student_id"))
        question_id = int(request.form.get("question_id"))
        q_type = request.form.get("question_type", "mcq")
        monotonic_dt_ms = max(500, int(request.form.get("monotonic_dt_ms", 5000)))
    except (ValueError, TypeError) as e:
        return redirect(url_for("exam", student_id=1, error="Invalid submission parameters."))

    session_data = get_or_create_student_session(student_id)

    action_type = EduRouteBridge.ACTION_MCQ
    mcq_option = 0xFF
    full_text = None

    if q_type == "mcq":
        try:
            mcq_option = int(request.form.get("mcq_option", 0))
            action_type = EduRouteBridge.ACTION_MCQ
        except ValueError:
            mcq_option = 0
    else:
        full_text = request.form.get("full_text", "").strip()
        action_type = EduRouteBridge.ACTION_TEXT_DELTA

    # Submit to C++ engine via C ABI
    server_ms = int(time.time() * 1000)
    res = engine.submit_answer(
        student_id=student_id,
        question_id=question_id,
        action_type=action_type,
        mcq_option=mcq_option,
        monotonic_dt_ms=monotonic_dt_ms,
        full_text=full_text,
        server_recv_ms=server_ms
    )

    session_data["current_score"] = res["current_score"]
    session_data["live_rank"] = res["live_rank"]
    session_data["live_percentile"] = res["live_percentile"]
    session_data["updated_theta"] = res["updated_theta"]
    session_data["is_flagged"] = res["is_flagged_ring"]
    session_data["cluster_root_id"] = res["cluster_root_id"]
    session_data["answered_count"] += 1

    # Check if the adaptive exam completed (no next question served)
    if res["next_question_id"] is None or session_data["answered_count"] >= 10:
        session_data["completed"] = True
        return redirect(url_for("exam_completed", student_id=student_id))

    session_data["current_question_id"] = res["next_question_id"]
    return redirect(url_for("exam", student_id=student_id))

@app.route("/exam/completed")
def exam_completed():
    student_id = int(request.args.get("student_id", 1))
    session_data = get_or_create_student_session(student_id)
    return render_template(
        "exam_finished.html",
        student_id=student_id,
        current_score=session_data["current_score"],
        live_rank=session_data["live_rank"],
        live_percentile=session_data["live_percentile"],
        updated_theta=session_data["updated_theta"],
        is_flagged=session_data["is_flagged"],
        cluster_root_id=session_data["cluster_root_id"],
        comp_stats=engine.get_compression_stats(),
        wal_stats=engine.get_wal_stats(),
        dag_nodes=engine.get_dag_node_count()
    )

@app.route("/dashboard")
def dashboard():
    auto = request.args.get("auto", "0") == "1"
    leaderboard = engine.get_top_k(50)
    edges = engine.get_collusion_edges(50)
    mem = engine.get_memory_used()
    comp_stats = engine.get_compression_stats()
    wal_stats = engine.get_wal_stats()
    dag_nodes = engine.get_dag_node_count()
    return render_template(
        "dashboard.html",
        auto_refresh=auto,
        leaderboard=leaderboard,
        edges=edges,
        mem=mem,
        comp_stats=comp_stats,
        wal_stats=wal_stats,
        dag_nodes=dag_nodes,
        bank_size=len(QUESTION_BANK),
        notice=request.args.get("notice")
    )

@app.route("/questions")
def questions():
    return render_template(
        "questions_list.html",
        questions=QUESTION_BANK,
        topics=TOPICS
    )

# -----------------------------------------------------------------------------
# Simulation & Demonstration Endpoints
# -----------------------------------------------------------------------------

@app.route("/api/simulate_collusion", methods=["POST"])
def simulate_collusion():
    """
    Demonstrates the anti-cheat pipeline: students 10 and 11 submit
    the exact same essay text with nearly identical timestamps.
    """
    text_plagiarized = (
        "Disjoint Set Union maintains partitioned subsets with find returning root "
        "and path compression flattening the tree hierarchy for near constant time."
    )
    t_now = int(time.time() * 1000)

    # Student 10 submits first
    engine.submit_answer(
        student_id=10,
        question_id=16,
        action_type=EduRouteBridge.ACTION_TEXT_DELTA,
        mcq_option=0xFF,
        monotonic_dt_ms=12400,
        full_text=text_plagiarized,
        server_recv_ms=t_now
    )

    # Student 11 submits identical text 10ms later (triggers k-mer match + DSU edge)
    engine.submit_answer(
        student_id=11,
        question_id=16,
        action_type=EduRouteBridge.ACTION_TEXT_DELTA,
        mcq_option=0xFF,
        monotonic_dt_ms=12415,
        full_text=text_plagiarized,
        server_recv_ms=t_now + 10
    )

    return redirect(url_for("dashboard"))

@app.route("/api/simulate_students", methods=["POST"])
def simulate_students():
    """
    Simulates a batch of random students answering questions to populate leaderboard.
    """
    t_now = int(time.time() * 1000)
    sample_ids = random.sample(range(20, 99), 5)
    for sid in sample_ids:
        # Submit 3 MCQ answers per student
        for qid in [1, 2, 3]:
            dt = random.randint(4000, 25000)
            correct_opt = get_question_by_id(qid)["correct_mcq_option"]
            # 70% chance of choosing correct option
            opt = correct_opt if random.random() < 0.7 else (correct_opt + 1) % 4
            engine.submit_answer(
                student_id=sid,
                question_id=qid,
                action_type=EduRouteBridge.ACTION_MCQ,
                mcq_option=opt,
                monotonic_dt_ms=dt,
                server_recv_ms=t_now
            )
    return redirect(url_for("dashboard"))

@app.route("/api/leaderboard", methods=["GET"])
def api_leaderboard():
    return jsonify(engine.get_top_k(50))

@app.route("/api/collusion", methods=["GET"])
def api_collusion():
    return jsonify(engine.get_collusion_edges(50))

@app.route("/api/telemetry", methods=["GET"])
def api_telemetry():
    return jsonify(engine.get_memory_used())

@app.route("/api/compression_stats", methods=["GET"])
def api_compression_stats():
    return jsonify(engine.get_compression_stats())

@app.route("/api/wal_stats", methods=["GET"])
def api_wal_stats():
    return jsonify(engine.get_wal_stats())

@app.route("/api/compress_demo", methods=["POST"])
def api_compress_demo():
    data = request.get_json(silent=True) or request.form
    old_text = data.get("old_text", "")
    new_text = data.get("new_text", "")
    stats = engine.compress_delta(1, old_text, new_text)
    return jsonify(stats)

@app.route("/api/sync_offline", methods=["POST"])
def api_sync_offline():
    """
    Ingests an array of offline queued submissions stored in browser localStorage.
    Replays each into the C++ engine (WAL + Huffman + Merkle DAG + AntiCheat + IRT).
    """
    data = request.get_json(silent=True)
    if not data or "submissions" not in data:
        return jsonify({"success": False, "error": "No submissions array provided"}), 400

    student_id = int(data.get("student_id", 1))
    session_data = get_or_create_student_session(student_id)
    submissions = data.get("submissions", [])

    results = []
    t_now = int(time.time() * 1000)

    for item in submissions:
        qid = int(item.get("question_id", 1))
        q_type = item.get("question_type", "mcq")
        dt = int(item.get("monotonic_dt_ms", 5000))

        if q_type == "mcq":
            mcq_opt = int(item.get("mcq_option", 0))
            full_txt = None
            act = EduRouteBridge.ACTION_MCQ
        else:
            mcq_opt = 0xFF
            full_txt = item.get("full_text", "")
            act = EduRouteBridge.ACTION_TEXT_DELTA

        res = engine.submit_answer(
            student_id=student_id,
            question_id=qid,
            action_type=act,
            mcq_option=mcq_opt,
            monotonic_dt_ms=dt,
            full_text=full_txt,
            server_recv_ms=t_now
        )

        session_data["current_score"] = res["current_score"]
        session_data["live_rank"] = res["live_rank"]
        session_data["live_percentile"] = res["live_percentile"]
        session_data["updated_theta"] = res["updated_theta"]
        session_data["is_flagged"] = res["is_flagged_ring"]
        session_data["cluster_root_id"] = res["cluster_root_id"]
        session_data["answered_count"] += 1
        if res["next_question_id"] is not None:
            session_data["current_question_id"] = res["next_question_id"]

        results.append(res)

    return jsonify({
        "success": True,
        "synced_count": len(submissions),
        "student_id": student_id,
        "session": session_data,
        "compression_stats": engine.get_compression_stats(),
        "wal_stats": engine.get_wal_stats(),
        "dag_nodes": engine.get_dag_node_count()
    })

@app.route("/api/simulate_offline_sync", methods=["POST"])
def simulate_offline_sync():
    """
    Demonstrates offline resilience: student 99 loses network connection,
    buffers 3 answers to local storage, then reconnects and flushes the buffer.
    """
    sid = 99
    session = get_or_create_student_session(sid)
    t_now = int(time.time() * 1000)

    # Question 1: MCQ answer queued while offline
    engine.submit_answer(
        student_id=sid, question_id=1,
        action_type=EduRouteBridge.ACTION_MCQ,
        mcq_option=2, monotonic_dt_ms=4500,
        server_recv_ms=t_now
    )

    # Question 2: Initial essay draft
    txt1 = "Adjacency list uses dyn_array of vertices to minimize memory."
    engine.submit_answer(
        student_id=sid, question_id=16,
        action_type=EduRouteBridge.ACTION_TEXT_DELTA,
        mcq_option=0xFF, monotonic_dt_ms=8000,
        full_text=txt1, server_recv_ms=t_now + 100
    )

    # Question 2: Edited essay draft (Huffman delta compressed)
    txt2 = "Adjacency list uses dyn_array of vertices to minimize memory and enables O(V+E) graph traversal."
    res = engine.submit_answer(
        student_id=sid, question_id=16,
        action_type=EduRouteBridge.ACTION_TEXT_DELTA,
        mcq_option=0xFF, monotonic_dt_ms=13000,
        full_text=txt2, server_recv_ms=t_now + 200
    )

    session["current_score"] = res["current_score"]
    session["live_rank"] = res["live_rank"]
    session["live_percentile"] = res["live_percentile"]
    session["updated_theta"] = res["updated_theta"]
    session["is_flagged"] = res["is_flagged_ring"]

    notice = f"Successfully recovered and synced 3 offline disk answers for Student #{sid} into WAL and Merkle DAG!"
    return redirect(url_for("dashboard", notice=notice))

@app.route("/api/simulate_wal_restart", methods=["POST"])
def simulate_wal_restart():
    """
    Simulates a mid-flight server crash:
    Appends an unacknowledged WAL record representing an interrupted connection,
    restarts, then recovers and decompresses the Huffman delta from disk.
    """
    sid = 88
    t_now = int(time.time() * 1000)
    unacked_seq = engine.wal_append_unacked(
        student_id=sid, question_id=18, timestamp_ms=t_now,
        old_text="Initial draft before disconnect",
        new_text="Final revision preserved on disk during sudden disconnect"
    )

    # Decompress and recover the pending record from disk WAL
    rec = engine.wal_recover_and_apply_next()
    recovered_text = rec["text"] if rec else "None"

    notice = f"WAL Disk Recovery Check: Found unacked sequence #{unacked_seq} on disk, decompressed Huffman delta: '{recovered_text}'"
    return redirect(url_for("dashboard", notice=notice))

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    print(f"EduRoute Webapp listening on http://0.0.0.0:{port}")
    app.run(host="0.0.0.0", port=port, debug=False)
