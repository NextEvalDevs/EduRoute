#pragma once

#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/string_view.hpp"
#include "core/types.hpp"

#include <cstdio>

namespace sync_engine {

    // A full answer snapshot in the per-(student, question) Merkle history.
    // answer.data is owned by the Arena supplied to MerkleDAG.
    struct MerkleNode {
            core::StudentId student_id;
            core::QuestionId question_id;
            core::Hash64 parent_hash;
            core::Hash64 node_hash;
            core::TimestampMs timestamp_ms;
            core::StringView answer;
    };

    class MerkleDAG {
        public:
            static constexpr core::usize HISTORY_LIMIT = 5;

            explicit MerkleDAG (core::Arena &arena) noexcept;
            ~MerkleDAG ();

            MerkleDAG (const MerkleDAG &)            = delete;
            MerkleDAG &operator= (const MerkleDAG &) = delete;

            // Opens/creates the append-only snapshot file and replays valid records.
            // Returns false if the file cannot be opened or contains an invalid header.
            bool open (const char *filepath);

            // Closes the backing file. In-memory nodes remain valid until the Arena is reset.
            void close () noexcept;

            bool is_open () const noexcept;

            // Adds a complete answer snapshot. The previous snapshot for this exact
            // student/question pair becomes parent. The newest five remain queryable.
            // A successful return means the record was written and flushed to disk.
            bool append (core::StudentId student_id, core::QuestionId question_id, core::TimestampMs timestamp_ms,
                         core::StringView full_answer, core::Hash64 *out_hash = nullptr);

            // Returns the current/latest snapshot for this student/question.
            bool latest (core::StudentId student_id, core::QuestionId question_id, MerkleNode &out) const noexcept;

            // Returns the Nth newest retained snapshot: index 0 is latest.
            bool history (core::StudentId student_id, core::QuestionId question_id, core::usize newest_index,
                          MerkleNode &out) const noexcept;

            // Number of retained snapshots for this pair (maximum HISTORY_LIMIT).
            core::usize history_size (core::StudentId student_id, core::QuestionId question_id) const noexcept;

            core::usize node_count () const noexcept;

            // Deterministic hash over explicitly serialized fields and answer bytes.
            static core::Hash64 calculate_hash (core::StudentId student_id, core::QuestionId question_id,
                                                core::Hash64 parent_hash, core::TimestampMs timestamp_ms,
                                                core::StringView full_answer) noexcept;

        private:
            core::Arena &arena_;
            core::Array<MerkleNode> nodes_;
            std::FILE *file_;

            bool append_memory (core::StudentId student_id, core::QuestionId question_id,
                                core::TimestampMs timestamp_ms, core::Hash64 parent_hash, core::Hash64 node_hash,
                                core::StringView answer) noexcept;

            core::Hash64 current_head (core::StudentId student_id, core::QuestionId question_id) const noexcept;

            bool read_all () noexcept;
    };

} // namespace sync_engine
