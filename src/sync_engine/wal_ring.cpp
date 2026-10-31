#include "sync_engine/wal_ring.hpp"

#include <cstring>
#include <limits>

namespace sync_engine {
namespace {

// --------------------------------------------------
// WAL FILE FORMAT
// --------------------------------------------------

constexpr core::u8 FILE_MAGIC[4] = {'E', 'R', 'W', 'L'};

constexpr core::u32 FILE_VERSION = 1;

constexpr core::u8 RECORD_DATA = 1;
constexpr core::u8 RECORD_ACK = 2;

// Header:
// magic[4] + version[4] + type[1] + payload_size[4] + checksum[8]
constexpr core::usize HEADER_SIZE = 21;

// DATA payload:
// sequence[8]
// student_id[4]
// timestamp[8]
// bit_length[8]
// original_size[8]
// delta_count[8]
// byte_count[8]
constexpr core::usize DATA_FIXED_SIZE = 52;

constexpr core::usize FREQUENCY_BYTES = 256 * 4;

constexpr core::u32 MAX_COMPRESSED_BYTES = 64u * 1024u * 1024u;


// --------------------------------------------------
// INTEGER SERIALIZATION
// Little-endian format
// --------------------------------------------------

void put_u32(core::u8* out, core::u32 value) noexcept {
    for (core::usize i = 0; i < 4; ++i) {
        out[i] = static_cast<core::u8>(value >> (i * 8));
    }
}

void put_u64(core::u8* out, core::u64 value) noexcept {
    for (core::usize i = 0; i < 8; ++i) {
        out[i] = static_cast<core::u8>(value >> (i * 8));
    }
}

core::u32 get_u32(const core::u8* in) noexcept {
    core::u32 value = 0;

    for (core::usize i = 0; i < 4; ++i) {
        value |= static_cast<core::u32>(in[i]) << (i * 8);
    }

    return value;
}

core::u64 get_u64(const core::u8* in) noexcept {
    core::u64 value = 0;

    for (core::usize i = 0; i < 8; ++i) {
        value |= static_cast<core::u64>(in[i]) << (i * 8);
    }

    return value;
}


// --------------------------------------------------
// CHECKSUM
// --------------------------------------------------

core::Hash64 checksum(
    const core::u8* header_prefix,
    const core::u8* payload,
    core::usize payload_size
) noexcept {

    const core::Hash64 h1 =
        core::fnv1a_hash_bytes(header_prefix, 13);

    const core::Hash64 h2 =
        core::fnv1a_hash_bytes(payload, payload_size);

    return h1 ^ (
        h2 +
        0x9e3779b97f4a7c15ULL +
        (h1 << 6) +
        (h1 >> 2)
    );
}


// --------------------------------------------------
// FILE HELPERS
// --------------------------------------------------

bool read_exact(
    std::FILE* file,
    void* destination,
    core::usize size
) noexcept {

    return size == 0 ||
           std::fread(destination, 1, size, file) == size;
}


bool write_record(
    std::FILE* file,
    core::u8 type,
    const core::u8* payload,
    core::u32 payload_size
) noexcept {

    core::u8 header[HEADER_SIZE]{};

    std::memcpy(header, FILE_MAGIC, 4);

    put_u32(header + 4, FILE_VERSION);

    header[8] = type;

    put_u32(header + 9, payload_size);

    put_u64(
        header + 13,
        checksum(header, payload, payload_size)
    );

    if (std::fseek(file, 0, SEEK_END) != 0) {
        return false;
    }

    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        return false;
    }

    if (payload_size > 0 &&
        std::fwrite(payload, 1, payload_size, file) != payload_size) {
        return false;
    }

    return std::fflush(file) == 0;
}


// --------------------------------------------------
// COMPRESSED DATA VALIDATION
// --------------------------------------------------

bool validate_compressed(
    const CompressedTextDelta& value
) noexcept {

    if (!value.valid) {
        return false;
    }

    if (value.frequencies.size() != 256) {
        return false;
    }

    if (value.bytes.size() > MAX_COMPRESSED_BYTES) {
        return false;
    }

    const core::usize expected_bytes =
        value.bit_length / 8 +
        (value.bit_length % 8 != 0 ? 1 : 0);

    if (value.bytes.size() != expected_bytes) {
        return false;
    }

    core::u64 total_frequency = 0;

    for (core::usize i = 0; i < 256; ++i) {
        total_frequency += value.frequencies[i];
    }

    return total_frequency == value.original_size;
}

} // namespace


// ==================================================
// CONSTRUCTOR / DESTRUCTOR
// ==================================================

WriteAheadLog::WriteAheadLog(core::Arena& arena) noexcept
    : arena_(arena),
      records_(arena, 16),
      file_(nullptr),
      acknowledged_sequence_(0),
      last_sequence_(0) {
}


WriteAheadLog::~WriteAheadLog() {
    close();
}


// ==================================================
// OPEN
// ==================================================

bool WriteAheadLog::open(const char* filepath) {

    if (!filepath || filepath[0] == '\0') {
        return false;
    }

    close();

    records_.clear();

    acknowledged_sequence_ = 0;
    last_sequence_ = 0;

    file_ = std::fopen(filepath, "a+b");

    if (!file_) {
        return false;
    }

    if (!read_all()) {
        close();
        return false;
    }

    return true;
}


// ==================================================
// CLOSE
// ==================================================

void WriteAheadLog::close() noexcept {

    if (file_) {
        std::fflush(file_);
        std::fclose(file_);
        file_ = nullptr;
    }
}


bool WriteAheadLog::is_open() const noexcept {
    return file_ != nullptr;
}


// ==================================================
// COPY COMPRESSED DATA
// ==================================================

bool WriteAheadLog::copy_compressed(
    const CompressedTextDelta& source,
    CompressedTextDelta& destination
) noexcept {

    if (!validate_compressed(source)) {
        return false;
    }

    destination.bytes.clear();
    destination.frequencies.clear();

    for (core::usize i = 0; i < source.bytes.size(); ++i) {
        destination.bytes.push_back(source.bytes[i]);
    }

    for (core::usize i = 0; i < 256; ++i) {
        destination.frequencies.push_back(source.frequencies[i]);
    }

    destination.bit_length = source.bit_length;
    destination.original_size = source.original_size;
    destination.delta_count = source.delta_count;
    destination.valid = true;

    return true;
}


// ==================================================
// APPEND
// ==================================================

bool WriteAheadLog::append(
    core::StudentId student_id,
    core::TimestampMs timestamp_ms,
    const CompressedTextDelta& compressed,
    core::u64* out_sequence
) {

    if (!file_ ||
        !validate_compressed(compressed) ||
        compressed.delta_count == 0) {

        return false;
    }

    if (last_sequence_ == std::numeric_limits<core::u64>::max()) {
        return false;
    }

    const core::u64 sequence = last_sequence_ + 1;

    // Calculate payload size.
    const core::usize payload_size =
        DATA_FIXED_SIZE +
        FREQUENCY_BYTES +
        compressed.bytes.size();

    if (payload_size > std::numeric_limits<core::u32>::max()) {
        return false;
    }

    // Build serialized payload.
    core::Array<core::u8> payload(arena_, payload_size);

    auto push_u32 = [&payload](core::u32 value) {
        payload.push_back(static_cast<core::u8>(value));
        payload.push_back(static_cast<core::u8>(value >> 8));
        payload.push_back(static_cast<core::u8>(value >> 16));
        payload.push_back(static_cast<core::u8>(value >> 24));
    };

    auto push_u64 = [&payload](core::u64 value) {
        for (core::usize i = 0; i < 8; ++i) {
            payload.push_back(
                static_cast<core::u8>(value >> (i * 8))
            );
        }
    };

    // Fixed metadata.
    push_u64(sequence);
    push_u32(student_id);
    push_u64(timestamp_ms);

    push_u64(static_cast<core::u64>(compressed.bit_length));
    push_u64(static_cast<core::u64>(compressed.original_size));
    push_u64(static_cast<core::u64>(compressed.delta_count));
    push_u64(static_cast<core::u64>(compressed.bytes.size()));

    // Huffman frequency table.
    for (core::usize i = 0; i < 256; ++i) {
        push_u32(compressed.frequencies[i]);
    }

    // Compressed bytes.
    for (core::usize i = 0; i < compressed.bytes.size(); ++i) {
        payload.push_back(compressed.bytes[i]);
    }

    if (payload.size() != payload_size) {
        return false;
    }

    // Allocate and construct the in-memory record.
    WalRecord* record = arena_.create<WalRecord>(arena_);

    if (!record) {
        return false;
    }

    record->sequence = sequence;
    record->student_id = student_id;
    record->timestamp_ms = timestamp_ms;

    if (!copy_compressed(compressed, record->compressed)) {
        return false;
    }

    // Persist before making the record visible in memory.
    if (!write_record(
            file_,
            RECORD_DATA,
            payload.data(),
            static_cast<core::u32>(payload.size()))) {

        return false;
    }

    records_.push_back(record);

    last_sequence_ = sequence;

    if (out_sequence) {
        *out_sequence = sequence;
    }

    return true;
}


// ==================================================
// PEEK NEXT
// ==================================================

bool WriteAheadLog::peek_next(
    const WalRecord*& out
) const noexcept {

    out = nullptr;

    for (core::usize i = 0; i < records_.size(); ++i) {

        if (records_[i]->sequence > acknowledged_sequence_) {
            out = records_[i];
            return true;
        }
    }

    return false;
}


// ==================================================
// ACKNOWLEDGE
// ==================================================

bool WriteAheadLog::acknowledge(core::u64 sequence) {

    if (!file_ ||
        sequence < acknowledged_sequence_ ||
        sequence > last_sequence_) {

        return false;
    }

    if (sequence == acknowledged_sequence_) {
        return true;
    }

    core::u8 payload[8];

    put_u64(payload, sequence);

    if (!write_record(
            file_,
            RECORD_ACK,
            payload,
            sizeof(payload))) {

        return false;
    }

    acknowledged_sequence_ = sequence;

    return true;
}


// ==================================================
// PENDING COUNT
// ==================================================

core::usize WriteAheadLog::pending_count() const noexcept {

    core::usize count = 0;

    for (core::usize i = 0; i < records_.size(); ++i) {

        if (records_[i]->sequence > acknowledged_sequence_) {
            ++count;
        }
    }

    return count;
}


// ==================================================
// SEQUENCE GETTERS
// ==================================================

core::u64 WriteAheadLog::acknowledged_sequence() const noexcept {
    return acknowledged_sequence_;
}


core::u64 WriteAheadLog::last_sequence() const noexcept {
    return last_sequence_;
}


// ==================================================
// RECOVERY
// ==================================================

bool WriteAheadLog::read_all() noexcept {

    if (!file_) {
        return false;
    }

    if (std::fseek(file_, 0, SEEK_SET) != 0) {
        return false;
    }

    for (;;) {

        core::u8 header[HEADER_SIZE]{};

        const core::usize got =
            std::fread(header, 1, sizeof(header), file_);

        // Normal end of file.
        if (got == 0 && std::feof(file_)) {
            break;
        }

        // Reject incomplete headers.
        if (got != sizeof(header)) {
            return false;
        }

        // Validate header.
        if (std::memcmp(header, FILE_MAGIC, 4) != 0 ||
            get_u32(header + 4) != FILE_VERSION) {

            return false;
        }

        const core::u8 type = header[8];

        const core::u32 payload_size = get_u32(header + 9);

        if (payload_size >
            MAX_COMPRESSED_BYTES +
            DATA_FIXED_SIZE +
            FREQUENCY_BYTES) {

            return false;
        }

        // Read payload.
        core::Array<core::u8> payload(arena_, payload_size);

        for (core::u32 i = 0; i < payload_size; ++i) {

            core::u8 byte = 0;

            if (!read_exact(file_, &byte, 1)) {
                return false;
            }

            payload.push_back(byte);
        }

        // Verify checksum.
        if (checksum(header, payload.data(), payload.size()) !=
            get_u64(header + 13)) {

            return false;
        }


        // ------------------------------------------
        // ACK RECORD
        // ------------------------------------------

        if (type == RECORD_ACK) {

            if (payload.size() != 8) {
                return false;
            }

            const core::u64 ack =
                get_u64(payload.data());

            if (ack < acknowledged_sequence_ ||
                ack > last_sequence_) {

                return false;
            }

            acknowledged_sequence_ = ack;

            continue;
        }


        // ------------------------------------------
        // DATA RECORD
        // ------------------------------------------

        if (type != RECORD_DATA ||
            payload.size() < DATA_FIXED_SIZE + FREQUENCY_BYTES) {

            return false;
        }

        const core::u8* p = payload.data();

        core::usize pos = 0;

        const core::u64 sequence =
            get_u64(p + pos);
        pos += 8;

        const core::StudentId student_id =
            get_u32(p + pos);
        pos += 4;

        const core::TimestampMs timestamp =
            get_u64(p + pos);
        pos += 8;

        const core::u64 bit_length =
            get_u64(p + pos);
        pos += 8;

        const core::u64 original_size =
            get_u64(p + pos);
        pos += 8;

        const core::u64 delta_count =
            get_u64(p + pos);
        pos += 8;

        const core::u64 byte_count =
            get_u64(p + pos);
        pos += 8;


        // Validate metadata and lengths.
        if (last_sequence_ == std::numeric_limits<core::u64>::max() ||
            sequence != last_sequence_ + 1 ||
            bit_length > std::numeric_limits<core::usize>::max() ||
            original_size > std::numeric_limits<core::usize>::max() ||
            delta_count > std::numeric_limits<core::usize>::max() ||
            byte_count > MAX_COMPRESSED_BYTES ||
            byte_count != payload.size() - pos - FREQUENCY_BYTES) {

            return false;
        }


        // Construct the record directly inside the Arena.
        WalRecord* record = arena_.create<WalRecord>(arena_);

        if (!record) {
            return false;
        }

        record->sequence = sequence;
        record->student_id = student_id;
        record->timestamp_ms = timestamp;


        // Read Huffman frequency table.
        core::u64 frequency_sum = 0;

        for (core::usize i = 0; i < 256; ++i) {

            const core::u32 frequency =
                get_u32(p + pos);

            pos += 4;

            record->compressed.frequencies.push_back(frequency);

            frequency_sum += frequency;
        }

        if (frequency_sum != original_size) {
            return false;
        }


        // Read compressed bytes.
        for (core::usize i = 0;
             i < static_cast<core::usize>(byte_count);
             ++i) {

            record->compressed.bytes.push_back(p[pos + i]);
        }


        // Set compressed metadata.
        record->compressed.bit_length =
            static_cast<core::usize>(bit_length);

        record->compressed.original_size =
            static_cast<core::usize>(original_size);

        record->compressed.delta_count =
            static_cast<core::usize>(delta_count);

        record->compressed.valid = true;


        if (!validate_compressed(record->compressed) ||
            record->compressed.delta_count == 0) {

            return false;
        }

        records_.push_back(record);

        last_sequence_ = sequence;
    }

    // Position file at the end for future appends.
    return std::fseek(file_, 0, SEEK_END) == 0;
}

} // namespace sync_engine