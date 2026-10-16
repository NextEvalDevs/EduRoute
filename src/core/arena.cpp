#include <cassert>
#include <cstdlib>


#include "core/arena_alloc.hpp"

namespace core {

    Arena::Arena (std::size_t capacity) : capacity_ (capacity), offset_ (0) {
        if (capacity_ > 0) {
            buffer_ = static_cast<uint8_t *> (std::malloc (capacity_));
            assert (buffer_ != nullptr && "Failed to allocate memory");
        }
    }

    Arena::~Arena () {
        if (buffer_) {
            std::free (buffer_);
            buffer_ = nullptr;
        }
        capacity_ = 0;
        offset_   = 0;
    }

    Arena::Arena (Arena &&other) noexcept :
        buffer_ (other.buffer_), capacity_ (other.capacity_), offset_ (other.offset_) {
        other.buffer_   = nullptr;
        other.capacity_ = 0;
        other.offset_   = 0;
    }

    void *Arena::allocate (std::size_t size, std::size_t alignment) {
        if (size == 0 || buffer_ == nullptr) return nullptr;

        assert (alignment > 0 && (alignment & (alignment - 1)) == 0 && "Alignment must be a power of 2");

        std::uintptr_t caddr        = reinterpret_cast<std::uintptr_t> (buffer_ + offset_);
        std::uintptr_t aligned_addr = (caddr + (alignment - 1)) & ~(alignment - 1);
        std::size_t newoffset       = (aligned_addr - reinterpret_cast<std::uintptr_t> (buffer_)) + size;
        if (newoffset > capacity_) return nullptr;
        offset_ = newoffset;
        return reinterpret_cast<void *> (aligned_addr);
    }


    Arena &Arena::operator= (Arena &&other) noexcept {
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

    void Arena::reset_to_marker (Arena::Marker marker) noexcept {
        assert (marker.offset <= offset_ && "Cannot rewind");
        offset_ = marker.offset;
    }

} // namespace core
