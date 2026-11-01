#pragma once

#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/string_view.hpp"
#include "core/types.hpp"

#include <cstdio>

namespace sync_engine {

    // Snapshot node in the per-(student, question) revision history
    // answer string data is arena-backed
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

            // Open disk backing file and recover past revisions
            bool open (const char *filepath);

            void close () noexcept;
            bool is_open () const noexcept;

            // Append revision node linked to previous head, commits to disk
            bool append (core::StudentId student_id, core::QuestionId question_id, core::TimestampMs timestamp_ms,
                         core::StringView full_answer, core::Hash64 *out_hash = nullptr);

            // Get most recent answer for this question
            bool latest (core::StudentId student_id, core::QuestionId question_id, MerkleNode &out) const noexcept;

            // Get previous revision (0 = newest/latest)
            bool history (core::StudentId student_id, core::QuestionId question_id, core::usize newest_index,
                          MerkleNode &out) const noexcept;

            core::usize history_size (core::StudentId student_id, core::QuestionId question_id) const noexcept;
            core::usize node_count () const noexcept;

            // Deterministic hash: parent_hash + metadata + answer content
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
