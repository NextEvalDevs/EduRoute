#pragma once

#include "core/arena_alloc.hpp"
#include "core/types.hpp"

namespace adaptive_rank {

    // Item Response Theory (IRT) question selection and ability estimation.
    // 2-parameter logistic model (2PL):
    //   P(correct) = 1 / (1 + exp(-a * (theta - b)))
    // where b is item difficulty and a is discrimination.
    // Supports adaptive item serving, Bayesian/Newton-Raphson ability updates,
    // and fixed paper generation from blueprint distributions.

    // ------------------------------------------------------------------------------------------
    // Small deterministic random generator (xorshift64*). No std::random, same output everywhere.
    // ------------------------------------------------------------------------------------------
    class Rng {
        public:
            explicit Rng (core::u64 seed = 0x9E3779B97F4A7C15ULL) noexcept { reseed (seed); }

            void reseed (core::u64 seed) noexcept {
                state_ = seed ^ 0x9E3779B97F4A7C15ULL;
                if (state_ == 0U) state_ = 0x2545F4914F6CDD1DULL;
            }
            core::u64 next () noexcept {
                core::u64 x = state_;
                x ^= x >> 12U;
                x ^= x << 25U;
                x ^= x >> 27U;
                state_ = x;
                return x * 0x2545F4914F6CDD1DULL;
            }
            /// Uniform integer in [0, n). Returns 0 when n == 0.
            core::u32 below (core::u32 n) noexcept {
                return n == 0U ? 0U : static_cast<core::u32> ((next () >> 32U) % n);
            }
            /// Uniform real in [0, 1).
            core::f64 uniform () noexcept {
                return static_cast<core::f64> (next () >> 11U) * (1.0 / 9007199254740992.0);
            }

        private:
            core::u64 state_{0x2545F4914F6CDD1DULL};
    };

    // ------------------------------------------------------------------------------------------
    // Question + bank
    // ------------------------------------------------------------------------------------------
    constexpr core::f64 kMinTheta          = -4.0;
    constexpr core::f64 kMaxTheta          = 4.0;
    constexpr core::f64 kMaxDiscrimination = 4.0;
    constexpr core::u32 kTierCount         = static_cast<core::u32> (core::DifficultyTier::Count); // 5

    enum class QuestionType : core::u8 { MCQ, DESCRIPTIVE };

    struct Question {
            core::f32 difficulty;     // b, in [-4, 4]
            core::f32 discrimination; // a, in (0, 4]
            core::QuestionId id;
            core::TopicId topic;
            core::DifficultyTier tier;
            QuestionType type;
            core::u8 correct_option; // index of the right MCQ option
    };

    /// Centre difficulty of a tier: Tier1=-2, Tier2=-1, Tier3=0, Tier4=+1, Tier5=+2.
    [[nodiscard]] core::f64 tier_center (core::DifficultyTier tier) noexcept;
    /// Which tier a difficulty value belongs to (boundaries at -1.5, -0.5, +0.5, +1.5).
    [[nodiscard]] core::DifficultyTier tier_for_difficulty (core::f64 difficulty) noexcept;
    /// Probability that a student of ability `theta` answers a question (a, b) correctly.
    [[nodiscard]] core::f64 probability_correct (core::f64 theta, core::f64 a, core::f64 b) noexcept;
    /// Convert ability (-4..4) to the 0..MAX_SCORE_BUCKET scale used by the leaderboard structures.
    [[nodiscard]] core::i32 theta_to_scaled_score (core::f64 theta) noexcept;

    class QuestionBank {
        public:
            QuestionBank ()                                = default;
            QuestionBank (const QuestionBank &)            = delete;
            QuestionBank &operator= (const QuestionBank &) = delete;

            /// Bytes to reserve in the arena for a bank of `capacity` questions.
            [[nodiscard]] static core::usize required_bytes (core::u32 capacity) noexcept;

            /// capacity must be 1..65535.
            [[nodiscard]] core::StatusCode init (core::Arena &arena, core::u32 capacity) noexcept;
            [[nodiscard]] bool ready () const noexcept { return items_ != nullptr; }

            /// Add a question whose difficulty is the CENTRE of its tier (good until you have calibrated data).
            [[nodiscard]] core::StatusCode add (core::QuestionId id, core::TopicId topic, core::DifficultyTier tier,
                                                core::u8 correct_option, core::f64 discrimination = 1.0) noexcept;
            /// Add a question with a measured difficulty; its tier is derived from it.
            [[nodiscard]] core::StatusCode add_calibrated (core::QuestionId id, core::TopicId topic,
                                                           core::f64 difficulty, core::u8 correct_option,
                                                           core::f64 discrimination = 1.0) noexcept;
            [[nodiscard]] core::StatusCode add_descriptive (core::QuestionId id, core::TopicId topic,
                                                            core::DifficultyTier tier) noexcept;
            [[nodiscard]] core::StatusCode add_descriptive_calibrated (core::QuestionId id, core::TopicId topic,
                                                                       core::f64 difficulty) noexcept;
            [[nodiscard]] const Question *find (core::QuestionId id) const noexcept;
            /// Index (0..size-1) of a question id, or kNoIndex.
            [[nodiscard]] core::u32 index_of (core::QuestionId id) const noexcept;
            /// Question at an index. index must be < size() (returns nullptr otherwise).
            [[nodiscard]] const Question *at (core::u32 index) const noexcept;

            [[nodiscard]] core::u32 size () const noexcept { return size_; }
            [[nodiscard]] core::u32 capacity () const noexcept { return capacity_; }
            [[nodiscard]] core::u32 count_in_tier (core::DifficultyTier tier) const noexcept;

            static constexpr core::u32 kNoIndex = 0xFFFFFFFFU;

        private:
            Question *items_{nullptr};
            core::u16 *index_{nullptr}; // id -> position, 65536 entries, 0xFFFF = absent
            core::u32 capacity_{0};
            core::u32 size_{0};
            core::u32 tier_count_[kTierCount]{};

            [[nodiscard]] core::StatusCode insert_checked (const Question &q) noexcept;
    };

    // ------------------------------------------------------------------------------------------
    // Adaptive exam for one student
    // ------------------------------------------------------------------------------------------
    struct SelectorConfig {
            core::u32 max_questions = 20;   // hard cap on questions asked (also capped by bank size)
            core::u32 min_questions = 5;    // SE-based stopping cannot fire before this many answers
            core::f64 se_stop       = 0.0;  // stop when standard error <= this (0 = never stop early)
            core::u32 max_per_topic = 0;    // 0 = unlimited; otherwise a soft cap per topic
            core::f64 topic_balance = 0.25; // >0 nudges selection towards topics not used yet
            core::u32 top_k_random  = 3;    // pick randomly among the k most informative (1..8): exposure control
            core::f64 prior_mean    = 0.0;  // where we assume a new student starts
            core::f64 prior_sd      = 1.0;  // how unsure we are about that (>0)
            core::u64 seed          = 1;    // makes the exam reproducible
    };

    class ExamSession {
        public:
            ExamSession ()                               = default;
            ExamSession (const ExamSession &)            = delete;
            ExamSession &operator= (const ExamSession &) = delete;

            [[nodiscard]] static core::usize required_bytes (core::u32 bank_size, core::u32 max_questions) noexcept;

            /// The bank must outlive the session and must not have questions removed (it cannot remove any).
            [[nodiscard]] core::StatusCode init (core::Arena &arena, const QuestionBank &bank, core::StudentId student,
                                                 const SelectorConfig &cfg) noexcept;
            [[nodiscard]] bool ready () const noexcept { return bank_ != nullptr; }

            /// Serve the most informative unanswered question. Calling it again before answering returns
            /// the SAME question (safe to retry after a crash / lost packet). InvalidState once finished().
            [[nodiscard]] core::StatusCode next_question (core::QuestionId *out_id) noexcept;

            /// Record a graded answer. Works for any bank question, even out of order (offline replay).
            [[nodiscard]] core::StatusCode submit (core::QuestionId id, bool correct,
                                                   core::DurationMs time_ms) noexcept;
            /// Record the chosen MCQ option; it is graded against the bank. *was_correct may be null.
            [[nodiscard]] core::StatusCode submit_option (core::QuestionId id, core::u8 option,
                                                          core::DurationMs time_ms,
                                                          bool *was_correct = nullptr) noexcept;

            [[nodiscard]] bool finished () const noexcept;

            [[nodiscard]] core::StudentId student_id () const noexcept { return student_; }
            [[nodiscard]] core::f64 ability () const noexcept { return theta_; }
            [[nodiscard]] core::f64 standard_error () const noexcept { return se_; }
            [[nodiscard]] core::u32 answered () const noexcept { return n_answered_; }
            [[nodiscard]] core::u32 correct () const noexcept { return n_correct_; }
            [[nodiscard]] core::u32 total_time_ms () const noexcept { return total_time_ms_; }
            /// Ability mapped to 0..MAX_SCORE_BUCKET - feed this to FenwickScore / OrderStatAvl.
            [[nodiscard]] core::i32 scaled_score () const noexcept { return theta_to_scaled_score (theta_); }
            [[nodiscard]] core::LeaderboardKey leaderboard_key () const noexcept {
                return core::LeaderboardKey{scaled_score (), total_time_ms_, student_};
            }

        private:
            static constexpr core::u32 kNone = 0xFFFFFFFFU;

            const QuestionBank *bank_{nullptr};
            core::StudentId student_{core::INVALID_STUDENT_ID};
            SelectorConfig cfg_{};
            Rng rng_{};

            core::u32 n_bank_{0};          // bank size captured at init
            core::u32 cap_{0};             // effective max questions
            core::u32 *resp_idx_{nullptr}; // bank index of each answered question
            core::u8 *resp_ok_{nullptr};   // 1 if that answer was correct
            core::u8 *used_{nullptr};      // used_[bank index] = already answered

            core::u32 n_answered_{0};
            core::u32 n_correct_{0};
            core::u32 total_time_ms_{0};
            core::u32 pending_{kNone}; // bank index currently served but unanswered
            core::f64 theta_{0.0};
            core::f64 se_{1.0};
            core::u16 topic_used_[256]{};

            void reestimate () noexcept;
            [[nodiscard]] bool pick (bool enforce_topic_cap, core::u32 *out_index) noexcept;
    };

    // ------------------------------------------------------------------------------------------
    // Fixed question paper from a difficulty blueprint
    // ------------------------------------------------------------------------------------------
    struct Blueprint {
            core::u16 per_tier[kTierCount]{}; // how many questions from Tier1..Tier5

            [[nodiscard]] core::u32 total () const noexcept {
                core::u32 t = 0;
                for (core::u32 i = 0; i < kTierCount; ++i) t += per_tier[i];
                return t;
            }
            /// Split n questions across tiers proportionally to `weights` (largest-remainder rounding).
            [[nodiscard]] static Blueprint from_weights (const core::f64 (&weights)[kTierCount], core::u16 n) noexcept;
            /// Classic exam shape: 15% / 25% / 30% / 20% / 10% easy -> hard.
            [[nodiscard]] static Blueprint standard (core::u16 n) noexcept;
            /// Concentrates the paper around a target ability (e.g. 1.5 gives a hard paper).
            [[nodiscard]] static Blueprint for_ability (core::f64 theta, core::u16 n) noexcept;
    };

    struct Paper {
            core::QuestionId *ids{nullptr}; // easy -> hard, memory owned by the arena
            core::u16 count{0};             // questions actually placed
            core::u16 requested{0};         // questions the blueprint asked for
            core::u16 shortfall{0};         // requested - count (bank too small)
            core::f64 mean_difficulty{0.0};
            core::u16 tier_counts[kTierCount]{}; // what the paper really contains per tier
    };

    /**
     * Builds a paper: takes the blueprint's count from each tier, spreading across topics as evenly as
     * possible (random among equals, seeded). If a tier lacks questions the gap is filled from the
     * NEAREST tiers. Finally the paper is sorted easy -> hard.
     * Ok even when shortfall > 0 (check it). InvalidState for an empty blueprint/bank or null out.
     */
    [[nodiscard]] core::StatusCode build_paper (core::Arena &arena, const QuestionBank &bank,
                                                const Blueprint &blueprint, core::u64 seed, Paper *out) noexcept;

} // namespace adaptive_rank
