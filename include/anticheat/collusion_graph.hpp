#pragma once

#include "core/arena_alloc.hpp"
#include "core/dsu.hpp"
#include "core/dyn_array.hpp"
#include "core/hash_map.hpp"
#include "core/types.hpp"

namespace core::anticheat {

    /**
     * CollusionGraph - weighted undirected graph for cheating-cluster detection.
     *
     * Each edge (A, B) accumulates three independent suspicion signals:
     *   - text_sim    : peak cosine/Jaccard similarity from KmerWinnow         [0,1]
     *   - time_score  : accumulated time-sync spike hits from AnomalyStats     [0,∞)
     *   - err_score   : accumulated shared-rare-error hits from AnomalyStats   [0,∞)
     *
     * A combined weight W(A,B) is computed as a weighted sum of these three.
     * When W(A,B) >= COLLUSION_THRESHOLD the edge is "confirmed" and the two
     * students are merged into the same DSU component (cheating cluster).
     *
     * Memory: all nodes and the edge hash-map live in the arena supplied at init().
     *         No individual frees occur. The DSU is re-sized once at init().
     *
     * Status codes:
     *    Ok                   success
     *    InvalidState         used before init() or bad arguments
     *    OutOfMemory          arena too small
     *    DuplicateSubmission  update_edge called with identical (a,b) order after flipping
     */
    struct CollusionEdge {
            StudentId a;
            StudentId b;
            u32 cluster_root;    // DSU root at the time of last query
            f32 combined_weight; // W(A,B)
            f32 peak_text_sim;   // highest text similarity seen
            f32 accum_time_score;
            f32 accum_err_score;
            u16 collision_count; // how many times the edge has been updated
    };

    class CollusionGraph {
        public:
            // Combined weight threshold to declare a cheating pair
            static constexpr f32 COLLUSION_THRESHOLD = 3.0f;

            // Weights for the linear combination
            static constexpr f32 W_TEXT = 4.0f;
            static constexpr f32 W_TIME = 1.0f;
            static constexpr f32 W_ERR  = 1.5f;

            CollusionGraph ()                                  = default;
            CollusionGraph (const CollusionGraph &)            = delete;
            CollusionGraph &operator= (const CollusionGraph &) = delete;

            /**
             * Must be called once before any other method.
             * @param arena         persistent arena for edge storage and DSU
             * @param max_students  upper bound on student IDs (used for DSU sizing)
             * @param max_edges     expected peak unique edges (pre-sizes the hash map)
             */
            [[nodiscard]] StatusCode init (Arena &arena, u32 max_students, usize max_edges = 4096) noexcept;
            [[nodiscard]] bool ready () const noexcept { return ready_; }
            void clear () noexcept;

            // ---- Mutations -------------------------------------------------------------

            /**
             * Apply text-similarity evidence for a pair.
             * Updates peak_text_sim (keeps the maximum seen).
             * Recomputes combined_weight; if it crosses COLLUSION_THRESHOLD,
             * merges A and B in the DSU and returns true.
             *
             * @param a, b        student pair (order irrelevant)
             * @param text_sim    Jaccard similarity in [0,1] from KmerWinnow
             * @param flagged_out set to true if the pair is now confirmed colluders
             */
            void update_text (StudentId a, StudentId b, f32 text_sim, bool &flagged_out) noexcept;

            /**
             * Apply statistical-anomaly evidence from AnomalyStats.
             * Adds time_delta to accum_time_score and err_delta to accum_err_score.
             * Recomputes combined_weight; merges if threshold crossed.
             *
             * @param a, b        student pair
             * @param time_delta  time_score increment (>= 0)
             * @param err_delta   err_score increment  (>= 0)
             * @param flagged_out set to true if the pair is now confirmed colluders
             */
            void update_stat (StudentId a, StudentId b, f32 time_delta, f32 err_delta, bool &flagged_out) noexcept;

            // ---- Queries ---------------------------------------------------------------

            /**
             * Is the student currently in a confirmed-collusion DSU cluster?
             * (i.e. is their component size > 1?)
             */
            [[nodiscard]] bool is_flagged (StudentId sid) noexcept;

            /**
             * DSU root of the cluster this student belongs to.
             * Always valid even if the student is alone.
             */
            [[nodiscard]] u32 cluster_root (StudentId sid) noexcept;

            /**
             * DSU cluster size for this student.
             */
            [[nodiscard]] u32 cluster_size (StudentId sid) noexcept;

            /**
             * Retrieve edge data for a pair. Returns false if the edge doesn't exist.
             */
            [[nodiscard]] bool get_edge (StudentId a, StudentId b, CollusionEdge *out) const noexcept;

            /**
             * Copy up to max_edges confirmed edges (W >= COLLUSION_THRESHOLD) into out.
             * Returns the number of edges written.
             */
            [[nodiscard]] usize get_confirmed_edges (CollusionEdge *out, usize max_edges) const noexcept;

            /**
             * Total number of stored edges (confirmed or not).
             */
            [[nodiscard]] usize edge_count () const noexcept { return edge_map_ ? edge_map_->size () : 0u; }

        private:
            // An edge in the map: packed struct, trivially copyable.
            struct EdgeData {
                    f32 peak_text_sim{0.0f};
                    f32 accum_time_score{0.0f};
                    f32 accum_err_score{0.0f};
                    f32 combined_weight{0.0f};
                    u16 collision_count{0u};
                    bool confirmed{false}; // has W crossed COLLUSION_THRESHOLD?
            };

            HashMap<u64, EdgeData> *edge_map_{nullptr};
            DisjointSet *dsu_{nullptr};
            bool ready_{false};

            // Canonical key: lo_id << 32 | hi_id
            static constexpr u64 edge_key (StudentId a, StudentId b) noexcept {
                const StudentId lo = (a < b) ? a : b;
                const StudentId hi = (a < b) ? b : a;
                return (static_cast<u64> (lo) << 32u) | static_cast<u64> (hi);
            }

            // Recompute combined_weight and (if newly crossing threshold) unite in DSU.
            void recompute_and_check (StudentId a, StudentId b, EdgeData &ed, bool &flagged_out) noexcept;
    };

} // namespace core::anticheat
