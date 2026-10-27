#pragma once

#include "core/arena_alloc.hpp"
#include "core/string_view.hpp"
#include "core/types.hpp"

namespace core {
    /**
     * @cite Splitmax64 Adapted from https://github.com/svaarala/duktape/blob/master/misc/splitmix64.c
     * @namespace core
     * @tparam K type (can be u32 u64 etc)
     */
    template<typename K>
    struct DefaultHash {
            static constexpr u64 hash (const K &key) noexcept {
                if constexpr ((std::is_same_v<K, StringView>) )
                    return key.hash ();
                else {
                    u64 x = static_cast<u64> (key);
                    x     = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
                    x     = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
                    x     = x ^ (x >> 31);
                    return x;
                }
            }
    };

    template<typename K, typename V, typename Hasher = DefaultHash<K>>
    class HashMap {
            static_assert (std::is_trivially_copyable_v<K>);
            static_assert (std::is_trivially_destructible_v<V>);


        private:
            struct Slot {
                    K key;
                    V val;
                    u32 psl{0};
            };

            Arena &arena_;
            Slot *slots_{nullptr};
            usize capacity_{0};
            usize size_{0};

            static constexpr usize next_pow2 (usize n) noexcept {
                if (n == 0) return 1;
                n--;
                n |= n >> 1;
                n |= n >> 2;
                n |= n >> 4;
                n |= n >> 8;
                n |= n >> 16;
                n |= n >> 32;
                return ++n;
            }

            void allocate_table (usize cap) {
                capacity_ = next_pow2 (cap);
                slots_    = static_cast<Slot *> (arena_.allocate (capacity_ * sizeof (Slot), alignof (Slot)));
                assert (slots_);
                memset (slots_, 0, capacity_ * sizeof (Slot));
            }

            void rehash (usize newcap) {
                Slot *oldslots = slots_;
                usize oldcap   = capacity_;
                allocate_table (newcap);
                size_ = 0;
                for (usize i = 0; i < oldcap; i++)
                    if (oldslots[i].psl != 0) insert (oldslots[i].key, oldslots[i].val);
            }


        public:
            explicit HashMap (Arena &arena, usize initcap = 256) : arena_ (arena), capacity_ (initcap) {
                allocate_table (initcap);
            }

            HashMap (const HashMap &)            = delete;
            HashMap &operator= (const HashMap &) = delete;


            [[nodiscard]] usize size () const noexcept { return size_; }
            [[nodiscard]] usize capacity () const noexcept { return capacity_; }
            [[nodiscard]] bool empty () const noexcept { return size_ == 0; }


            V *insert (const K &key, V val) {
                if (size_ * 5 >= capacity_ * 4) rehash (capacity_ * 2);

                const usize mask = capacity_ - 1;
                usize idx        = Hasher::hash (key) & mask;

                K curr_key = key;
                V curr_val = val;

                u32 curr_psl = 1;

                V *result = nullptr;

                while (true) {
                    if (slots_[idx].psl == 0) {
                        slots_[idx].key = curr_key;
                        ::new (static_cast<void *> (&slots_[idx].val)) V (std::move (curr_val));
                        slots_[idx].psl = curr_psl;
                        ++size_;
                        if (!result) result = &slots_[idx].val;
                        return result;
                    }

                    if (slots_[idx].key == curr_key) {
                        slots_[idx].val = std::move (curr_val);
                        return &slots_[idx].val;
                    }

                    if (curr_psl > slots_[idx].psl) {
                        if (!result) result = &slots_[idx].val;
                        swap (curr_key, slots_[idx].key);
                        swap (curr_val, slots_[idx].val);
                        swap (curr_psl, slots_[idx].psl);
                    }
                    ++curr_psl;
                    idx = (idx + 1) & mask;
                }
            }

            [[nodiscard]] V *find (const K &key) noexcept {
                usize mask = capacity_ - 1;
                usize idx  = Hasher::hash (key) & mask;
                u32 psl    = 1;

                while (true) {
                    if (slots_[idx].psl == 0 || psl > slots_[idx].psl) return nullptr;
                    if (slots_[idx].key == key) return &slots_[idx].val;
                    ++psl;
                    idx = (idx + 1) & mask;
                }
            }

            [[nodiscard]] const V *find (const K &key) const noexcept {
                return const_cast<HashMap *> (this)->find (key);
            }

            void clear () noexcept {
                std::memset (slots_, 0, capacity_ * sizeof (Slot));
                size_ = 0;
            }

            constexpr V &operator[] (const K &key) noexcept {
                V *ptr = find (key);
                if (!ptr) ptr = insert (key, V{});
                return *ptr;
            }
            V &operator[] (K &&key) {
                V *ptr = find (key);
                if (!ptr) ptr = insert (key, V{});
                return *ptr;
            }
    };
} // namespace core
