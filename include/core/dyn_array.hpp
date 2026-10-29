#pragma  once
#include <cassert>
#include <cstddef>
#include <cstring>
#include "core/arena_alloc.hpp"

namespace core {
    template<typename T>
    class Array {
            static_assert (std::is_trivially_copyable_v<T>, "T must be trivially copyable");

        private:
            Arena &arena_;
            T *data_{nullptr};
            std::size_t size_{0};
            std::size_t capacity_{0};
            void grow (std::size_t min_capacity) {
                std::size_t new_capacity = capacity_ == 0 ? 8 : capacity_ * 2;
                if (new_capacity < min_capacity) new_capacity = min_capacity;

                std::size_t current_bytes = capacity_ * sizeof (T);
                std::size_t new_bytes     = new_capacity * sizeof (T);
                std::size_t needed        = new_bytes - current_bytes;

                uint8_t *expected_end = reinterpret_cast<uint8_t *> (data_) + current_bytes;
                uint8_t *current_top  = reinterpret_cast<uint8_t *> (arena_.used ());

                if (data_ && expected_end == current_top) {
                    void *extra = arena_.allocate (needed, alignof (T));
                    if (extra) {
                        capacity_ = new_capacity;
                        return;
                    }
                }

                T *new_data = static_cast<T *> (arena_.allocate (new_bytes, alignof (T)));
                assert (new_data && "Arena exhausted");
                if (data_ && size_ > 0) std::memcpy (new_data, data_, size_ * sizeof (T));
                data_     = new_data;
                capacity_ = new_capacity;
            }

        public:
            explicit Array (Arena &arena, std::size_t initial_capacity = 0) : arena_ (arena) {
                if (initial_capacity > 0) reserve (initial_capacity);
            }

            Array (const Array &)            = delete;
            Array &operator= (const Array &) = delete;

            Array (Array &&oth) noexcept :
                arena_ (oth.arena_), data_ (oth.data_), size_ (oth.size_), capacity_ (oth.capacity_) {
                oth.data_     = nullptr;
                oth.size_     = 0;
                oth.capacity_ = 0;
            }

            Array &operator= (Array &&oth) noexcept {
                if (this != &oth) {
                    assert (&arena_ == &oth.arena_ && "Cannot Move from different arwena");
                    data_     = oth.data_;
                    size_     = oth.size_;
                    capacity_ = oth.capacity_;

                    oth.data_     = nullptr;
                    oth.size_     = 0;
                    oth.capacity_ = 0;
                }
                return *this;
            }

            T &operator[] (std::size_t index) noexcept {
                assert (index < size_ && "index out of bounds");
                return data_[index];
            }
            const T &operator[] (std::size_t index) const noexcept {
                assert (index < size_ && "index out of bounds");
                return data_[index];
            }

            T *data () noexcept { return data_; }
            const T *data () const noexcept { return data_; }

            [[nodiscard]] std::size_t size () const noexcept { return size_; }
            [[nodiscard]] std::size_t capacity () const noexcept { return capacity_; }
            [[nodiscard]] bool empty () const noexcept { return size_ == 0; }

            void reserve (std::size_t new_capacity) {
                if (new_capacity > capacity_) grow (new_capacity);
            }

            void push_back (const T &val) {
                if (size_ == capacity_) grow (size_ + 1);
                data_[size_++] = val;
            }

            template<typename... Args>
            T &emplace_back (Args &&...args) {
                if (size_ == capacity_) grow (size_ + 1);
                ::new (static_cast<void *> (data_ + size_)) T (std::forward<Args> (args)...);
                return data_[size_++];
            }

            void pop_back () {
                assert (size_ > 0 && "pop_back() called on an empty array");
                size_--;
            }
            void clear () noexcept { size_ = 0; }

            T *begin () noexcept { return data_; }
            T *end () noexcept { return data_ + size_; }
            const T *begin () const noexcept { return data_; }
            const T *end () const noexcept { return data_ + size_; }
    };

} // namespace core


