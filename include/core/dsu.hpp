#pragma once

#include <cassert>
#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/types.hpp"

namespace core {
    /**
     * Disjoint Set Union
     * Origin: Folklore
     */
    class DisjointSet {
        private:
            Array<u32> parent_;
            Array<u8> rank_;
            Array<u32> comp_size_;
            u32 num_elem_{0};
            u32 num_comp_{0};

        public:
            explicit DisjointSet (Arena &arena, u32 n) :
                parent_ (arena, n), rank_ (arena, n), comp_size_ (arena, n), num_elem_ (n), num_comp_ (n) {
                reset ();
            }

            void reset () noexcept {
                parent_.clear ();
                rank_.clear ();
                comp_size_.clear ();
                for (u32 i = 0; i < num_elem_; i++) {
                    parent_.push_back (i);
                    rank_.push_back (0);
                    comp_size_.push_back (1);
                }
            }

            [[nodiscard]] u32 find (u32 x) noexcept {
                EDU_ASSERT (x < num_elem_, "OOB");
                while (x != parent_[x]) {
                    parent_[x] = parent_[parent_[x]];
                    x          = parent_[x];
                }
                return x;
            }

            bool join (u32 x, u32 y) noexcept {
                u32 rx = find (x);
                u32 ry = find (y);
                if (rx == ry) return false;

                if (rank_[rx] < rank_[ry]) swap (rx, ry);
                parent_[ry] = rx;
                comp_size_[rx] += comp_size_[ry];
                if (rank_[rx] == rank_[ry]) ++rank_[rx];
                num_comp_--;
                return true;
            }

            [[nodiscard]] bool connected (u32 x, u32 y) noexcept { return find (x) == find (y); }
            [[nodiscard]] u32 component_size (u32 x) noexcept { return comp_size_[x]; }
            [[nodiscard]] u32 num_components () const noexcept { return num_comp_; }
            [[nodiscard]] u32 size () const noexcept { return num_elem_; }
    };
} // namespace core
