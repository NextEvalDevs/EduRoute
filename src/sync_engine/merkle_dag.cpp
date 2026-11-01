#include "sync_engine/merkle_dag.hpp"

#include <cstring>
#include <limits>

namespace sync_engine {
    namespace {

        constexpr core::u8 FILE_MAGIC[4]     = {0x7f, 'C', 'A', 'T'};
        constexpr core::u32 FILE_VERSION     = 1;
        constexpr core::u32 MAX_ANSWER_BYTES = 16u * 1024u * 1024u;

        // Binary record layout (little-endian wire format):
        // magic[4], version[4], student[4], question[2], timestamp[8],
        // parent[8], node_hash[8], answer_len[4], checksum[8] = 46 bytes, then answer payload
        constexpr core::usize RECORD_HEADER_SIZE = 4 + 4 + 4 + 2 + 8 + 8 + 8 + 4 + 8;

        void put_u16 (core::u8 *out, core::u16 value) noexcept {
            out[0] = static_cast<core::u8> (value);
            out[1] = static_cast<core::u8> (value >> 8);
        }
        void put_u32 (core::u8 *out, core::u32 value) noexcept {
            for (core::usize i = 0; i < 4; ++i) out[i] = static_cast<core::u8> (value >> (8 * i));
        }
        void put_u64 (core::u8 *out, core::u64 value) noexcept {
            for (core::usize i = 0; i < 8; ++i) out[i] = static_cast<core::u8> (value >> (8 * i));
        }
        core::u16 get_u16 (const core::u8 *in) noexcept {
            return static_cast<core::u16> (in[0]) | static_cast<core::u16> (static_cast<core::u16> (in[1]) << 8);
        }
        core::u32 get_u32 (const core::u8 *in) noexcept {
            core::u32 value = 0;
            for (core::usize i = 0; i < 4; ++i) value |= static_cast<core::u32> (in[i]) << (8 * i);
            return value;
        }
        core::u64 get_u64 (const core::u8 *in) noexcept {
            core::u64 value = 0;
            for (core::usize i = 0; i < 8; ++i) value |= static_cast<core::u64> (in[i]) << (8 * i);
            return value;
        }

        // records the checksum
        core::Hash64 record_checksum (const core::u8 *header_without_checksum, const core::u8 *answer,
                                      core::usize answer_len) noexcept {
            core::u8 fixed[RECORD_HEADER_SIZE - 8]{};
            std::memcpy (fixed, header_without_checksum, sizeof (fixed));
            const core::Hash64 header_hash = core::fnv1a_hash_bytes (fixed, sizeof (fixed));
            const core::Hash64 answer_hash = core::fnv1a_hash_bytes (answer, answer_len);
            return header_hash ^ (answer_hash + 0x9e3779b97f4a7c15ULL + (header_hash << 6) + (header_hash >> 2));
        }

        bool read_exact (std::FILE *file, void *data, core::usize size) noexcept {
            return size == 0 || std::fread (data, 1, size, file) == size;
        }

    } // namespace

    MerkleDAG::MerkleDAG (core::Arena &arena) noexcept : arena_ (arena), nodes_ (arena, 16), file_ (nullptr) {}

    MerkleDAG::~MerkleDAG () { close (); }

    bool MerkleDAG::open (const char *filepath) {
        if (!filepath || filepath[0] == '\0') return false;
        close ();

        file_ = std::fopen (filepath, "a+b");
        if (!file_) return false;

        if (!read_all ()) {
            close ();
            return false;
        }
        return true;
    }

    void MerkleDAG::close () noexcept {
        if (file_) {
            std::fflush (file_);
            std::fclose (file_);
            file_ = nullptr;
        }
    }

    bool MerkleDAG::is_open () const noexcept { return file_ != nullptr; }


    // how many hash calculating routines do we have accross the codebase?
    core::Hash64 MerkleDAG::calculate_hash (core::StudentId student_id, core::QuestionId question_id,
                                            core::Hash64 parent_hash, core::TimestampMs timestamp_ms,
                                            core::StringView full_answer) noexcept {
        // Pack into explicit byte layout first so compiler padding doesn't poison the hash
        core::u8 fixed[4 + 2 + 8 + 8 + 4]{};
        put_u32 (fixed, student_id);
        put_u16 (fixed + 4, question_id);
        put_u64 (fixed + 6, parent_hash);
        put_u64 (fixed + 14, timestamp_ms);
        put_u32 (fixed + 22, static_cast<core::u32> (full_answer.size ()));

        const core::Hash64 metadata_hash = core::fnv1a_hash_bytes (fixed, sizeof (fixed));
        const core::Hash64 answer_hash   = core::fnv1a_hash_bytes (full_answer.data (), full_answer.size ());
        return metadata_hash ^ (answer_hash + 0x9e3779b97f4a7c15ULL + (metadata_hash << 6) + (metadata_hash >> 2));
    }

    core::Hash64 MerkleDAG::current_head (core::StudentId student_id, core::QuestionId question_id) const noexcept {
        for (core::usize i = nodes_.size (); i > 0; --i) {
            const MerkleNode &node = nodes_[i - 1];
            if (node.student_id == student_id && node.question_id == question_id) return node.node_hash;
        }
        return core::ROOT_DAG_HASH;
    }

    bool MerkleDAG::append_memory (core::StudentId student_id, core::QuestionId question_id,
                                   core::TimestampMs timestamp_ms, core::Hash64 parent_hash, core::Hash64 node_hash,
                                   core::StringView answer) noexcept {
        if (answer.size () > 0 && !answer.data ()) return false;

        char *owned = nullptr;
        if (answer.size () > 0) {
            owned = static_cast<char *> (arena_.allocate (answer.size (), alignof (char)));
            if (!owned) return false;
            std::memcpy (owned, answer.data (), answer.size ());
        }

        MerkleNode node{};
        node.student_id   = student_id;
        node.question_id  = question_id;
        node.parent_hash  = parent_hash;
        node.node_hash    = node_hash;
        node.timestamp_ms = timestamp_ms;
        node.answer       = core::StringView (owned, answer.size ());

        nodes_.push_back (node);
        return true;
    }

    bool MerkleDAG::append (core::StudentId student_id, core::QuestionId question_id, core::TimestampMs timestamp_ms,
                            core::StringView full_answer, core::Hash64 *out_hash) {
        if (full_answer.size () > MAX_ANSWER_BYTES || full_answer.size () > std::numeric_limits<core::u32>::max () ||
            (full_answer.size () > 0 && !full_answer.data ()))
            return false;

        const core::Hash64 parent = current_head (student_id, question_id);
        const core::Hash64 hash   = calculate_hash (student_id, question_id, parent, timestamp_ms, full_answer);

        if (file_) {
            core::u8 header[RECORD_HEADER_SIZE]{};
            std::memcpy (header, FILE_MAGIC, 4);
            put_u32 (header + 4, FILE_VERSION);
            put_u32 (header + 8, student_id);
            put_u16 (header + 12, question_id);
            put_u64 (header + 14, timestamp_ms);
            put_u64 (header + 22, parent);
            put_u64 (header + 30, hash);
            put_u32 (header + 38, static_cast<core::u32> (full_answer.size ()));

            const core::Hash64 checksum = record_checksum (
                    header, reinterpret_cast<const core::u8 *> (full_answer.data ()), full_answer.size ());
            put_u64 (header + 42, checksum);

            if (std::fseek (file_, 0, SEEK_END) != 0) return false;
            if (std::fwrite (header, 1, sizeof (header), file_) != sizeof (header)) return false;
            if (full_answer.size () > 0 &&
                std::fwrite (full_answer.data (), 1, full_answer.size (), file_) != full_answer.size ())
                return false;
            if (std::fflush (file_) != 0) return false;
        }

        if (!append_memory (student_id, question_id, timestamp_ms, parent, hash, full_answer)) {
            // failed to grab memory in arena
            return false;
        }

        if (out_hash) *out_hash = hash;
        return true;
    }


    // reads all
    bool MerkleDAG::read_all () noexcept {
        if (!file_) return false;
        nodes_.clear ();

        if (std::fseek (file_, 0, SEEK_SET) != 0) return false;

        for (;;) {
            core::u8 header[RECORD_HEADER_SIZE]{};
            const core::usize got = std::fread (header, 1, sizeof (header), file_);
            if (got == 0 && std::feof (file_)) break;

            // Torn / incomplete record at end of file - stop replay here and keep what was valid
            if (got != sizeof (header)) break;
            if (std::memcmp (header, FILE_MAGIC, 4) != 0 || get_u32 (header + 4) != FILE_VERSION) return false;

            const core::u32 student_id      = get_u32 (header + 8);
            const core::u16 question_id     = get_u16 (header + 12);
            const core::u64 timestamp       = get_u64 (header + 14);
            const core::u64 parent          = get_u64 (header + 22);
            const core::u64 stored_hash     = get_u64 (header + 30);
            const core::u32 answer_len      = get_u32 (header + 38);
            const core::u64 stored_checksum = get_u64 (header + 42);

            if (answer_len > MAX_ANSWER_BYTES) return false;

            char *answer = nullptr;
            if (answer_len > 0) {
                answer = static_cast<char *> (arena_.allocate (answer_len, alignof (char)));
                if (!answer) return false;
                if (!read_exact (file_, answer, answer_len)) break;
            }

            const core::StringView answer_view (answer, answer_len);
            const core::Hash64 expected_hash = calculate_hash (student_id, question_id, parent, timestamp, answer_view);
            const core::Hash64 expected_checksum =
                    record_checksum (header, reinterpret_cast<const core::u8 *> (answer), answer_len);

            if (expected_hash != stored_hash || expected_checksum != stored_checksum) return false;

            // Integrity check: verify hash chain links directly back to previous revision
            if (current_head (student_id, question_id) != parent) return false;

            MerkleNode node{};
            node.student_id   = student_id;
            node.question_id  = question_id;
            node.parent_hash  = parent;
            node.node_hash    = stored_hash;
            node.timestamp_ms = timestamp;
            node.answer       = answer_view;
            nodes_.push_back (node);
        }

        // Ready file cursor for new appends
        if (std::fseek (file_, 0, SEEK_END) != 0) return false;
        return true;
    }

    bool MerkleDAG::latest (core::StudentId student_id, core::QuestionId question_id, MerkleNode &out) const noexcept {
        for (core::usize i = nodes_.size (); i > 0; --i) {
            const MerkleNode &node = nodes_[i - 1];
            if (node.student_id == student_id && node.question_id == question_id) {
                out = node;
                return true;
            }
        }
        return false;
    }

    bool MerkleDAG::history (core::StudentId student_id, core::QuestionId question_id, core::usize newest_index,
                             MerkleNode &out) const noexcept {
        core::usize found = 0;
        for (core::usize i = nodes_.size (); i > 0; --i) {
            const MerkleNode &node = nodes_[i - 1];
            if (node.student_id == student_id && node.question_id == question_id) {
                if (found == newest_index) {
                    out = node;
                    return true;
                }
                ++found;
                if (found >= HISTORY_LIMIT) break;
            }
        }
        return false;
    }

    core::usize MerkleDAG::history_size (core::StudentId student_id, core::QuestionId question_id) const noexcept {
        core::usize count = 0;
        for (core::usize i = nodes_.size (); i > 0 && count < HISTORY_LIMIT; --i) {
            const MerkleNode &node = nodes_[i - 1];
            if (node.student_id == student_id && node.question_id == question_id) ++count;
        }
        return count;
    }

    core::usize MerkleDAG::node_count () const noexcept { return nodes_.size (); }

} // namespace sync_engine
