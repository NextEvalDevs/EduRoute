#pragma once


#include "core/arena_alloc.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"


namespace core::anticheat {


    struct TextSimMatch {
            StudentId other_student;
            u32 shared_fing;
            f32 cont_score;
    };


    class KmerWinnow {
        private:
            static constexpr usize KMER_LEN = 5;
            static constexpr usize WINDOW   = 4;
            static constexpr u64 RK_BASE    = 257ull;
            static constexpr u64 RK_MOD     = 1000000007ull;

            struct PostingNode {
                    StudentId student_id;
                    PostingNode *next;
            };

            Arena &pers_arena_;
            HashMap<Hash64, PostingNode *> inverted_index_;
            HashMap<u64, u32> doc_fp_c_;


            static constexpr Hash64 make_idx_key (QuestionId qid, Hash64 fp_hash) noexcept {
                return (static_cast<Hash64> (qid) << 48u) ^ fp_hash;
            }
            static constexpr u64 make_student_q_key (QuestionId qid, StudentId sid) noexcept {
                return (static_cast<u64> (qid) << 32u) | static_cast<u64> (sid);
            }

        public:
            KmerWinnow (Arena &pers, usize idxc = 16384) :
                pers_arena_ (pers), inverted_index_ (pers_arena_, idxc), doc_fp_c_ (pers_arena_, 4096) {}
            static void extract_fingerprints (StringView raw_text, Arena &scratch, Array<Hash64> &out_fingerprints);
            void index_and_match (StudentId sid, QuestionId qid, const Array<Hash64> &fingerprints, Arena &scratch,
                                  Array<TextSimMatch> &out_matches) noexcept;
    };
} // namespace core::anticheat
