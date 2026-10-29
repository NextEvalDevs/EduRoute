#include "adaptive_rank/fenwick_score.hpp"

namespace adaptive_rank {

    using namespace core;

    namespace {

        // Lowest set bit: 12 (1100) -> 4 (0100). Drives every jump in a Fenwick tree.
        constexpr u32 lowbit (u32 i) noexcept { return i & (0U - i); }

        // Biggest power of two <= n (used by the "walk down" rank search).
        constexpr u32 highest_pow2_leq (u32 n) noexcept {
            u32 p = 1;
            while (p <= n / 2U) p <<= 1U;
            return p;
        }

        constexpr u32 kTopBit = highest_pow2_leq (FenwickScore::kBuckets);

        constexpr u32 to_bucket (i32 score) noexcept {
            if (score < 0) return 0;
            if (static_cast<u32> (score) > MAX_SCORE_BUCKET) return MAX_SCORE_BUCKET;
            return static_cast<u32> (score);
        }

    } // namespace

    usize FenwickScore::required_bytes () noexcept {
        return (static_cast<usize> (kBuckets) + 1U) * sizeof (u32) + static_cast<usize> (kBuckets) * sizeof (u32) +
               2U * alignof (std::max_align_t);
    }

    StatusCode FenwickScore::init (Arena &arena) noexcept {
        const Arena::Marker mark = arena.get_marker ();
        auto *tree               = static_cast<u32 *> (
                arena.allocate ((static_cast<usize> (kBuckets) + 1U) * sizeof (u32), alignof (u32)));
        auto *cnt = static_cast<u32 *> (arena.allocate (static_cast<usize> (kBuckets) * sizeof (u32), alignof (u32)));
        if (tree == nullptr || cnt == nullptr) {
            arena.reset_to_marker (mark); // give back what we took, leave object unusable
            tree_  = nullptr;
            cnt_   = nullptr;
            total_ = 0;
            return StatusCode::OutOfMemory;
        }
        tree_ = tree;
        cnt_  = cnt;
        clear ();
        return StatusCode::Ok;
    }

    void FenwickScore::clear () noexcept {
        if (tree_ == nullptr) return;
        for (u32 i = 0; i <= kBuckets; ++i) tree_[i] = 0;
        for (u32 i = 0; i < kBuckets; ++i) cnt_[i] = 0;
        total_ = 0;
    }

    StatusCode FenwickScore::add (i32 score) noexcept {
        if (!ready () || total_ == 0xFFFFFFFFU) return StatusCode::InvalidState;
        const u32 b = to_bucket (score);
        ++cnt_[b];
        ++total_;
        for (u32 i = b + 1U; i <= kBuckets; i += lowbit (i)) ++tree_[i]; // jump UP
        return StatusCode::Ok;
    }

    StatusCode FenwickScore::remove (i32 score) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        const u32 b = to_bucket (score);
        if (cnt_[b] == 0) return StatusCode::KeyNotFound;
        --cnt_[b];
        --total_;
        for (u32 i = b + 1U; i <= kBuckets; i += lowbit (i)) --tree_[i];
        return StatusCode::Ok;
    }

    StatusCode FenwickScore::update (i32 old_score, i32 new_score) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        const StatusCode st = remove (old_score);
        if (st != StatusCode::Ok) return st; // nothing was changed
        return add (new_score);              // cannot fail: remove() just freed one slot
    }

    u32 FenwickScore::prefix (u32 bucket) const noexcept {
        u32 sum = 0;
        for (u32 i = bucket + 1U; i > 0U; i -= lowbit (i)) sum += tree_[i]; // jump DOWN
        return sum;
    }

    u32 FenwickScore::count_at_most (i32 score) const noexcept {
        if (!ready ()) return 0;
        return prefix (to_bucket (score));
    }

    u32 FenwickScore::count_less (i32 score) const noexcept {
        if (!ready ()) return 0;
        const u32 b = to_bucket (score);
        return b == 0U ? 0U : prefix (b - 1U);
    }

    u32 FenwickScore::count_greater (i32 score) const noexcept {
        if (!ready ()) return 0;
        return total_ - prefix (to_bucket (score));
    }

    u32 FenwickScore::count_at (i32 score) const noexcept {
        if (!ready ()) return 0;
        return cnt_[to_bucket (score)];
    }

    u32 FenwickScore::count_in_range (i32 lo, i32 hi) const noexcept {
        if (!ready () || lo > hi) return 0;
        return count_at_most (hi) - count_less (lo);
    }

    u32 FenwickScore::rank (i32 score) const noexcept { return count_greater (score) + 1U; }

    f64 FenwickScore::percentile (i32 score) const noexcept {
        if (!ready () || total_ == 0U) return 0.0;
        const f64 below = static_cast<f64> (count_less (score));
        const f64 tied  = static_cast<f64> (count_at (score));
        return (below + 0.5 * tied) / static_cast<f64> (total_) * 100.0;
    }

    StatusCode FenwickScore::score_at_rank (u32 rank_1_based, i32 *out_score) const noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        if (out_score == nullptr) return StatusCode::InvalidState;
        if (rank_1_based == 0U || rank_1_based > total_) return StatusCode::KeyNotFound;

        // rank r from the top == (total - r + 1)-th from the bottom. Walk down the tree.
        u32 k   = total_ - rank_1_based + 1U;
        u32 pos = 0;
        for (u32 pw = kTopBit; pw > 0U; pw >>= 1U) {
            const u32 next = pos + pw;
            if (next <= kBuckets && tree_[next] < k) {
                pos = next;
                k -= tree_[next];
            }
        }
        *out_score = static_cast<i32> (pos); // 1-indexed slot pos+1 == bucket pos
        return StatusCode::Ok;
    }

} // namespace adaptive_rank
