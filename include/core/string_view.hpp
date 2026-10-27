#pragma once

#include <cassert>
#include <cstring>
#include <iostream>
#include "types.hpp"


namespace core {
    class StringView {
        private:
            const char *data_{nullptr};
            std::size_t size_{0};

        public:
            constexpr StringView () noexcept = default;
            constexpr StringView (const char *data_, std::size_t size_) noexcept : data_ (data_), size_ (size_) {}

            constexpr StringView (const char *cstr) noexcept : data_ (cstr), size_ (cstr ? std::strlen (cstr) : 0) {}

            [[nodiscard]] constexpr const char *data () const noexcept { return data_; }
            [[nodiscard]] constexpr std::size_t size () const noexcept { return size_; }
            [[nodiscard]] constexpr std::size_t length () const noexcept { return size_; }
            [[nodiscard]] constexpr bool empty () const noexcept { return size_ == 0; }


            constexpr const char &operator[] (std::size_t index) const noexcept {
                EDU_ASSERT (index < size_, "out of bounds");
                return data_[index];
            }

            [[nodiscard]] constexpr StringView substr (std::size_t pos, std::size_t cnt) const noexcept {
                EDU_ASSERT (pos <= size_, "out of bounds");
                const std::size_t rcnt = (pos + cnt > size_) ? (size_ - pos) : cnt;
                return StringView (data_ + pos, rcnt);
            }

            constexpr void remove_prefix (std::size_t n) noexcept {
                assert (n <= size_);
                data_ += n;
                size_ -= n;
            }

            constexpr void remove_suffix (std::size_t n) noexcept {
                assert (n <= size_);
                size_ -= n;
            }

            [[nodiscard]] bool operator== (const StringView &oth) const noexcept {
                if (size_ != oth.size_) return false;
                if (size_ == 0) return true;
                return std::memcmp (data_, oth.data_, size_) == 0;
            }

            [[nodiscard]] bool operator!= (const StringView &oth) const noexcept { return !(*this == oth); }

            [[nodiscard]] constexpr Hash64 hash () const noexcept { return fnv1a_hash_bytes (data_, size_); }

            [[nodiscard]] const char *begin () const noexcept { return data_; }
            [[nodiscard]] const char *end () const noexcept { return data_ + size_; }
    };
    inline std::ostream &operator<< (std::ostream &os, const StringView &sv) noexcept {
        return os.write (sv.data (), static_cast<std::streamsize> (sv.size ()));
    }
} // namespace core
