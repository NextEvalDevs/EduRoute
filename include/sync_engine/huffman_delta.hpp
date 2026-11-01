#pragma once

#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/string_view.hpp"
#include "core/types.hpp"

namespace sync_engine {

    // Represents an edit delta between old and new answer states (MCQs use "A".."D")
    struct TextDelta {
            core::u32 answer_id;
            core::StringView old_value;
            core::StringView new_value;
    };

    using TextDeltaArray = core::Array<TextDelta>;

    // Compressed delta packet: packed bitstream + 256 frequency weights for rebuilding tree
    struct CompressedTextDelta {
            core::Array<core::u8> bytes;
            core::Array<core::u32> frequencies; // 256-entry histogram
            core::usize bit_length;
            core::usize original_size;
            core::usize delta_count;
            bool valid;
    };

    // Compares old vs new text and records delta if anything changed
    void generate_text_delta (core::u32 answer_id, core::StringView old_value, core::StringView new_value,
                              TextDeltaArray &deltas);

    // Packs delta array into wire byte buffer
    bool serialize_text_delta (const TextDeltaArray &deltas, core::Array<core::u8> &output);

    // Huffman-encodes serialized deltas
    CompressedTextDelta compress_text_delta (core::Arena &arena, const TextDeltaArray &deltas);

    // Reconstructs deltas from compressed bitstream (returned string views point into text_storage)
    TextDeltaArray decompress_text_delta (core::Arena &arena, const CompressedTextDelta &compressed,
                                          core::Array<core::u8> &text_storage);

    // Patches current_value using delta (verifies base text matches old_value first)
    bool apply_text_delta (core::StringView current_value, const TextDelta &delta, core::Array<core::u8> &output);

} // namespace sync_engine
