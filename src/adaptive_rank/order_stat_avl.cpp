#include "adaptive_rank/order_stat_avl.hpp"

namespace core::adaptive_rank {

    using namespace core;

    usize OrderStatAvl::required_bytes (u32 capacity) noexcept {
        return static_cast<usize> (capacity) * sizeof (Node) + alignof (std::max_align_t);
    }

    StatusCode OrderStatAvl::init (Arena &arena, u32 capacity) noexcept {
        if (capacity == 0U || capacity >= kNil) return StatusCode::InvalidState;
        auto *pool =
                static_cast<Node *> (arena.allocate (static_cast<usize> (capacity) * sizeof (Node), alignof (Node)));
        if (pool == nullptr) {
            nodes_    = nullptr;
            capacity_ = 0;
            size_     = 0;
            return StatusCode::OutOfMemory;
        }
        nodes_    = pool;
        capacity_ = capacity;
        clear ();
        return StatusCode::Ok;
    }

    void OrderStatAvl::clear () noexcept {
        size_        = 0;
        root_        = kNil;
        next_unused_ = 0;
        free_head_   = kNil;
    }

    // ---------------------------------------------------------------- node pool

    u32 OrderStatAvl::alloc_node (const LeaderboardKey &key) noexcept {
        u32 idx = kNil;
        if (free_head_ != kNil) {
            idx        = free_head_;
            free_head_ = nodes_[idx].left;
        } else if (next_unused_ < capacity_) {
            idx = next_unused_++;
        } else {
            return kNil;
        }
        nodes_[idx].key          = key;
        nodes_[idx].left         = kNil;
        nodes_[idx].right        = kNil;
        nodes_[idx].subtree_size = 1;
        nodes_[idx].height       = 1;
        return idx;
    }

    void OrderStatAvl::free_node (u32 idx) noexcept {
        nodes_[idx].left = free_head_;
        free_head_       = idx;
    }

    // ---------------------------------------------------------------- AVL helpers

    i32 OrderStatAvl::h (u32 n) const noexcept { return n == kNil ? 0 : static_cast<i32> (nodes_[n].height); }
    u32 OrderStatAvl::sz (u32 n) const noexcept { return n == kNil ? 0U : nodes_[n].subtree_size; }

    void OrderStatAvl::pull (u32 n) noexcept {
        const i32 hl           = h (nodes_[n].left);
        const i32 hr           = h (nodes_[n].right);
        nodes_[n].height       = static_cast<u8> ((hl > hr ? hl : hr) + 1);
        nodes_[n].subtree_size = sz (nodes_[n].left) + sz (nodes_[n].right) + 1U;
    }

    u32 OrderStatAvl::rotate_left (u32 x) noexcept {
        const u32 y     = nodes_[x].right;
        nodes_[x].right = nodes_[y].left;
        nodes_[y].left  = x;
        pull (x);
        pull (y);
        return y;
    }

    u32 OrderStatAvl::rotate_right (u32 y) noexcept {
        const u32 x     = nodes_[y].left;
        nodes_[y].left  = nodes_[x].right;
        nodes_[x].right = y;
        pull (y);
        pull (x);
        return x;
    }

    u32 OrderStatAvl::rebalance (u32 n) noexcept {
        pull (n);
        const i32 balance = h (nodes_[n].left) - h (nodes_[n].right);
        if (balance > 1) {
            const u32 l = nodes_[n].left;
            if (h (nodes_[l].left) < h (nodes_[l].right)) nodes_[n].left = rotate_left (l);
            return rotate_right (n);
        }
        if (balance < -1) {
            const u32 r = nodes_[n].right;
            if (h (nodes_[r].right) < h (nodes_[r].left)) nodes_[n].right = rotate_right (r);
            return rotate_left (n);
        }
        return n;
    }

    // ---------------------------------------------------------------- insert / erase

    u32 OrderStatAvl::insert_rec (u32 n, u32 idx) noexcept {
        if (n == kNil) return idx;
        if (nodes_[idx].key < nodes_[n].key)
            nodes_[n].left = insert_rec (nodes_[n].left, idx);
        else
            nodes_[n].right = insert_rec (nodes_[n].right, idx);
        return rebalance (n);
    }

    StatusCode OrderStatAvl::insert (const LeaderboardKey &key) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        if (contains (key)) return StatusCode::DuplicateSubmission;
        const u32 idx = alloc_node (key);
        if (idx == kNil) return StatusCode::OutOfMemory;
        root_ = insert_rec (root_, idx);
        ++size_;
        return StatusCode::Ok;
    }

    // Removes the smallest node of the subtree at n (does not free it). Returns the new subtree root.
    u32 OrderStatAvl::detach_min (u32 n, u32 &min_idx) noexcept {
        if (nodes_[n].left == kNil) {
            min_idx = n;
            return nodes_[n].right;
        }
        nodes_[n].left = detach_min (nodes_[n].left, min_idx);
        return rebalance (n);
    }

    u32 OrderStatAvl::erase_rec (u32 n, const LeaderboardKey &key, bool &found) noexcept {
        if (n == kNil) return kNil;
        if (key < nodes_[n].key) {
            nodes_[n].left = erase_rec (nodes_[n].left, key, found);
        } else if (nodes_[n].key < key) {
            nodes_[n].right = erase_rec (nodes_[n].right, key, found);
        } else {
            found = true;
            if (nodes_[n].left == kNil || nodes_[n].right == kNil) {
                const u32 child = nodes_[n].left != kNil ? nodes_[n].left : nodes_[n].right;
                free_node (n);
                return child;
            }
            // Two children: the smallest node of the right side takes this node's place.
            u32 min_idx           = kNil;
            const u32 new_right   = detach_min (nodes_[n].right, min_idx);
            nodes_[min_idx].left  = nodes_[n].left;
            nodes_[min_idx].right = new_right;
            free_node (n);
            return rebalance (min_idx);
        }
        return rebalance (n);
    }

    StatusCode OrderStatAvl::erase (const LeaderboardKey &key) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        bool found = false;
        root_      = erase_rec (root_, key, found);
        if (!found) return StatusCode::KeyNotFound;
        --size_;
        return StatusCode::Ok;
    }

    StatusCode OrderStatAvl::update (const LeaderboardKey &old_key, const LeaderboardKey &new_key) noexcept {
        if (!ready ()) return StatusCode::InvalidState;
        if (!contains (old_key)) return StatusCode::KeyNotFound;
        if (old_key == new_key) return StatusCode::Ok;
        if (contains (new_key)) return StatusCode::DuplicateSubmission; // nothing changed
        const StatusCode e = erase (old_key);
        if (e != StatusCode::Ok) return e;
        return insert (new_key); // cannot fail: erase() just freed a node
    }

    // ---------------------------------------------------------------- queries

    bool OrderStatAvl::contains (const LeaderboardKey &key) const noexcept {
        if (!ready ()) return false;
        u32 n = root_;
        while (n != kNil) {
            if (key < nodes_[n].key)
                n = nodes_[n].left;
            else if (nodes_[n].key < key)
                n = nodes_[n].right;
            else
                return true;
        }
        return false;
    }

    u32 OrderStatAvl::count_better (const LeaderboardKey &key) const noexcept {
        if (!ready ()) return 0;
        u32 n   = root_;
        u32 cnt = 0;
        while (n != kNil) {
            if (nodes_[n].key < key) { // this node and its whole left side are better than key
                cnt += sz (nodes_[n].left) + 1U;
                n = nodes_[n].right;
            } else {
                n = nodes_[n].left;
            }
        }
        return cnt;
    }

    StatusCode OrderStatAvl::rank_of (const LeaderboardKey &key, u32 *out_rank) const noexcept {
        if (!ready () || out_rank == nullptr) return StatusCode::InvalidState;
        if (!contains (key)) return StatusCode::KeyNotFound;
        *out_rank = count_better (key) + 1U;
        return StatusCode::Ok;
    }

    StatusCode OrderStatAvl::select (u32 k, LeaderboardKey *out_key) const noexcept {
        if (!ready () || out_key == nullptr) return StatusCode::InvalidState;
        if (k == 0U || k > size_) return StatusCode::KeyNotFound;
        u32 n = root_;
        while (n != kNil) {
            const u32 left = sz (nodes_[n].left);
            if (k <= left) {
                n = nodes_[n].left;
            } else if (k == left + 1U) {
                *out_key = nodes_[n].key;
                return StatusCode::Ok;
            } else {
                k -= left + 1U;
                n = nodes_[n].right;
            }
        }
        return StatusCode::KeyNotFound; // unreachable if invariants hold
    }

    void OrderStatAvl::collect (u32 n, u32 k, LeaderboardKey *out, u32 &written) const noexcept {
        if (n == kNil || written >= k) return;
        collect (nodes_[n].left, k, out, written);
        if (written >= k) return;
        out[written++] = nodes_[n].key;
        collect (nodes_[n].right, k, out, written);
    }

    u32 OrderStatAvl::top_k (u32 k, LeaderboardKey *out) const noexcept {
        if (!ready () || out == nullptr || k == 0U) return 0;
        u32 written = 0;
        collect (root_, k, out, written);
        return written;
    }

    u32 OrderStatAvl::height () const noexcept { return ready () ? static_cast<u32> (h (root_)) : 0U; }

    // ---------------------------------------------------------------- self-check

    bool OrderStatAvl::check (u32 n, const LeaderboardKey *lo, const LeaderboardKey *hi, u32 &nodes_seen,
                              i32 &height_out) const noexcept {
        if (n == kNil) {
            height_out = 0;
            return true;
        }
        if (nodes_seen > capacity_) return false; // cycle guard
        ++nodes_seen;
        const LeaderboardKey &k = nodes_[n].key;
        if (lo != nullptr && !(*lo < k)) return false;
        if (hi != nullptr && !(k < *hi)) return false;
        i32 hl = 0, hr = 0;
        if (!check (nodes_[n].left, lo, &k, nodes_seen, hl)) return false;
        if (!check (nodes_[n].right, &k, hi, nodes_seen, hr)) return false;
        const i32 bal = hl - hr;
        if (bal > 1 || bal < -1) return false;
        height_out = (hl > hr ? hl : hr) + 1;
        if (static_cast<i32> (nodes_[n].height) != height_out) return false;
        return nodes_[n].subtree_size == sz (nodes_[n].left) + sz (nodes_[n].right) + 1U;
    }

    bool OrderStatAvl::validate () const noexcept {
        if (!ready ()) return true;
        u32 seen = 0;
        i32 ht   = 0;
        if (!check (root_, nullptr, nullptr, seen, ht)) return false;
        return seen == size_ && sz (root_) == size_;
    }

} // namespace core::adaptive_rank
