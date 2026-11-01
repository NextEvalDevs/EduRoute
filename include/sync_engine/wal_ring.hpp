#pragma once

#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/types.hpp"
#include "sync_engine/huffman_delta.hpp"

#include <cstdio>

namespace sync_engine {

    // In-memory representation of a persisted WAL entry
    struct WalRecord {
            core::u64 sequence;
            core::StudentId student_id;
            core::TimestampMs timestamp_ms;
            CompressedTextDelta compressed;

            explicit WalRecord (core::Arena &arena) :
                sequence (0), student_id (0), timestamp_ms (0),
                compressed{core::Array<core::u8> (arena), core::Array<core::u32> (arena, 256), 0, 0, 0, false} {}
    };

    class WriteAheadLog {
        public:
            explicit WriteAheadLog (core::Arena &arena) noexcept;
            ~WriteAheadLog ();

            WriteAheadLog (const WriteAheadLog &)            = delete;
            WriteAheadLog &operator= (const WriteAheadLog &) = delete;

            // Open WAL file on disk and recover un-ACKed state
            bool open (const char *filepath);

            void close () noexcept;
            bool is_open () const noexcept;

            // Append new compressed batch to disk WAL
            bool append (core::StudentId student_id, core::TimestampMs timestamp_ms,
                         const CompressedTextDelta &compressed, core::u64 *out_sequence = nullptr);

            // Check next un-synced record (oldest sequence > acknowledged_sequence)
            bool peek_next (const WalRecord *&out) const noexcept;

            // Mark sequence as synced by the server
            bool acknowledge (core::u64 sequence);

            core::usize pending_count () const noexcept;
            core::u64 acknowledged_sequence () const noexcept;
            core::u64 last_sequence () const noexcept;

        private:
            core::Arena &arena_;
            core::Array<WalRecord *> records_;

            std::FILE *file_;

            core::u64 acknowledged_sequence_;
            core::u64 last_sequence_;

            bool read_all () noexcept;

            bool copy_compressed (const CompressedTextDelta &source, CompressedTextDelta &destination) noexcept;
    };

} // namespace sync_engine
