#ifndef ALLOCATOR_HPP
#define ALLOCATOR_HPP

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
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
     *  CHEAT SHEET / COMMON USAGE PATTERNS:
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
            explicit Arena (std::size_t capacity);
            ~Arena ();

            Arena (const Arena &)            = delete;
            Arena &operator= (const Arena &) = delete;


            Arena (Arena &&other) noexcept;
            Arena &operator= (Arena &&other) noexcept;

            /**
             * Use this for raw bytes
             * @param size Size in bytes
             * @param alignment Alignment (ignore if u don't know)
             * @return pointer to the bytes
             */
            void *allocate (std::size_t size, std::size_t alignment = alignof (std::max_align_t));

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
            void reset_to_marker (Marker marker) noexcept;
            void reset () noexcept { offset_ = 0; };

            std::size_t used () const noexcept { return offset_; }
            std::size_t capacity () const noexcept { return capacity_; }
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
