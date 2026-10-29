#include "anticheat/anomaly_stats.hpp"

namespace core::anticheat {

    // =========================================================================
    // init / clear
    // =========================================================================

    StatusCode AnomalyStats::init (Arena &arena, usize max_pairs) noexcept {
        arena_ = &arena;

        const Arena::Marker mark = arena.get_marker ();

        dt_index_ = arena.create<HashMap<u64, u32>> (arena, 4096u);
        if (!dt_index_) {
            arena.reset_to_marker (mark);
            return StatusCode::OutOfMemory;
        }

        mcq_index_ = arena.create<HashMap<u32, McqPostingNode *>> (arena, 4096u);
        if (!mcq_index_) {
            arena.reset_to_marker (mark);
            return StatusCode::OutOfMemory;
        }

        option_cnt_ = arena.create<HashMap<u32, u32>> (arena, 4096u);
        if (!option_cnt_) {
            arena.reset_to_marker (mark);
            return StatusCode::OutOfMemory;
        }

        question_total_ = arena.create<HashMap<u32, u32>> (arena, 1024u);
        if (!question_total_) {
            arena.reset_to_marker (mark);
            return StatusCode::OutOfMemory;
        }

        pair_map_ = arena.create<HashMap<u64, PairScore>> (arena, max_pairs);
        if (!pair_map_) {
            arena.reset_to_marker (mark);
            return StatusCode::OutOfMemory;
        }

        ready_ = true;
        return StatusCode::Ok;
    }

    void AnomalyStats::clear () noexcept {
        // We cannot call destructors or free map memory individually (arena model).
        // Reset is only meaningful if the caller resets the arena too.
        ready_ = false;
    }

    // =========================================================================
    // Internal helper: add_pair_hit
    // =========================================================================

    void AnomalyStats::add_pair_hit (StudentId a, StudentId b, f32 time_delta, f32 err_delta,
                                     Array<StatAnomalyMatch> &out_matches) noexcept {
        const u64 key = pair_key (a, b);
        PairScore *ps = pair_map_->find (key);
        if (ps == nullptr) {
            PairScore fresh{time_delta, err_delta};
            pair_map_->insert (key, fresh);
            ps = pair_map_->find (key);
        } else {
            ps->time_score += time_delta;
            ps->err_score += err_delta;
        }

        // Determine which of a/b is the "other" relative to whichever caller placed us.
        // We always emit a match describing "b as other" (canonical order is handled above).
        if (ps != nullptr) {
            StatAnomalyMatch m{b, ps->time_score, ps->err_score};
            out_matches.push_back (m);
        }
    }

    // =========================================================================
    // record_dt
    // =========================================================================

    void AnomalyStats::record_dt (StudentId sid, QuestionId qid, u32 dt_ms, Arena &scratch,
                                  Array<StatAnomalyMatch> &out_matches) noexcept {
        if (!ready_) return;

        out_matches.clear ();

        const u64 my_key = dt_key (qid, sid);

        // Collect all existing (qid, *) entries from dt_index_ by iterating
        // through a scratch array of (sid, dt) we find per question.
        // Because HashMap doesn't support prefix iteration, we store dt entries
        // keyed by (qid<<32|sid) and rely on the fact that for a given question
        // the key space is qid<<32 | [0..max_students).  We do a linear scan
        // of the internal slots – this is intentional and acceptable because
        // the number of students per question is bounded by max_students, and
        // we only do it once per submission event.

        // We iterate through the raw slots of dt_index_ by probing with a
        // scratch list of other students we have recorded for this qid.
        // To avoid O(capacity) iteration we maintain a per-question submission
        // list via the mcq_index_ reuse pattern below, using a separate
        // "dt posting list" embedded in the scratch arena.

        // Scratch: collect (sid, dt) pairs for this question.
        // We build a tiny parallel structure: one PostingNode list per qid stored
        // through the mcq_index_ channel keyed with a synthetic option = 0xFF.
        constexpr u8 DT_SENTINEL_OPTION = 0xFEu;
        const u32 list_key              = mcq_key (qid, DT_SENTINEL_OPTION);

        McqPostingNode **list_head_slot = mcq_index_->find (list_key);
        McqPostingNode *list_head       = list_head_slot ? *list_head_slot : nullptr;

        // Compare against every previously recorded student for this question.
        for (McqPostingNode *cur = list_head; cur != nullptr; cur = cur->next) {
            if (cur->student_id == sid) continue;

            const u64 other_key = dt_key (qid, cur->student_id);
            const u32 *other_dt = dt_index_->find (other_key);
            if (other_dt == nullptr) continue;

            const u32 diff = (dt_ms >= *other_dt) ? (dt_ms - *other_dt) : (*other_dt - dt_ms);
            if (diff <= TIME_SYNC_THRESHOLD_MS) {
                add_pair_hit (sid, cur->student_id, TIME_HIT_WEIGHT, 0.0f, out_matches);
            }
        }

        // Record this student's Δt (overwrite if already present).
        dt_index_->insert (my_key, dt_ms);

        // Append this student to the per-question posting list if not already there.
        bool already_listed = false;
        for (McqPostingNode *cur = list_head; cur != nullptr; cur = cur->next) {
            if (cur->student_id == sid) {
                already_listed = true;
                break;
            }
        }
        if (!already_listed) {
            auto *node       = arena_->create<McqPostingNode> ();
            node->student_id = sid;
            node->next       = list_head;
            mcq_index_->insert (list_key, node);
        }

        (void) scratch; // scratch provided for API consistency; not needed here
    }

    // =========================================================================
    // record_mcq
    // =========================================================================

    void AnomalyStats::record_mcq (StudentId sid, QuestionId qid, u8 chosen_option, u8 correct_option, Arena &scratch,
                                   Array<StatAnomalyMatch> &out_matches) noexcept {
        if (!ready_) return;

        out_matches.clear ();

        // Update per-(qid) total submission count.
        const u32 qtot_key = static_cast<u32> (qid);
        u32 *qtot          = question_total_->find (qtot_key);
        if (qtot == nullptr) {
            question_total_->insert (qtot_key, 1u);
        } else {
            ++(*qtot);
        }
        const u32 total_submissions = qtot ? (*qtot) : 1u;

        // Update per-(qid, option) count.
        const u32 opt_key = mcq_key (qid, chosen_option);
        u32 *opt_cnt      = option_cnt_->find (opt_key);
        if (opt_cnt == nullptr) {
            option_cnt_->insert (opt_key, 1u);
            opt_cnt = option_cnt_->find (opt_key);
        } else {
            ++(*opt_cnt);
        }

        // Only flag if the chosen option is wrong.
        if (chosen_option == correct_option) {
            (void) scratch;
            return;
        }

        // Check if this option is "rare".
        const u32 this_opt_count = opt_cnt ? *opt_cnt : 1u;
        const f32 freq           = (total_submissions > 0u)
                                           ? static_cast<f32> (this_opt_count) / static_cast<f32> (total_submissions)
                                           : 1.0f;

        if (freq >= RARE_ERROR_FREQ_THRESHOLD) {
            (void) scratch;
            return; // Not rare enough to be suspicious
        }

        // Walk the posting list for (qid, chosen_option): these are all students
        // who also chose this rare wrong option.
        const u32 plist_key        = mcq_key (qid, chosen_option);
        McqPostingNode **head_slot = mcq_index_->find (plist_key);
        McqPostingNode *head       = head_slot ? *head_slot : nullptr;

        for (McqPostingNode *cur = head; cur != nullptr; cur = cur->next) {
            if (cur->student_id == sid) continue;
            add_pair_hit (sid, cur->student_id, 0.0f, ERR_HIT_WEIGHT, out_matches);
        }

        // Append this student to the posting list (if not already present).
        bool already_listed = false;
        for (McqPostingNode *cur = head; cur != nullptr; cur = cur->next) {
            if (cur->student_id == sid) {
                already_listed = true;
                break;
            }
        }
        if (!already_listed) {
            auto *node       = arena_->create<McqPostingNode> ();
            node->student_id = sid;
            node->next       = head;
            mcq_index_->insert (plist_key, node);
        }

        (void) scratch;
    }

    // =========================================================================
    // get_pair_scores
    // =========================================================================

    StatusCode AnomalyStats::get_pair_scores (StudentId a, StudentId b, f32 *out_time_score,
                                              f32 *out_err_score) const noexcept {
        if (!ready_ || out_time_score == nullptr || out_err_score == nullptr) { return StatusCode::InvalidState; }
        const u64 key = pair_key (a, b);
        PairScore *ps = pair_map_->find (key);
        if (ps == nullptr) return StatusCode::KeyNotFound;
        *out_time_score = ps->time_score;
        *out_err_score  = ps->err_score;
        return StatusCode::Ok;
    }

} // namespace core::anticheat
