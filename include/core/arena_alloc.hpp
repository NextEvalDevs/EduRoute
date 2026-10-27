#ifndef ALLOCATOR_HPP
#define ALLOCATOR_HPP

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include "core/types.hpp"

namespace core {

    /**
     * ========================
     *  CORE ARENA ALLOCATOR
     * =========================
     *
     *
     *  1. NO MANUAL DELETES: Never call `free()` or `delete` on pointers from here.
     *  2. TRIVIAL TYPES ONLY: `create<T>()` enforces `is_trivially_destructible`.
     *     Destructors ARE NEVER CALLED when resetting or rolling back.
     *     - Safe:   int, float, POD structs, glm::vec3, fixed-size raw buffers.
     *     - BANNED: std::string, std::vector, std::unique_ptr (causes memory leaks)Note: These are banned anyway.
     *  3. THREAD SAFETY: Not thread-safe. Keep one arena per thread.
     *
     * ----------------------------------------------------------------------------
     *  COMMON USAGE PATTERNS:
     * ----------------------------------------------------------------------------
     *
     *  1. Instantiating Objects (Constructed in-place):
     *     struct Particle { float x, y, z; };
     *     Particle* p = arena.create<Particle>(1.0f, 2.0f, 3.0f);
     *
     *  2. Raw Memory / Arrays:
     *     uint8_t* raw_bytes = static_cast<uint8_t*>(arena.allocate(1024, alignof(uint64_t)));
     *
     *  3. Scoped Rollbacks (Frame / Scratchpad / Temp Calculations):
     *     Automatically unwinds all allocations made inside the scope on exit.
     *
     *     {
     *         core::ScopedArena scratch(arena);
     *         auto* temp = scratch.get().create<Matrix4x4>();
     *         // ... do heavy computations ...
     *     } // <- Memory reclaimed here instantly. Pointers to `temp` are now INVALID.
     *
     *  4. Manual Savepoints (Multi-stage pipelines):
     *     auto checkpoint = arena.get_marker();
     *     // ... allocate things ...
     *     arena.reset_to_marker(checkpoint); // rolls back to checkpoint
     *
     *  5. Nuclear Reset:
     *     arena.reset(); // drops entire buffer usage to 0
     *  6. Check for size while Creating, it DOES NOT reallocate dynamically
     * ============================================================================
     */
    class Arena {
        private:
            std::size_t capacity_{0};
            std::size_t offset_{0};
            uint8_t *buffer_{nullptr};

        public:
            Arena (std::size_t capacity) : capacity_ (capacity), offset_ (0) {
                if (capacity_ > 0) {
                    buffer_ = static_cast<uint8_t *> (std::malloc (capacity_));
                    EDU_ASSERT (buffer_ != nullptr, "Failed to allocate memory");
                }
            }
            ~Arena () {
                if (buffer_) {
                    std::free (buffer_);
                    buffer_ = nullptr;
                }
                capacity_ = 0;
                offset_   = 0;
            }

            Arena (const Arena &)            = delete;
            Arena &operator= (const Arena &) = delete;


            Arena (Arena &&other) noexcept :
                capacity_ (other.capacity_), offset_ (other.offset_), buffer_ (other.buffer_) {
                other.buffer_   = nullptr;
                other.capacity_ = 0;
                other.offset_   = 0;
            }

            /**
             * Use this for raw bytes
             * @param size Size in bytes
             * @param alignment Alignment (ignore if u don't know)
             * @return pointer to the bytes
             */
            // void *allocate (std::size_t size, std::size_t alignment = alignof (std::max_align_t));

            /**
             * Enforces trivial destruyction (int float etc)
             * @tparam T Type
             * @return Pointer to the type obhects
             */
            template<typename T, typename... Args>
            T *create (Args &&...args) {
                static_assert (std::is_trivially_destructible_v<T>,
                               "Arena allocations must be trivially destructible!");
                void *ptr = allocate (sizeof (T), alignof (T));
                if (!ptr) return nullptr;
                return ::new (ptr) T (std::forward<Args> (args)...);
            }

            struct Marker {
                    std::size_t offset;
            };

            Marker get_marker () const noexcept { return Marker{offset_}; }
            void reset_to_marker (Marker marker) noexcept {
                EDU_ASSERT (marker.offset <= offset_, "Cannot rewind");
                offset_ = marker.offset;
            }
            void reset () noexcept { offset_ = 0; };

            [[nodiscard]] usize used () const noexcept { return offset_; }
            [[nodiscard]] usize capacity () const noexcept { return capacity_; }

            void *allocate (std::size_t size, std::size_t alignment) {
                if (size == 0 || buffer_ == nullptr) return nullptr;

                EDU_ASSERT (alignment > 0 && (alignment & (alignment - 1)) == 0, "Alignment must be a power of 2");

                std::uintptr_t caddr        = reinterpret_cast<std::uintptr_t> (buffer_ + offset_);
                std::uintptr_t aligned_addr = (caddr + (alignment - 1)) & ~(alignment - 1);
                std::size_t newoffset       = (aligned_addr - reinterpret_cast<std::uintptr_t> (buffer_)) + size;
                if (newoffset > capacity_) return nullptr;
                offset_ = newoffset;
                return reinterpret_cast<void *> (aligned_addr);
            }

            Arena &operator= (Arena &&other) noexcept {
                if (this != &other) {
                    if (buffer_) std::free (buffer_);
                    buffer_         = other.buffer_;
                    capacity_       = other.capacity_;
                    offset_         = other.offset_;
                    other.buffer_   = nullptr;
                    other.capacity_ = 0;
                    other.offset_   = 0;
                }
                return *this;
            }
    };

    class ScopedArena {
        private:
            Arena &arena_;
            Arena::Marker marker_;

        public:
            explicit ScopedArena (Arena &arena) : arena_ (arena), marker_ (arena.get_marker ()) {}
            ~ScopedArena () { arena_.reset_to_marker (marker_); }
            ScopedArena (const ScopedArena &)            = delete;
            ScopedArena &operator= (const ScopedArena &) = delete;

            Arena &get () noexcept { return arena_; }
    };


} // namespace core


#endif
