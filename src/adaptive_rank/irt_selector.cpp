#include "adaptive_rank/irt_selector.hpp"

#include <cmath>
#include <cstring>

namespace adaptive_rank {

    using namespace core;

    // ================================================================= helpers

    f64 tier_center (DifficultyTier tier) noexcept {
        switch (tier) {
            case DifficultyTier::Tier1: return -2.0;
            case DifficultyTier::Tier2: return -1.0;
            case DifficultyTier::Tier3: return 0.0;
            case DifficultyTier::Tier4: return 1.0;
            case DifficultyTier::Tier5: return 2.0;
            default: return 0.0;
        }
    }

    DifficultyTier tier_for_difficulty (f64 d) noexcept {
        if (d < -1.5) return DifficultyTier::Tier1;
        if (d < -0.5) return DifficultyTier::Tier2;
        if (d < 0.5) return DifficultyTier::Tier3;
        if (d < 1.5) return DifficultyTier::Tier4;
        return DifficultyTier::Tier5; // also catches NaN-free large values
    }

    f64 probability_correct (f64 theta, f64 a, f64 b) noexcept { return 1.0 / (1.0 + std::exp (-a * (theta - b))); }

    i32 theta_to_scaled_score (f64 theta) noexcept {
        if (!(theta == theta)) return 0; // NaN guard
        const f64 t = theta < kMinTheta ? kMinTheta : (theta > kMaxTheta ? kMaxTheta : theta);
        const f64 s = (t - kMinTheta) / (kMaxTheta - kMinTheta) * static_cast<f64> (MAX_SCORE_BUCKET);
        return static_cast<i32> (std::floor (s + 0.5));
    }

    namespace {
        constexpr u16 kAbsent = 0xFFFFU;

        bool valid_discrimination (f64 a) noexcept { return a > 0.0 && a <= kMaxDiscrimination; }
        bool valid_difficulty (f64 b) noexcept { return b >= kMinTheta && b <= kMaxTheta; }
    } // namespace

    // ================================================================= QuestionBank

    usize QuestionBank::required_bytes (u32 capacity) noexcept {
        return static_cast<usize> (capacity) * sizeof (Question) + 65536U * sizeof (u16) +
               2U * alignof (std::max_align_t);
    }

    StatusCode QuestionBank::init (Arena &arena, u32 capacity) noexcept {
        if (capacity == 0U || capacity > 65535U) return StatusCode::InvalidState;
        const Arena::Marker mark = arena.get_marker ();
        auto *items = static_cast<Question *> (arena.allocate (static_cast<usize> (capacity) * sizeof (Question), alignof (Question)));
        auto *index = static_cast<u16 *> (arena.allocate (65536U * sizeof (u16), alignof (u16)));
        if (items == nullptr || index == nullptr) {
            arena.reset_to_marker (mark);
            items_    = nullptr;
            index_    = nullptr;
            capacity_ = 0;
            size_     = 0;
            return StatusCode::OutOfMemory;
        }
        items_    = items;
        index_    = index;
        capacity_ = capacity;
        size_     = 0;
        for (u32 i = 0; i < 65536U; ++i) index_[i] = kAbsent;
        for (u32 i = 0; i < kTierCount; ++i) tier_count_[i] = 0;
        return StatusCode::Ok;
    }

    StatusCode QuestionBank::insert_checked (const Question &q) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        if (q.id == INVALID_QUESTION_ID) return StatusCode::InvalidState;
        if (static_cast<u32> (q.tier) >= kTierCount) return StatusCode::InvalidState;
        if (!valid_discrimination (static_cast<f64> (q.discrimination))) return StatusCode::InvalidState;
        if (!valid_difficulty (static_cast<f64> (q.difficulty))) return StatusCode::InvalidState;
        if (index_[q.id] != kAbsent) return StatusCode::DuplicateSubmission;
        if (size_ >= capacity_) return StatusCode::OutOfMemory;
        items_[size_]  = q;
        index_[q.id]   = static_cast<u16> (size_);
        ++tier_count_[static_cast<u32> (q.tier)];
        ++size_;
        return StatusCode::Ok;
    }

    StatusCode QuestionBank::add (QuestionId id, TopicId topic, DifficultyTier tier, u8 correct_option,
                                  f64 discrimination) noexcept {
        if (static_cast<u32> (tier) >= kTierCount) return StatusCode::InvalidState;
        if (!valid_discrimination (discrimination)) return StatusCode::InvalidState;
        Question q{};
        q.difficulty     = static_cast<f32> (tier_center (tier));
        q.discrimination = static_cast<f32> (discrimination);
        q.id             = id;
        q.topic          = topic;
        q.tier           = tier;
        q.correct_option = correct_option;
        return insert_checked (q);
    }

    StatusCode QuestionBank::add_calibrated (QuestionId id, TopicId topic, f64 difficulty, u8 correct_option,
                                             f64 discrimination) noexcept {
        if (!valid_difficulty (difficulty) || !valid_discrimination (discrimination)) return StatusCode::InvalidState;
        Question q{};
        q.difficulty     = static_cast<f32> (difficulty);
        q.discrimination = static_cast<f32> (discrimination);
        q.id             = id;
        q.topic          = topic;
        q.tier           = tier_for_difficulty (difficulty);
        q.correct_option = correct_option;
        return insert_checked (q);
    }

    u32 QuestionBank::index_of (QuestionId id) const noexcept {
        if (!ready () || index_[id] == kAbsent) return kNoIndex;
        return index_[id];
    }

    const Question *QuestionBank::find (QuestionId id) const noexcept {
        const u32 i = index_of (id);
        return i == kNoIndex ? nullptr : &items_[i];
    }

    const Question *QuestionBank::at (u32 index) const noexcept {
        return (ready () && index < size_) ? &items_[index] : nullptr;
    }

    u32 QuestionBank::count_in_tier (DifficultyTier tier) const noexcept {
        const u32 t = static_cast<u32> (tier);
        return t < kTierCount ? tier_count_[t] : 0U;
    }

    // ================================================================= ExamSession

    usize ExamSession::required_bytes (u32 bank_size, u32 max_questions) noexcept {
        const u32 cap = max_questions < bank_size ? max_questions : bank_size;
        return static_cast<usize> (cap) * (sizeof (u32) + sizeof (u8)) + static_cast<usize> (bank_size) +
               4U * alignof (std::max_align_t);
    }

    StatusCode ExamSession::init (Arena &arena, const QuestionBank &bank, StudentId student, const SelectorConfig &cfg) noexcept {
        if (!bank.ready () || bank.size () == 0U) return StatusCode::InvalidState;
        if (cfg.max_questions == 0U) return StatusCode::InvalidState;
        if (!(cfg.prior_sd > 0.0) || !(cfg.se_stop >= 0.0) || !(cfg.topic_balance >= 0.0)) return StatusCode::InvalidState;
        if (!(cfg.prior_mean == cfg.prior_mean)) return StatusCode::InvalidState;

        const u32 n   = bank.size ();
        const u32 cap = cfg.max_questions < n ? cfg.max_questions : n;

        const Arena::Marker mark = arena.get_marker ();
        auto *idx  = static_cast<u32 *> (arena.allocate (static_cast<usize> (cap) * sizeof (u32), alignof (u32)));
        auto *ok   = static_cast<u8 *> (arena.allocate (cap, 1));
        auto *used = static_cast<u8 *> (arena.allocate (n, 1));
        if (idx == nullptr || ok == nullptr || used == nullptr) {
            arena.reset_to_marker (mark);
            bank_ = nullptr;
            return StatusCode::OutOfMemory;
        }
        std::memset (used, 0, n);

        bank_          = &bank;
        student_       = student;
        cfg_           = cfg;
        if (cfg_.top_k_random < 1U) cfg_.top_k_random = 1U;
        if (cfg_.top_k_random > 8U) cfg_.top_k_random = 8U;
        rng_.reseed (cfg.seed ^ (static_cast<u64> (student) * 0xD6E8FEB86659FD93ULL));
        n_bank_        = n;
        cap_           = cap;
        resp_idx_      = idx;
        resp_ok_       = ok;
        used_          = used;
        n_answered_    = 0;
        n_correct_     = 0;
        total_time_ms_ = 0;
        pending_       = kNone;
        for (u32 i = 0; i < 256U; ++i) topic_used_[i] = 0;

        const f64 m = cfg.prior_mean < kMinTheta ? kMinTheta : (cfg.prior_mean > kMaxTheta ? kMaxTheta : cfg.prior_mean);
        theta_      = m;
        se_         = cfg.prior_sd;
        return StatusCode::Ok;
    }

    bool ExamSession::finished () const noexcept {
        if (!ready ()) return true;
        if (n_answered_ >= cap_) return true;
        return cfg_.se_stop > 0.0 && n_answered_ >= cfg_.min_questions && se_ <= cfg_.se_stop;
    }

    // MAP estimate of ability: Newton-Raphson on the log-likelihood plus a normal prior.
    // The prior keeps the estimate finite even for all-correct / all-wrong students.
    void ExamSession::reestimate () noexcept {
        const f64 var = cfg_.prior_sd * cfg_.prior_sd;
        f64 th        = theta_;
        for (u32 iter = 0; iter < 40U; ++iter) {
            f64 g  = -(th - cfg_.prior_mean) / var; // gradient
            f64 hh = -1.0 / var;                    // second derivative (always < 0)
            for (u32 j = 0; j < n_answered_; ++j) {
                const Question *q = bank_->at (resp_idx_[j]);
                const f64 a       = static_cast<f64> (q->discrimination);
                const f64 p       = probability_correct (th, a, static_cast<f64> (q->difficulty));
                g += a * ((resp_ok_[j] != 0U ? 1.0 : 0.0) - p);
                hh -= a * a * p * (1.0 - p);
            }
            f64 step = -g / hh;
            if (step > 1.0) step = 1.0;
            if (step < -1.0) step = -1.0;
            th += step;
            if (th < kMinTheta) th = kMinTheta;
            if (th > kMaxTheta) th = kMaxTheta;
            if (std::fabs (step) < 1e-7) break;
        }
        // Information at the final estimate -> standard error.
        f64 info = 1.0 / var;
        for (u32 j = 0; j < n_answered_; ++j) {
            const Question *q = bank_->at (resp_idx_[j]);
            const f64 a       = static_cast<f64> (q->discrimination);
            const f64 p       = probability_correct (th, a, static_cast<f64> (q->difficulty));
            info += a * a * p * (1.0 - p);
        }
        theta_ = th;
        se_    = 1.0 / std::sqrt (info);
    }

    // Find the most informative unanswered question(s); return one of the best top_k_random at random.
    bool ExamSession::pick (bool enforce_topic_cap, u32 *out_index) noexcept {
        constexpr u32 kMaxTop = 8;
        u32 top_idx[kMaxTop]  = {};
        f64 top_val[kMaxTop]  = {};
        u32 nt                = 0;
        const u32 k           = cfg_.top_k_random;

        for (u32 i = 0; i < n_bank_; ++i) {
            if (used_[i] != 0U) continue;
            const Question *q = bank_->at (i);
            const u32 used_in_topic = topic_used_[q->topic];
            if (enforce_topic_cap && cfg_.max_per_topic > 0U && used_in_topic >= cfg_.max_per_topic) continue;

            const f64 a    = static_cast<f64> (q->discrimination);
            const f64 p    = probability_correct (theta_, a, static_cast<f64> (q->difficulty));
            const f64 info = a * a * p * (1.0 - p); // Fisher information of this question at theta
            const f64 val  = info / (1.0 + cfg_.topic_balance * static_cast<f64> (used_in_topic));

            if (nt < k || val > top_val[k - 1U]) {
                u32 pos = nt < k ? nt : k - 1U;
                if (nt < k) ++nt;
                while (pos > 0U && top_val[pos - 1U] < val) {
                    top_val[pos] = top_val[pos - 1U];
                    top_idx[pos] = top_idx[pos - 1U];
                    --pos;
                }
                top_val[pos] = val;
                top_idx[pos] = i;
            }
        }
        if (nt == 0U) return false;
        *out_index = top_idx[rng_.below (nt)];
        return true;
    }

    StatusCode ExamSession::next_question (QuestionId *out_id) noexcept {
        if (!ready () || out_id == nullptr) return StatusCode::InvalidState;
        if (finished ()) return StatusCode::InvalidState;

        if (pending_ != kNone && used_[pending_] == 0U) { // same question again until it is answered
            *out_id = bank_->at (pending_)->id;
            return StatusCode::Ok;
        }
        u32 idx = kNone;
        if (!pick (true, &idx) && !pick (false, &idx)) return StatusCode::InvalidState; // bank exhausted
        pending_ = idx;
        *out_id  = bank_->at (idx)->id;
        return StatusCode::Ok;
    }

    StatusCode ExamSession::submit (QuestionId id, bool is_correct, DurationMs time_ms) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        const u32 idx = bank_->index_of (id);
        if (idx == QuestionBank::kNoIndex || idx >= n_bank_) return StatusCode::KeyNotFound;
        if (used_[idx] != 0U) return StatusCode::DuplicateSubmission;
        if (n_answered_ >= cap_) return StatusCode::InvalidState; // exam already full

        used_[idx]                = 1;
        resp_idx_[n_answered_]    = idx;
        resp_ok_[n_answered_]     = is_correct ? 1U : 0U;
        ++n_answered_;
        if (is_correct) ++n_correct_;
        const u64 t = static_cast<u64> (total_time_ms_) + static_cast<u64> (time_ms);
        total_time_ms_ = t > 0xFFFFFFFFULL ? 0xFFFFFFFFU : static_cast<u32> (t); // saturate, never wrap
        const u8 topic = bank_->at (idx)->topic;
        if (topic_used_[topic] < 0xFFFFU) ++topic_used_[topic];
        if (pending_ == idx) pending_ = kNone;
        reestimate ();
        return StatusCode::Ok;
    }

    StatusCode ExamSession::submit_option (QuestionId id, u8 option, DurationMs time_ms, bool *was_correct) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        const Question *q = bank_->find (id);
        if (q == nullptr) return StatusCode::KeyNotFound;
        const bool ok = (option == q->correct_option);
        const StatusCode st = submit (id, ok, time_ms);
        if (st == StatusCode::Ok && was_correct != nullptr) *was_correct = ok;
        return st;
    }

    // ================================================================= Blueprint

    Blueprint Blueprint::from_weights (const f64 (&weights)[kTierCount], u16 n) noexcept {
        Blueprint bp{};
        f64 sum = 0.0;
        f64 w[kTierCount];
        for (u32 i = 0; i < kTierCount; ++i) {
            w[i] = (weights[i] > 0.0 && weights[i] == weights[i]) ? weights[i] : 0.0;
            sum += w[i];
        }
        if (n == 0U) return bp;
        if (!(sum > 0.0)) { // no usable weights: split evenly
            for (u32 i = 0; i < kTierCount; ++i) w[i] = 1.0;
            sum = static_cast<f64> (kTierCount);
        }
        f64 frac[kTierCount];
        u32 given = 0;
        for (u32 i = 0; i < kTierCount; ++i) {
            const f64 quota = w[i] / sum * static_cast<f64> (n);
            const u32 fl    = static_cast<u32> (std::floor (quota + 1e-9)); // epsilon: 0.3/1.0*30 may be 8.9999999
            bp.per_tier[i]  = static_cast<u16> (fl);
            frac[i]         = quota - static_cast<f64> (fl);
            given += fl;
        }
        // Hand out the leftover seats to the biggest fractional parts (earlier tier wins ties).
        while (given < n) {
            u32 best = 0;
            for (u32 i = 1; i < kTierCount; ++i)
                if (frac[i] > frac[best] + 1e-9) best = i; // tolerance: exact ties go to the earlier tier
            ++bp.per_tier[best];
            frac[best] = -1.0;
            ++given;
        }
        return bp;
    }

    Blueprint Blueprint::standard (u16 n) noexcept {
        const f64 w[kTierCount] = {0.15, 0.25, 0.30, 0.20, 0.10};
        return from_weights (w, n);
    }

    Blueprint Blueprint::for_ability (f64 theta, u16 n) noexcept {
        if (!(theta == theta)) theta = 0.0;
        if (theta < kMinTheta) theta = kMinTheta;
        if (theta > kMaxTheta) theta = kMaxTheta;
        f64 w[kTierCount];
        for (u32 i = 0; i < kTierCount; ++i) {
            const f64 d = tier_center (static_cast<DifficultyTier> (i)) - theta;
            w[i]        = std::exp (-0.5 * d * d);
        }
        return from_weights (w, n);
    }

    // ================================================================= build_paper

    namespace {

        // Pick an untaken question of `tier`, preferring the least-used topic (random among equals).
        bool pick_in_tier (const QuestionBank &bank, u32 tier, const u8 *taken, const u16 *topic_used, Rng &rng,
                           u32 *out) noexcept {
            u32 best_use = 0xFFFFFFFFU;
            u32 ties     = 0;
            u32 chosen   = 0;
            bool found   = false;
            for (u32 i = 0; i < bank.size (); ++i) {
                if (taken[i] != 0U) continue;
                const Question *q = bank.at (i);
                if (static_cast<u32> (q->tier) != tier) continue;
                const u32 use = topic_used[q->topic];
                if (!found || use < best_use) {
                    best_use = use;
                    ties     = 1;
                    chosen   = i;
                    found    = true;
                } else if (use == best_use) {
                    ++ties;
                    if (rng.below (ties) == 0U) chosen = i;
                }
            }
            if (found) *out = chosen;
            return found;
        }

        // Shell sort of paper ids by (difficulty, id): easy -> hard.
        void sort_by_difficulty (const QuestionBank &bank, QuestionId *ids, u32 n) noexcept {
            static constexpr u32 gaps[] = {701, 301, 132, 57, 23, 10, 4, 1};
            for (const u32 gap: gaps) {
                for (u32 i = gap; i < n; ++i) {
                    const QuestionId tmp = ids[i];
                    const Question *qt   = bank.find (tmp);
                    u32 j                = i;
                    while (j >= gap) {
                        const Question *qp = bank.find (ids[j - gap]);
                        const bool later   = qp->difficulty > qt->difficulty ||
                                             (qp->difficulty == qt->difficulty && qp->id > qt->id);
                        if (!later) break;
                        ids[j] = ids[j - gap];
                        j -= gap;
                    }
                    ids[j] = tmp;
                }
            }
        }

    } // namespace

    StatusCode build_paper (Arena &arena, const QuestionBank &bank, const Blueprint &blueprint, u64 seed, Paper *out) noexcept {
        if (out == nullptr) return StatusCode::InvalidState;
        *out = Paper{};
        if (!bank.ready () || bank.size () == 0U) return StatusCode::InvalidState;
        const u32 total = blueprint.total ();
        if (total == 0U || total > 65535U) return StatusCode::InvalidState;

        const u32 n_bank = bank.size ();
        const u32 cap    = total < n_bank ? total : n_bank;

        auto *ids = static_cast<QuestionId *> (arena.allocate (static_cast<usize> (cap) * sizeof (QuestionId), alignof (QuestionId)));
        if (ids == nullptr) return StatusCode::OutOfMemory;
        const Arena::Marker scratch_mark = arena.get_marker ();
        auto *taken = static_cast<u8 *> (arena.allocate (n_bank, 1));
        if (taken == nullptr) return StatusCode::OutOfMemory;
        std::memset (taken, 0, n_bank);

        u16 topic_used[256] = {};
        Rng rng (seed);
        u32 placed = 0;
        u32 deficit[kTierCount] = {};

        // Pass 1: each tier takes what it can of its own quota.
        for (u32 t = 0; t < kTierCount; ++t) {
            for (u32 c = 0; c < blueprint.per_tier[t]; ++c) {
                u32 idx = 0;
                if (placed < cap && pick_in_tier (bank, t, taken, topic_used, rng, &idx)) {
                    taken[idx] = 1;
                    ids[placed++] = bank.at (idx)->id;
                    ++topic_used[bank.at (idx)->topic];
                } else {
                    ++deficit[t];
                }
            }
        }
        // Pass 2: unmet quota is borrowed from the nearest tiers (lower side first on ties).
        for (u32 t = 0; t < kTierCount; ++t) {
            for (u32 c = 0; c < deficit[t] && placed < cap; ++c) {
                bool got = false;
                for (u32 dist = 1; dist < kTierCount && !got; ++dist) {
                    for (u32 side = 0; side < 2U && !got; ++side) {
                        if (side == 0U && t < dist) continue;
                        if (side == 1U && t + dist >= kTierCount) continue;
                        const u32 from = side == 0U ? t - dist : t + dist;
                        u32 idx        = 0;
                        if (pick_in_tier (bank, from, taken, topic_used, rng, &idx)) {
                            taken[idx] = 1;
                            ids[placed++] = bank.at (idx)->id;
                            ++topic_used[bank.at (idx)->topic];
                            got = true;
                        }
                    }
                }
            }
        }
        arena.reset_to_marker (scratch_mark); // scratch flags no longer needed

        sort_by_difficulty (bank, ids, placed);

        f64 sum = 0.0;
        for (u32 i = 0; i < placed; ++i) {
            const Question *q = bank.find (ids[i]);
            sum += static_cast<f64> (q->difficulty);
            ++out->tier_counts[static_cast<u32> (q->tier)];
        }
        out->ids             = ids;
        out->count           = static_cast<u16> (placed);
        out->requested       = static_cast<u16> (total);
        out->shortfall       = static_cast<u16> (total - placed);
        out->mean_difficulty = placed > 0U ? sum / static_cast<f64> (placed) : 0.0;
        return StatusCode::Ok;
    }

} // namespace adaptive_rank
