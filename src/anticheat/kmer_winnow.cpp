#include "anticheat/kmer_winnow.hpp"


namespace core::anticheat {

    void KmerWinnow::extract_fingerprints (StringView raw_text, Arena &scratch, Array<Hash64> &out_fingerprints) {
        out_fingerprints.clear ();
        if (raw_text.size () < KMER_LEN) return;

        char *normb    = static_cast<char *> (scratch.allocate (raw_text.size (), 1));
        usize norm_len = 0;

        for (usize i = 0; i < raw_text.size (); i++) {
            char c = raw_text[i];
            if (c >= 'A' && c <= 'Z')
                normb[norm_len++] = c + 'a' - 'A';
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
                normb[norm_len++] = c;
        }

        if (norm_len < KMER_LEN) return;
        const usize num_kmer = norm_len - KMER_LEN + 1;

        Array<u64> kmer_hashes (scratch, num_kmer);
        u64 hpow = 1;
        for (usize i = 0; i < KMER_LEN - 1; i++) hpow = (hpow * RK_BASE) % RK_MOD;
        u64 curr_hash = 1;
        for (usize i = 0; i < KMER_LEN - 1; i++) curr_hash = (curr_hash * RK_BASE + (u8) normb[i]) % RK_MOD;
        kmer_hashes.push_back (curr_hash);
        for (usize i = KMER_LEN; i < norm_len; ++i) {
            u64 leading = (static_cast<u8> (normb[i - KMER_LEN]) * hpow) % RK_MOD;
            curr_hash   = (curr_hash + RK_MOD - leading) % RK_MOD;
            curr_hash   = (curr_hash * RK_BASE + static_cast<u8> (normb[i])) % RK_MOD;
            kmer_hashes.push_back (curr_hash);
        }

        if (num_kmer <= WINDOW) {
            u64 min_h = kmer_hashes[0];
            for (usize i = 1; i < num_kmer; i++) min_h = min (min_h, kmer_hashes[i]);
            out_fingerprints.push_back (min_h);
            return;
        }

        usize last_picked_idx = ~static_cast<usize> (0);
        for (usize w_start = 0; w_start + WINDOW <= num_kmer; ++w_start) {
            usize min_idx = w_start;
            for (usize j = w_start + 1; j < w_start + WINDOW; ++j) {
                if (kmer_hashes[j] <= kmer_hashes[min_idx]) { min_idx = j; }
            }
            if (min_idx != last_picked_idx) {
                bool already_added = false;
                for (usize k = 0; k < out_fingerprints.size (); ++k) {
                    if (out_fingerprints[k] == kmer_hashes[min_idx]) {
                        already_added = true;
                        break;
                    }
                }
                if (!already_added) { out_fingerprints.push_back (kmer_hashes[min_idx]); }
                last_picked_idx = min_idx;
            }
        }
    }

    void KmerWinnow::index_and_match (StudentId sid, QuestionId qid, const Array<Hash64> &fingerprints, Arena &scratch,
                                      Array<TextSimMatch> &out_matches) noexcept {
        out_matches.clear ();
        const u32 my_fp_cnt = (u32) fingerprints.size ();
        if (my_fp_cnt == 0) return;

        doc_fp_c_.insert (make_student_q_key (qid, sid), my_fp_cnt);


        HashMap<StudentId, u32> hit_cnts (scratch, 64);
        Array<StudentId> matched_students (scratch, 32);

        for (usize i = 0; i < fingerprints.size (); i++) {
            const Hash64 idxkey     = make_idx_key (qid, fingerprints[i]);
            PostingNode **head_slot = inverted_index_.find (idxkey);

            bool flag = false;

            if (head_slot != NULL) {
                for (PostingNode *curr = *head_slot; curr; curr = curr->next) {
                    if (curr->student_id == sid) {
                        flag = true;
                        continue;
                    }

                    u32 *cpt = hit_cnts.find (curr->student_id);
                    if (cpt != NULL) {
                        hit_cnts.insert (curr->student_id, 1u);
                        matched_students.push_back (curr->student_id);
                    } else
                        ++(*cpt);
                }
            }

            if (!flag) {
                auto *newn       = pers_arena_.create<PostingNode> ();
                newn->student_id = sid;
                newn->next       = (head_slot) ? *head_slot : nullptr;
                inverted_index_.insert (idxkey, newn);
            }
        }

        for (const auto &x: matched_students) {
            const StudentId otsid = x;
            const u32 shared      = *hit_cnts.find (otsid);
            if (shared < 2) continue;

            const u32 *otttpr = doc_fp_c_.find (make_student_q_key (sid, otsid));
            const u32 ottt    = otttpr ? *otttpr : shared;
            const u32 demon   = min (my_fp_cnt, ottt);

            const f32 score = (demon > 0) ? (f32) shared / (f32) demon : 0.0;
            out_matches.emplace_back (otsid, shared, clamp (score, 0.0f, 1.0f));
        }
    }


} // namespace core::anticheat
