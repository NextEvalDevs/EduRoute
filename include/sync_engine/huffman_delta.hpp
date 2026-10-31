#pragma once

#include "core/arena_alloc.hpp"
#include "core/types.hpp"
#include "core/dyn_array.hpp"
#include "core/string_view.hpp"

namespace sync_engine {

// A single answer change. Works for both MCQ and descriptive answers.
// MCQ values can simply be "A", "B", "C", or "D".
struct TextDelta {
    core::u32 answer_id;
    core::StringView old_value;
    core::StringView new_value;
};

using TextDeltaArray = core::Array<TextDelta>;

// Huffman-compressed serialized delta.
struct CompressedTextDelta {
    core::Array<core::u8> bytes;
    core::Array<core::u32> frequencies; // Always 256 entries.
    core::usize bit_length;
    core::usize original_size;
    core::usize delta_count;
    bool valid;
};

// Add a delta only when the answer actually changed.
void generate_text_delta(
    core::u32 answer_id,
    core::StringView old_value,
    core::StringView new_value,
    TextDeltaArray& deltas
);

// Serialize answer_id, old text, and new text.
bool serialize_text_delta(
    const TextDeltaArray& deltas,
    core::Array<core::u8>& output
);

// Serialize and Huffman-compress all deltas.
CompressedTextDelta compress_text_delta(
    core::Arena& arena,
    const TextDeltaArray& deltas
);

// Huffman-decompress and deserialize.
// Returned StringViews point into text_storage, which must remain alive.
TextDeltaArray decompress_text_delta(
    core::Arena& arena,
    const CompressedTextDelta& compressed,
    core::Array<core::u8>& text_storage
);

// Apply one delta if current_value matches the delta's old_value.
// The resulting answer is appended to output.
bool apply_text_delta(
    core::StringView current_value,
    const TextDelta& delta,
    core::Array<core::u8>& output
);

} // namespace sync_engine
