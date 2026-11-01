#pragma once

#include "core/arena_alloc.hpp"
#include "core/types.hpp"

namespace adaptive_rank {

    // Binary Indexed Tree maintaining live score distributions across 0..MAX_SCORE_BUCKET.
    // Gives O(log B) rank and percentile queries without scanning student arrays.
    // Scores outside the valid bucket range are clamped defensively.
    // Backed by arena memory; error states return core::StatusCode.
    class FenwickScore {
        public:
            static constexpr core::u32 kBuckets = core::MAX_SCORE_BUCKET + 1U;

            FenwickScore ()                                = default;
            FenwickScore (const FenwickScore &)            = delete;
            FenwickScore &operator= (const FenwickScore &) = delete;

            /// Bytes to reserve in the arena for this object (includes alignment slack).
            [[nodiscard]] static core::usize required_bytes () noexcept;

            [[nodiscard]] core::StatusCode init (core::Arena &arena) noexcept;
            [[nodiscard]] bool ready () const noexcept { return tree_ != nullptr; }

            /// Forget every student (keeps the memory).
            void clear () noexcept;

            // ---- updates -------------------------------------------------------------------
            [[nodiscard]] core::StatusCode add (core::i32 score) noexcept;
            [[nodiscard]] core::StatusCode remove (core::i32 score) noexcept;
            /// remove(old) + add(new) as ONE step: if old_score is missing, nothing changes.
            [[nodiscard]] core::StatusCode update (core::i32 old_score, core::i32 new_score) noexcept;

            // ---- queries -------------------------------------------------------------------
            [[nodiscard]] core::u32 count_at_most (core::i32 score) const noexcept;
            [[nodiscard]] core::u32 count_less (core::i32 score) const noexcept;
            [[nodiscard]] core::u32 count_greater (core::i32 score) const noexcept;
            [[nodiscard]] core::u32 count_at (core::i32 score) const noexcept;
            /// How many students have lo <= score <= hi (0 if lo > hi).
            [[nodiscard]] core::u32 count_in_range (core::i32 lo, core::i32 hi) const noexcept;
            /// 1 + number of students strictly better. Empty board -> 1.
            [[nodiscard]] core::u32 rank (core::i32 score) const noexcept;
            /// Percentile in [0,100]: share of students below + half of those tied. Empty -> 0.
            [[nodiscard]] core::f64 percentile (core::i32 score) const noexcept;
            /// Score of the student at the given rank (1 = best). KeyNotFound if rank is 0 or > total.
            [[nodiscard]] core::StatusCode score_at_rank (core::u32 rank, core::i32 *out_score) const noexcept;

            [[nodiscard]] core::u32 total () const noexcept { return total_; }

        private:
            core::u32 *tree_{nullptr}; // size kBuckets+1, 1-indexed Fenwick array
            core::u32 *cnt_{nullptr};  // size kBuckets, exact count per bucket
            core::u32 total_{0};

            [[nodiscard]] core::u32 prefix (core::u32 bucket) const noexcept; // students in buckets 0..bucket
    };

} // namespace adaptive_rank
