"""
EduRoute Web Application
Simple, low-bandwidth Flask frontend strictly using the C ABI (libeduroute.so).
Designed for low-end hardware and low internet bandwidth.
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
# 1. Global C++ Engine Bridge Initialization
# -----------------------------------------------------------------------------
WAL_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "eduroute_wal.bin")
engine = EduRouteBridge(max_students=500, max_questions=len(QUESTION_BANK) + 10, wal_path=WAL_FILE)

# Register all questions into the C++ backend Question Bank
for q in QUESTION_BANK:
    engine.register_question(
        question_id=q["id"],
        topic_id=q["topic_id"],
        difficulty_beta=float(q["difficulty_beta"]),
        expected_time_ms=int(q["expected_time_ms"]),
        correct_mcq_option=int(q["correct_mcq_option"])
    )

# In-memory student session cache (student_id -> state dict)
# C++ backend remains single source of truth for scores, rank, theta, and DSU clusters.
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
# 2. Web Routes
# -----------------------------------------------------------------------------

@app.route("/")
def index():
    mem = engine.get_memory_used()
    leaderboard = engine.get_top_k(50)
    edges = engine.get_collusion_edges(50)
    return render_template(
        "index.html",
        bank_size=len(QUESTION_BANK),
        top_students=leaderboard,
        confirmed_edges=edges,
        mem=mem
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

    return render_template(
        "exam.html",
        student_id=student_id,
        question=q_data,
        topic_name=topic_name,
        current_score=session_data["current_score"],
        live_rank=session_data["live_rank"],
        live_percentile=session_data["live_percentile"],
        updated_theta=session_data["updated_theta"],
        is_flagged=session_data["is_flagged"],
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
        cluster_root_id=session_data["cluster_root_id"]
    )

@app.route("/dashboard")
def dashboard():
    auto = request.args.get("auto", "0") == "1"
    leaderboard = engine.get_top_k(50)
    edges = engine.get_collusion_edges(50)
    mem = engine.get_memory_used()
    return render_template(
        "dashboard.html",
        auto_refresh=auto,
        leaderboard=leaderboard,
        edges=edges,
        mem=mem,
        bank_size=len(QUESTION_BANK)
    )

@app.route("/questions")
def questions():
    return render_template(
        "questions_list.html",
        questions=QUESTION_BANK,
        topics=TOPICS
    )

# -----------------------------------------------------------------------------
# 3. Demonstration & Simulation Endpoints
# -----------------------------------------------------------------------------

@app.route("/api/simulate_collusion", methods=["POST"])
def simulate_collusion():
    """
    Demonstrates the anticheat engine by simulating two students (#10 and #11)
    submitting the same plagiarized answer text with synchronous timing on Question #16.
    """
    text_plagiarized = (
        "Disjoint Set Union maintains partitioned subsets with find returning root "
        "and path compression flattening the tree hierarchy for near constant time."
    )
    t_now = int(time.time() * 1000)

    # Student 10 submits
    engine.submit_answer(
        student_id=10,
        question_id=16,
        action_type=EduRouteBridge.ACTION_TEXT_DELTA,
        mcq_option=0xFF,
        monotonic_dt_ms=12400,
        full_text=text_plagiarized,
        server_recv_ms=t_now
    )

    # Student 11 submits identical text 10ms later (suspicious delta sync + identical k-mers)
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
    Simulates 5 legitimate random students answering questions to populate the leaderboard.
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

if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    print(f"EduRoute Webapp listening on http://0.0.0.0:{port}")
    app.run(host="0.0.0.0", port=port, debug=False)
