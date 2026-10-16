#pragma once


#include <cassert>
#include "core/dyn_array.hpp"
#include "core/types.hpp"

namespace core {
    class BitWriter {
        private:
            Array<u8> buf_;
            u64 bit_accum_{0};
            u32 bit_count_{0};


        public:
            explicit BitWriter (Arena &arena, usize intial_bytes = 256) : buf_ (arena, intial_bytes) {}


            void write_bits (u64 value, u32 numbits) {
                assert (numbits <= 56);

                const u64 mask = (numbits == 64) ? ~0ull : ((1ull << numbits) - 1ull);
                bit_accum_ |= (value & mask) << bit_count_;
                bit_count_ += numbits;
                while (bit_count_ >= 8) {
                    buf_.push_back (static_cast<u8> (bit_accum_ & 0xffu));
                    bit_accum_ >>= 8;
                    bit_count_ -= 8;
                }
            }

            void write_bit (bool bit) { write_bits (bit ? 1ull : 0ull, 1); }
            void flush () {
                if (bit_count_ > 0) {
                    buf_.push_back (static_cast<u8> (bit_accum_ & 0xffu));
                    bit_accum_ = 0;
                    bit_count_ = 0;
                }
            }

            [[nodiscard]] const u8 *data () const noexcept { return buf_.data (); }
            [[nodiscard]] usize byte_size () const noexcept { return buf_.size (); }
    };

    class BitReader {
        private:
            const u8 *data_{nullptr};
            usize byte_size_{0};
            usize byte_pos_{0};
            u64 bit_accum_{0};
            u32 bit_count_{0};

            void refill () noexcept {
                while (bit_count_ <= 56 && byte_pos_ < byte_size_) {
                    bit_accum_ |= (static_cast<u64> (data_[byte_pos_]) << bit_count_);
                    bit_count_ += 8;
                }
            }

        public:
            constexpr BitReader (const u8 *data, usize byte_size) : data_ (data), byte_size_ (byte_size) {}

            [[nodiscard]] u64 read_bits (u32 numbits) noexcept {
                assert (numbits <= 56);
                refill ();
                assert (bit_count_ >= numbits);

                const u64 mask = (numbits == 64) ? ~0ull : ((1ull << numbits) - 1ull);
                const u64 val  = bit_accum_ & mask;
                bit_accum_ >>= numbits;
                bit_count_ -= numbits;
                return val;
            }
            [[nodiscard]] bool read_bit () noexcept { return read_bits (1) != 0; }
            [[nodiscard]] bool has_bits (u32 numbits) const noexcept {
                return (bit_count_ + (byte_size_ - byte_pos_) * 8) >= numbits;
            }
    };
} // namespace core
