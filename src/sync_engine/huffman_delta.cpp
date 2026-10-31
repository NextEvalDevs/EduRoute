#include "sync_engine/huffman_delta.hpp"

#include <limits>
#include <utility>
#include "core/hash_map.hpp"

namespace sync_engine {

    namespace {

        using FrequencyMap = core::HashMap<core::u8, core::u32>;

        struct HuffmanNode {
                core::u8 byte;
                core::u32 frequency;
                core::u32 order;
                HuffmanNode *left;
                HuffmanNode *right;

                bool is_leaf () const { return left == nullptr && right == nullptr; }
        };

        struct HuffmanCode {
                core::u64 bits;
                core::u32 length;
        };

        using CodeMap = core::HashMap<core::u8, HuffmanCode>;

        struct EncodedBits {
                core::Array<core::u8> bytes;
                core::usize bit_length;
                bool valid;
        };

        void write_u32 (core::Array<core::u8> &out, core::u32 value) {
            out.push_back (static_cast<core::u8> (value >> 24));
            out.push_back (static_cast<core::u8> (value >> 16));
            out.push_back (static_cast<core::u8> (value >> 8));
            out.push_back (static_cast<core::u8> (value));
        }

        bool read_u32 (const core::Array<core::u8> &in, core::usize &pos, core::u32 &value) {
            if (pos > in.size () || in.size () - pos < 4) return false;

            value = 0;
            for (int i = 0; i < 4; ++i) value = (value << 8) | in[pos++];

            return true;
        }

        class MinHeap {
                HuffmanNode *nodes_[256];
                core::usize count_;

                static bool less (const HuffmanNode *a, const HuffmanNode *b) {
                    if (a->frequency != b->frequency) return a->frequency < b->frequency;
                    return a->order < b->order;
                }

                void sift_up (core::usize i) {
                    while (i > 0) {
                        core::usize p = (i - 1) / 2;
                        if (!less (nodes_[i], nodes_[p])) break;
                        HuffmanNode *t = nodes_[i];
                        nodes_[i]      = nodes_[p];
                        nodes_[p]      = t;
                        i              = p;
                    }
                }

                void sift_down (core::usize i) {
                    while (true) {
                        core::usize l        = i * 2 + 1;
                        core::usize r        = l + 1;
                        core::usize smallest = i;

                        if (l < count_ && less (nodes_[l], nodes_[smallest])) smallest = l;
                        if (r < count_ && less (nodes_[r], nodes_[smallest])) smallest = r;
                        if (smallest == i) break;

                        HuffmanNode *t   = nodes_[i];
                        nodes_[i]        = nodes_[smallest];
                        nodes_[smallest] = t;
                        i                = smallest;
                    }
                }

            public:
                MinHeap () : count_ (0) {}

                core::usize size () const { return count_; }

                bool push (HuffmanNode *node) {
                    if (count_ >= 256) return false;
                    nodes_[count_] = node;
                    sift_up (count_);
                    ++count_;
                    return true;
                }

                HuffmanNode *pop () {
                    if (count_ == 0) return nullptr;
                    HuffmanNode *result = nodes_[0];
                    --count_;
                    if (count_ != 0) {
                        nodes_[0] = nodes_[count_];
                        sift_down (0);
                    }
                    return result;
                }
        };

        void count_frequencies (FrequencyMap &frequencies, const core::u8 *data, core::usize length) {
            for (core::usize i = 0; i < length; ++i) {
                core::u32 &f = frequencies[data[i]];
                if (f != std::numeric_limits<core::u32>::max ()) ++f;
            }
        }

        HuffmanNode *build_tree (core::Arena &arena, const FrequencyMap &frequencies) {
            MinHeap heap;

            for (core::u32 b = 0; b < 256; ++b) {
                const core::u32 *frequency = frequencies.find (static_cast<core::u8> (b));
                if (!frequency || *frequency == 0) continue;

                HuffmanNode *node = arena.create<HuffmanNode> ();
                if (!node) return nullptr;

                node->byte      = static_cast<core::u8> (b);
                node->frequency = *frequency;
                node->order     = b;
                node->left      = nullptr;
                node->right     = nullptr;

                if (!heap.push (node)) return nullptr;
            }

            if (heap.size () == 0) return nullptr;

            core::u32 order = 256;

            while (heap.size () > 1) {
                HuffmanNode *left  = heap.pop ();
                HuffmanNode *right = heap.pop ();

                if (!left || !right || left->frequency > std::numeric_limits<core::u32>::max () - right->frequency)
                    return nullptr;

                HuffmanNode *parent = arena.create<HuffmanNode> ();
                if (!parent) return nullptr;

                parent->byte      = 0;
                parent->frequency = left->frequency + right->frequency;
                parent->order     = order++;
                parent->left      = left;
                parent->right     = right;

                if (!heap.push (parent)) return nullptr;
            }

            return heap.pop ();
        }

        // This implementation supports Huffman codes up to 64 bits.
        void make_codes (const HuffmanNode *node, core::u64 bits, core::u32 length, CodeMap &codes, bool &valid) {
            if (!node || !valid) return;

            if (node->is_leaf ()) {
                // A one-symbol input is represented by the single bit 0.
                codes[node->byte] = {bits, length == 0 ? 1U : length};
                return;
            }

            if (length >= 63) {
                valid = false;
                return;
            }

            make_codes (node->left, bits << 1, length + 1, codes, valid);
            make_codes (node->right, (bits << 1) | 1ULL, length + 1, codes, valid);
        }

        EncodedBits encode (core::Arena &arena, const core::u8 *input, core::usize length, const CodeMap &codes) {
            EncodedBits result{core::Array<core::u8> (arena), 0, false};

            if (length == 0) {
                result.valid = true;
                return result;
            }
            if (!input) return result;

            for (core::usize i = 0; i < length; ++i) {
                const HuffmanCode *code = codes.find (input[i]);
                if (!code || code->length == 0 || code->length > 64) return result;

                for (core::u32 j = 0; j < code->length; ++j) {
                    core::u32 shift = code->length - 1 - j;
                    bool bit        = ((code->bits >> shift) & 1ULL) != 0;

                    if (result.bit_length % 8 == 0) result.bytes.push_back (0);

                    if (bit) {
                        core::usize index      = result.bit_length / 8;
                        core::u32 bit_position = static_cast<core::u32> (7 - result.bit_length % 8);
                        result.bytes[index] |= static_cast<core::u8> (1U << bit_position);
                    }
                    ++result.bit_length;
                }
            }

            result.valid = true;
            return result;
        }

        core::Array<core::u8> decode (core::Arena &arena, const EncodedBits &encoded, const HuffmanNode *root,
                                      core::usize original_size) {
            core::Array<core::u8> output (arena);
            if (!encoded.valid) return output;
            if (original_size == 0) return output;
            if (!root) return output;

            core::usize expected_bytes = encoded.bit_length / 8 + (encoded.bit_length % 8 == 0 ? 0 : 1);

            if (expected_bytes != encoded.bytes.size ()) return output;

            if (root->is_leaf ()) {
                if (encoded.bit_length != original_size) return output;
                for (core::usize i = 0; i < original_size; ++i) {
                    bool bit = ((encoded.bytes[i / 8] >> (7 - i % 8)) & 1U) != 0;
                    if (bit) return core::Array<core::u8> (arena);
                    output.push_back (root->byte);
                }
                return output;
            }

            const HuffmanNode *current = root;

            for (core::usize i = 0; i < encoded.bit_length; ++i) {
                bool bit = ((encoded.bytes[i / 8] >> (7 - i % 8)) & 1U) != 0;
                current  = bit ? current->right : current->left;
                if (!current) return core::Array<core::u8> (arena);

                if (current->is_leaf ()) {
                    output.push_back (current->byte);
                    current = root;

                    if (output.size () > original_size) return core::Array<core::u8> (arena);
                }
            }

            if (output.size () != original_size || current != root) return core::Array<core::u8> (arena);

            return output;
        }

        bool same_text (core::StringView a, core::StringView b) {
            if (a.size () != b.size ()) return false;
            for (core::usize i = 0; i < a.size (); ++i)
                if (a[i] != b[i]) return false;
            return true;
        }

    } // namespace

    void generate_text_delta (core::u32 answer_id, core::StringView old_value, core::StringView new_value,
                              TextDeltaArray &deltas) {
        if (!same_text (old_value, new_value)) deltas.push_back ({answer_id, old_value, new_value});
    }

    bool serialize_text_delta (const TextDeltaArray &deltas, core::Array<core::u8> &output) {
        // Check lengths before writing anything.
        for (core::usize i = 0; i < deltas.size (); ++i) {
            if (deltas[i].old_value.size () > std::numeric_limits<core::u32>::max () ||
                deltas[i].new_value.size () > std::numeric_limits<core::u32>::max ())
                return false;
        }

        for (core::usize i = 0; i < deltas.size (); ++i) {
            const TextDelta &d = deltas[i];

            write_u32 (output, d.answer_id);
            write_u32 (output, static_cast<core::u32> (d.old_value.size ()));

            for (core::usize j = 0; j < d.old_value.size (); ++j)
                output.push_back (static_cast<core::u8> (d.old_value[j]));

            write_u32 (output, static_cast<core::u32> (d.new_value.size ()));

            for (core::usize j = 0; j < d.new_value.size (); ++j)
                output.push_back (static_cast<core::u8> (d.new_value[j]));
        }

        return true;
    }

    CompressedTextDelta compress_text_delta (core::Arena &arena, const TextDeltaArray &deltas) {
        CompressedTextDelta result{
                core::Array<core::u8> (arena), core::Array<core::u32> (arena), 0, 0, deltas.size (), false};
        result.bit_length    = 0;
        result.original_size = 0;
        result.delta_count   = deltas.size ();
        result.valid         = false;

        core::Array<core::u8> raw (arena);
        if (!serialize_text_delta (deltas, raw)) return result;

        result.original_size = raw.size ();

        FrequencyMap frequencies (arena, 256);
        if (raw.size () != 0) count_frequencies (frequencies, raw.data (), raw.size ());

        for (core::u32 b = 0; b < 256; ++b) {
            const core::u32 *f = frequencies.find (static_cast<core::u8> (b));
            result.frequencies.push_back (f ? *f : 0);
        }

        if (raw.size () == 0) {
            result.valid = true;
            return result;
        }

        HuffmanNode *root = build_tree (arena, frequencies);
        if (!root) return result;

        CodeMap codes (arena, 256);
        bool codes_valid = true;
        make_codes (root, 0, 0, codes, codes_valid);
        if (!codes_valid) return result;

        EncodedBits encoded = encode (arena, raw.data (), raw.size (), codes);
        if (!encoded.valid) return result;

        result.bytes      = std::move (encoded.bytes);
        result.bit_length = encoded.bit_length;
        result.valid      = true;
        return result;
    }

    TextDeltaArray decompress_text_delta (core::Arena &arena, const CompressedTextDelta &compressed,
                                          core::Array<core::u8> &text_storage) {
        TextDeltaArray result (arena);

        if (!compressed.valid || compressed.frequencies.size () != 256) return result;

        if (compressed.delta_count == 0) {
            if (compressed.original_size == 0 && compressed.bit_length == 0) return result;
            return result;
        }

        FrequencyMap frequencies (arena, 256);
        core::u64 total_frequency = 0;

        for (core::u32 b = 0; b < 256; ++b) {
            core::u32 f = compressed.frequencies[b];
            if (f != 0) {
                frequencies[static_cast<core::u8> (b)] = f;
                total_frequency += f;
            }
        }

        if (total_frequency != compressed.original_size) return result;

        HuffmanNode *root = build_tree (arena, frequencies);
        if (!root) return result;

        EncodedBits encoded{core::Array<core::u8> (arena), compressed.bit_length, true};

        for (core::usize i = 0; i < compressed.bytes.size (); ++i) { encoded.bytes.push_back (compressed.bytes[i]); }

        core::Array<core::u8> raw = decode (arena, encoded, root, compressed.original_size);

        if (raw.size () != compressed.original_size) return result;

        struct ParsedDelta {
                core::u32 answer_id;
                core::usize old_offset;
                core::usize old_length;
                core::usize new_offset;
                core::usize new_length;
        };

        core::Array<ParsedDelta> parsed (arena);
        core::usize pos = 0;

        // Parse and validate the entire buffer first.
        for (core::usize i = 0; i < compressed.delta_count; ++i) {
            core::u32 id = 0, old_len = 0, new_len = 0;

            if (!read_u32 (raw, pos, id) || !read_u32 (raw, pos, old_len)) return TextDeltaArray (arena);

            if (pos > raw.size () || raw.size () - pos < old_len) return TextDeltaArray (arena);

            core::usize old_offset = pos;
            pos += old_len;

            if (!read_u32 (raw, pos, new_len)) return TextDeltaArray (arena);

            if (pos > raw.size () || raw.size () - pos < new_len) return TextDeltaArray (arena);

            core::usize new_offset = pos;
            pos += new_len;

            parsed.push_back ({id, old_offset, old_len, new_offset, new_len});
        }

        if (pos != raw.size ()) return TextDeltaArray (arena);

        // Copy strings into caller-owned storage so returned views remain valid.
        core::usize start = text_storage.size ();

        for (core::usize i = 0; i < parsed.size (); ++i) {
            const ParsedDelta &p = parsed[i];

            for (core::usize j = 0; j < p.old_length; ++j) text_storage.push_back (raw[p.old_offset + j]);

            for (core::usize j = 0; j < p.new_length; ++j) text_storage.push_back (raw[p.new_offset + j]);
        }

        const char *base   = reinterpret_cast<const char *> (text_storage.data ());
        core::usize cursor = start;

        for (core::usize i = 0; i < parsed.size (); ++i) {
            const ParsedDelta &p = parsed[i];

            core::usize old_offset = cursor;
            cursor += p.old_length;

            core::usize new_offset = cursor;
            cursor += p.new_length;

            core::StringView old_view (base + old_offset, p.old_length);
            core::StringView new_view (base + new_offset, p.new_length);

            result.push_back ({p.answer_id, old_view, new_view});
        }

        return result;
    }

    bool apply_text_delta (core::StringView current_value, const TextDelta &delta, core::Array<core::u8> &output) {
        if (!same_text (current_value, delta.old_value)) return false;

        for (core::usize i = 0; i < delta.new_value.size (); ++i)
            output.push_back (static_cast<core::u8> (delta.new_value[i]));

        return true;
    }

} // namespace sync_engine
