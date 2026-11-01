#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define EDU_LIKELY(x) __builtin_expect (!!(x), 1)
#define EDU_UNLIKELY(x) __builtin_expect (!!(x), 0)
#define EDU_FORCE_INLINE inline __attribute__ ((always_inline))


#ifndef NDEBUG
#define EDU_ASSERT(cond, msg)                                                                                          \
    do {                                                                                                               \
        if (EDU_UNLIKELY (!(cond))) {                                                                                  \
            std::fprintf (stderr, "[EDU_ASSERT FAILED] %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, (msg));           \
            std::abort ();                                                                                             \
        }                                                                                                              \
    } while (0)
#else
#define EDU_ASSERT(cond, msg) ((void) 0)
#endif

namespace core {

    using u8  = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    using i8  = std::int8_t;
    using i16 = std::int16_t;
    using i32 = std::int32_t;
    using i64 = std::int64_t;

    using f32 = float;
    using f64 = double;

    using usize = std::size_t;
    using isize = std::ptrdiff_t;

    using StudentId   = u32;
    using QuestionId  = u16;
    using TopicId     = u8;
    using Hash64      = u64;
    using DurationMs  = u32;
    using TimestampMs = u64;

    constexpr StudentId INVALID_STUDENT_ID   = 0xFFFFFFFFU;
    constexpr QuestionId INVALID_QUESTION_ID = 0xFFFFU;
    constexpr Hash64 ROOT_DAG_HASH           = 0ULL;
    constexpr u32 MAX_SCORE_BUCKET           = 10000U;


    // this works
    template<typename T>
    struct RemoveReference {
            using Type = T;
    };
    template<typename T>
    struct RemoveReference<T &> {
            using Type = T;
    };
    template<typename T>
    struct RemoveReference<T &&> {
            using Type = T;
    };

    template<typename T>
    EDU_FORCE_INLINE constexpr typename RemoveReference<T>::Type &&move (T &&arg) noexcept {
        return static_cast<typename RemoveReference<T>::Type &&> (arg);
    }

    template<typename T>
    EDU_FORCE_INLINE constexpr void swap (T &a, T &b) noexcept {
        T tmp = move (a);
        a     = move (b);
        b     = move (tmp);
    }

    template<typename T>
    EDU_FORCE_INLINE constexpr const T &min (const T &a, const T &b) noexcept {
        return (b < a) ? b : a;
    }

    template<typename T>
    EDU_FORCE_INLINE constexpr const T &max (const T &a, const T &b) noexcept {
        return (a < b) ? b : a;
    }

    template<typename T>
    EDU_FORCE_INLINE constexpr const T &clamp (const T &val, const T &lo, const T &hi) noexcept {
        return (val < lo) ? lo : ((hi < val) ? hi : val);
    }


    template<typename T>
    struct Span {
            T *data    = nullptr;
            usize size = 0;

            constexpr T &operator[] (usize idx) const noexcept {
                EDU_ASSERT (idx < size, "Span index out of bounds");
                return data[idx];
            }
    };

    using ByteSpan      = Span<u8>;
    using ConstByteSpan = Span<const u8>;

    enum class StatusCode : u8 {
        Ok                  = 0,
        OutOfMemory         = 1,
        KeyNotFound         = 2,
        CorruptedPayload    = 3,
        HashChainMismatch   = 4,
        DuplicateSubmission = 5,
        InvalidState        = 6
    };


    enum class ActionType : u8 { McqSelect = 0, TextDelta = 1, FocusLostEvent = 2, FinalSubmit = 3, HeartbeatSync = 4 };

    enum class DifficultyTier : u8 { Tier1 = 0, Tier2 = 1, Tier3 = 2, Tier4 = 3, Tier5 = 4, Count = 5 };

    struct DecodedSubmission {
            Hash64 node_hash;
            Hash64 parent_hash;
            StudentId student_id;
            QuestionId question_id;
            DurationMs monotonic_dt_ms;
            TimestampMs server_recv_ms;
            ActionType action;
            u8 mcq_option;
            const char *full_text_ptr;
            u32 full_text_len;
    };


    // Leaderboard key with 3-level tie-breaking:
    // 1. highest scaled score wins
    // 2. lower total time wins
    // 3. lower student id breaks exact ties (guarantees strict unique ordering in AVL tree)
    struct LeaderboardKey {
            i32 scaled_score;
            u32 total_time_ms;
            StudentId student_id;

            constexpr bool operator< (const LeaderboardKey &other) const noexcept {
                if (scaled_score != other.scaled_score) { return scaled_score > other.scaled_score; }
                if (total_time_ms != other.total_time_ms) { return total_time_ms < other.total_time_ms; }
                return student_id < other.student_id;
            }

            constexpr bool operator== (const LeaderboardKey &other) const noexcept {
                return scaled_score == other.scaled_score && total_time_ms == other.total_time_ms &&
                       student_id == other.student_id;
            }
    };


    EDU_FORCE_INLINE constexpr Hash64 fnv1a_hash_bytes (const void *data, usize len) noexcept {
        const auto *bytes = static_cast<const u8 *> (data);
        Hash64 hash       = 14695981039346656037ULL;
        for (usize i = 0; i < len; ++i) {
            hash ^= static_cast<Hash64> (bytes[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

} // namespace core
