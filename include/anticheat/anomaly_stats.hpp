#pragma once

#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"
#include "core/types.hpp"

namespace core::anticheat {

    /**
     * AnomalyStats - Statistical anomaly detection for anti-cheat.
     *
     * Tracks two signals per (StudentId, QuestionId) pair:
     *
     *   1. TIME-SYNC SPIKE: Two students' Δt values are suspiciously close.
     *      If |Δt_A - Δt_B| <= TIME_SYNC_THRESHOLD_MS, time_score increments.
     *
     *   2. SHARED RARE ERROR: Students select the same WRONG MCQ option on a
     *      question where that option is rarely chosen (< RARE_ERROR_FREQ_THRESHOLD).
     *      err_score increments for the pair.
     *
     * Both signals are accumulated into per-pair weights fed to CollusionGraph.
     *
     * Memory: all storage lives in the Arena supplied at construction.
     *         No individual frees ever occur.
     *
     * Status codes used:
     *    Ok            success / flag raised
     *    KeyNotFound   query for a pair that has no record yet
     *    InvalidState  used before init() or capacity overflow
     */
    struct StatAnomalyMatch {
        StudentId other_student; // the peer being compared against
        f32       time_score;    // accumulated Δt-sync suspicion [0, ∞)
        f32       err_score;     // accumulated shared-rare-error suspicion [0, ∞)
    };

    class AnomalyStats {
    public:
        // Δt within this window (ms) is flagged as suspicious
        static constexpr u32 TIME_SYNC_THRESHOLD_MS    = 500u;
        // Option chosen by fewer than this fraction of students is "rare"
        static constexpr f32 RARE_ERROR_FREQ_THRESHOLD = 0.10f;
        // Score increment per time-sync hit
        static constexpr f32 TIME_HIT_WEIGHT           = 1.0f;
        // Score increment per shared-rare-error hit
        static constexpr f32 ERR_HIT_WEIGHT            = 2.0f;

        AnomalyStats ()                                = default;
        AnomalyStats (const AnomalyStats &)            = delete;
        AnomalyStats &operator= (const AnomalyStats &) = delete;

        /**
         * Must be called once before any other method.
         * @param arena     persistent arena for index storage
         * @param max_pairs expected peak number of unique student-pairs
         */
        [[nodiscard]] StatusCode init (Arena &arena, usize max_pairs = 8192) noexcept;
        [[nodiscard]] bool       ready () const noexcept { return ready_; }
        void                     clear () noexcept;

        // ---- Time-sync detection ---------------------------------------------------

        /**
         * Record a student's Δt for a question.
         * Scans the existing Δt records for the same question; every student whose
         * |Δt_other - dt_ms| <= TIME_SYNC_THRESHOLD_MS gets a time_score hit added
         * to the pair (sid, other) in the pair map.
         *
         * @param sid    submitting student
         * @param qid    question being answered
         * @param dt_ms  client monotonic Δt in milliseconds
         * @param scratch scratch arena for temporary work (reset after return)
         * @param out_matches filled with peers that triggered hits (may be empty)
         */
        void record_dt (StudentId sid, QuestionId qid, u32 dt_ms,
                        Arena &scratch,
                        Array<StatAnomalyMatch> &out_matches) noexcept;

        // ---- Shared-rare-error detection -------------------------------------------

        /**
         * Record a student's MCQ choice for a question.
         * Updates the global frequency table for (qid, option).  If the option is
         * wrong AND the fraction of students who chose it is < RARE_ERROR_FREQ_THRESHOLD,
         * every other student who chose the same (wrong) option gets an err_score hit.
         *
         * @param sid            submitting student
         * @param qid            question
         * @param chosen_option  0..3 option the student selected
         * @param correct_option correct answer (0..3)
         * @param scratch        scratch arena
         * @param out_matches    peers that triggered err-score hits
         */
        void record_mcq (StudentId sid, QuestionId qid,
                         u8 chosen_option, u8 correct_option,
                         Arena &scratch,
                         Array<StatAnomalyMatch> &out_matches) noexcept;

        // ---- Queries ---------------------------------------------------------------

        /**
         * Retrieve accumulated time and error scores for a pair.
         * Returns KeyNotFound if the pair has never been recorded.
         * @param a, b  student IDs (order does not matter)
         */
        [[nodiscard]] StatusCode get_pair_scores (StudentId a, StudentId b,
                                                  f32 *out_time_score,
                                                  f32 *out_err_score) const noexcept;

    private:
        // --- Δt index: maps (qid<<32|sid) -> dt_ms --------------------------------
        // Stores only the most-recent Δt per (student, question).
        HashMap<u64, u32> *dt_index_{nullptr};

        // --- MCQ posting list: maps (qid<<8|option) -> PostingNode* ---------------
        struct McqPostingNode {
            StudentId       student_id;
            McqPostingNode *next;
        };
        HashMap<u32, McqPostingNode *> *mcq_index_{nullptr};

        // --- Per-(qid,option) submission count (for frequency) --------------------
        HashMap<u32, u32> *option_cnt_{nullptr};
        HashMap<u32, u32> *question_total_{nullptr}; // total submissions per qid

        // --- Accumulated pair suspicion scores ------------------------------------
        struct PairScore {
            f32 time_score{0.0f};
            f32 err_score{0.0f};
        };
        HashMap<u64, PairScore> *pair_map_{nullptr};

        Arena   *arena_{nullptr};
        bool     ready_{false};

        // Canonical pair key: always puts the smaller ID first.
        static constexpr u64 pair_key (StudentId a, StudentId b) noexcept {
            const StudentId lo = (a < b) ? a : b;
            const StudentId hi = (a < b) ? b : a;
            return (static_cast<u64> (lo) << 32u) | static_cast<u64> (hi);
        }

        static constexpr u64 dt_key (QuestionId qid, StudentId sid) noexcept {
            return (static_cast<u64> (qid) << 32u) | static_cast<u64> (sid);
        }

        static constexpr u32 mcq_key (QuestionId qid, u8 option) noexcept {
            return (static_cast<u32> (qid) << 8u) | static_cast<u32> (option);
        }

        // Increment or insert the pair score; also appends to out_matches.
        void add_pair_hit (StudentId a, StudentId b,
                           f32 time_delta, f32 err_delta,
                           Array<StatAnomalyMatch> &out_matches) noexcept;
    };

} // namespace core::anticheat
