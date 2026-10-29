#pragma once

#include "core/arena_alloc.hpp"
#include "core/types.hpp"

namespace core::adaptive_rank {

    /**
     * OrderStatAvl - exact leaderboard. A self-balancing binary search tree (AVL) in which every
     * node also remembers how many nodes are in its subtree. That gives, all in O(log n):
     *
     *      insert / erase / update      a student's result appears, disappears or changes
     *      rank_of(key)                 exact position of a student (1 = best)
     *      select(k)                    "who is 5th?"
     *      top_k(k, out)                the podium
     *
     * Keys are core::LeaderboardKey. Their operator< already means "better first":
     * higher scaled_score, then lower total_time_ms, then lower student_id. Because student_id
     * is the last tie-breaker, no two different students ever compare equal.
     *
     * Memory: ONE fixed pool of nodes taken from the arena in init(). Erased nodes go on a
     * free-list and are reused, so insert/erase forever never touches the arena again.
     *
     * Status codes:
     *    Ok                   success
     *    InvalidState         used before init(), capacity 0, or null output pointer
     *    OutOfMemory          arena too small in init(), or tree already holds `capacity` keys
     *    DuplicateSubmission  insert() of a key that is already present
     *    KeyNotFound          erase()/update()/rank_of() of a key that is not present, select() out of range
     */
    class OrderStatAvl {
        public:
            OrderStatAvl () = default;
            OrderStatAvl (const OrderStatAvl &)            = delete;
            OrderStatAvl &operator= (const OrderStatAvl &) = delete;

            [[nodiscard]] static core::usize required_bytes (core::u32 capacity) noexcept;

            [[nodiscard]] core::StatusCode init (core::Arena &arena, core::u32 capacity) noexcept;
            [[nodiscard]] bool ready () const noexcept { return nodes_ != nullptr; }
            void clear () noexcept;

            [[nodiscard]] core::StatusCode insert (const core::LeaderboardKey &key) noexcept;
            [[nodiscard]] core::StatusCode erase (const core::LeaderboardKey &key) noexcept;
            /// Replace old_key with new_key atomically (all-or-nothing).
            [[nodiscard]] core::StatusCode update (const core::LeaderboardKey &old_key,
                                                   const core::LeaderboardKey &new_key) noexcept;

            [[nodiscard]] bool contains (const core::LeaderboardKey &key) const noexcept;
            /// Exact rank (1 = best) of a key that is in the tree.
            [[nodiscard]] core::StatusCode rank_of (const core::LeaderboardKey &key, core::u32 *out_rank) const noexcept;
            /// How many stored keys are strictly better than `key` (works for keys not in the tree too).
            [[nodiscard]] core::u32 count_better (const core::LeaderboardKey &key) const noexcept;
            /// The key at rank k (1 = best).
            [[nodiscard]] core::StatusCode select (core::u32 k, core::LeaderboardKey *out_key) const noexcept;
            /// Copies up to k best keys into out (caller guarantees room for k). Returns how many were written.
            [[nodiscard]] core::u32 top_k (core::u32 k, core::LeaderboardKey *out) const noexcept;

            [[nodiscard]] core::u32 size () const noexcept { return size_; }
            [[nodiscard]] core::u32 capacity () const noexcept { return capacity_; }
            [[nodiscard]] core::u32 height () const noexcept;

            /// Self-check of every AVL / size / ordering invariant. For tests and debugging.
            [[nodiscard]] bool validate () const noexcept;

        private:
            static constexpr core::u32 kNil = 0xFFFFFFFFU;

            struct Node {
                    core::LeaderboardKey key;
                    core::u32 left;
                    core::u32 right;
                    core::u32 subtree_size;
                    core::u8 height;
            };

            Node *nodes_{nullptr};
            core::u32 capacity_{0};
            core::u32 size_{0};
            core::u32 root_{kNil};
            core::u32 next_unused_{0}; // bump pointer into the pool
            core::u32 free_head_{kNil}; // erased nodes, linked through `left`

            [[nodiscard]] core::u32 alloc_node (const core::LeaderboardKey &key) noexcept;
            void free_node (core::u32 idx) noexcept;

            [[nodiscard]] core::i32 h (core::u32 n) const noexcept;
            [[nodiscard]] core::u32 sz (core::u32 n) const noexcept;
            void pull (core::u32 n) noexcept; // recompute height + subtree_size from children
            core::u32 rotate_left (core::u32 x) noexcept;
            core::u32 rotate_right (core::u32 y) noexcept;
            core::u32 rebalance (core::u32 n) noexcept;

            core::u32 insert_rec (core::u32 n, core::u32 idx) noexcept;
            core::u32 erase_rec (core::u32 n, const core::LeaderboardKey &key, bool &found) noexcept;
            core::u32 detach_min (core::u32 n, core::u32 &min_idx) noexcept;
            void collect (core::u32 n, core::u32 k, core::LeaderboardKey *out, core::u32 &written) const noexcept;
            bool check (core::u32 n, const core::LeaderboardKey *lo, const core::LeaderboardKey *hi,
                        core::u32 &nodes_seen, core::i32 &height_out) const noexcept;
    };

} // namespace adaptive_rank
