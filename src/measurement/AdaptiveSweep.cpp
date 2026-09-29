/*
 * Copyright (c) 2026, Barcelona Supercomputing Center
 * Contact: mess             [at] bsc [dot] es
 *          victor.xirau     [at] bsc [dot] es
 *          mariana.carmin   [at] bsc [dot] es
 *          pau.diazcuesta   [at] bsc [dot] es
 *          javier.beiro     [at] bsc [dot] es
 *          petar.radojkovic [at] bsc [dot] es
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *     * Neither the name of the copyright holder nor the names
 *       of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "measurement/AdaptiveSweep.h"
#include "platform/Console.h"
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>

namespace measurement {
bool run_adaptive_sweep(const BenchmarkConfig& config, const std::vector<std::string>& labels,
                        ProgressTracker& progress, int& total_measurements,
                        const MeasurePoint& measure, const AccountPoint& account_progress,
                        int refinement_reserve) {
    if (labels.empty()) return true;
    const auto tier = CurveTracer::resolve_tier(config.adaptive_tier, config.adaptive_point_count);
    struct AdaptiveModeState {
        size_t index;
        std::string label;
        double bw_peak = 0.0;
        std::unique_ptr<CurveTracer> tracer;
        std::map<int, MeasuredPoint> unused_guards;
        bool completed = false;
    };
    std::vector<AdaptiveModeState> states(labels.size());
    for (size_t i = 0; i < labels.size(); ++i) {
        states[i].index = i;
        states[i].label = labels[i];
    }
    auto display_progress = [&]() {
        int remaining = total_measurements - progress.completed_iterations();
        std::string line = progress.format_progress(
            progress.completed_iterations(), total_measurements, remaining);
        std::cout << "\r" << line << std::flush;
    };

    auto update_progress_estimate = [&]() {
        int active_runs = 0;
        int active_points = 0;
        int active_segments = 0;
        for (const auto& state : states) {
            if (state.completed || !state.tracer) {
                continue;
            }
            active_runs++;
            active_points += state.tracer->point_count();
            active_segments += state.tracer->segments_above_threshold();
        }

        total_measurements = progress.estimate_with_active_runs(
            active_runs, active_points, active_segments);
        if (config.verbosity < 2) {
            display_progress();
        }
    };

    auto measure_point_for_state = [&](AdaptiveModeState& state,
                                       int pause,
                                       bool count_progress,
                                       bool replace_existing) {
        MeasuredPoint measured = measure(state.index, pause, state.bw_peak,
                                         count_progress, replace_existing);
        state.bw_peak = std::max(state.bw_peak, measured.stats.bw);
        return measured;
    };

    try {
        for (auto& state : states) {
            auto* state_ptr = &state;
            auto measure_fn = [&, state_ptr](int pause) -> ProbeStats {
                return measure_point_for_state(*state_ptr, pause, true, false).stats;
            };
            state_ptr->tracer = std::make_unique<CurveTracer>(tier, measure_fn, config.verbosity);
        }

        if (config.verbosity >= 2) {
            std::cout << platform::clear_line();
            std::cout << " Adaptive Setup:" << std::endl;
            std::cout << "   Modes:";
            for (size_t i = 0; i < states.size(); ++i) {
                std::cout << (i == 0 ? " " : ", ") << states[i].label;
            }
            std::cout << std::endl;
            std::cout << "   Budget: " << tier.max_budget << " points/mode";
            if (!states.empty()) {
                std::cout << " (" << (tier.max_budget * static_cast<int>(states.size()))
                          << " total)";
            }
            std::cout << std::endl << std::endl;
            display_progress();
        }

        auto measure_shared_pause = [&](int pause) {
            for (auto& state : states) {
                MeasuredPoint measured =
                    measure_point_for_state(state, pause, true, false);
                state.tracer->seed_point(measured.stats, true);
            }
            update_progress_estimate();
        };

        auto validate_suspicious_point = [&](AdaptiveModeState& state, int center_pause) {
            auto suspicious = state.tracer->suspicious_triplet_around(center_pause);
            if (!suspicious.has_value()) {
                return;
            }

            const int left_pause = suspicious->left_pause;
            const int right_pause = suspicious->right_pause;

            if (config.verbosity >= 2) {
                std::cout << platform::clear_line();
                std::cout << " Adaptive Validating suspicious " << state.label
                          << " pause " << center_pause << " between ["
                          << left_pause << ", " << right_pause << "]" << std::endl;
            }

            MeasuredPoint repeated =
                measure_point_for_state(state, center_pause, false, true);
            state.tracer->replace_point(repeated.stats);

            std::optional<MeasuredPoint> left_guard;
            std::optional<MeasuredPoint> right_guard;
            bool left_guard_new = false;
            bool right_guard_new = false;

            const int guard_left_pause = CurveTracer::split_pause(left_pause, center_pause);
            if (guard_left_pause > 0 &&
                state.tracer->measured_points().find(guard_left_pause) ==
                    state.tracer->measured_points().end()) {
                left_guard = measure_point_for_state(state, guard_left_pause, false, false);
                state.tracer->seed_point(left_guard->stats, false);
                left_guard_new = true;
            }

            const int guard_right_pause = CurveTracer::split_pause(center_pause, right_pause);
            if (guard_right_pause > 0 &&
                state.tracer->measured_points().find(guard_right_pause) ==
                    state.tracer->measured_points().end()) {
                right_guard = measure_point_for_state(state, guard_right_pause, false, false);
                state.tracer->seed_point(right_guard->stats, false);
                right_guard_new = true;
            }

            state.tracer->prepare_seeded_refinement();

            const SuspiciousTriplet outer =
                state.tracer->triplet_metrics(left_pause, center_pause, right_pause);
            const bool outer_is_suspicious =
                outer.is_suspicious(state.tracer->suspicious_threshold(outer));

            bool left_supported = true;
            if (guard_left_pause > 0 &&
                state.tracer->measured_points().find(guard_left_pause) !=
                    state.tracer->measured_points().end()) {
                const SuspiciousTriplet left_triplet =
                    state.tracer->triplet_metrics(left_pause, guard_left_pause, center_pause);
                left_supported =
                    !left_triplet.is_suspicious(state.tracer->suspicious_threshold(left_triplet));
            }

            bool right_supported = true;
            if (guard_right_pause > 0 &&
                state.tracer->measured_points().find(guard_right_pause) !=
                    state.tracer->measured_points().end()) {
                const SuspiciousTriplet right_triplet =
                    state.tracer->triplet_metrics(center_pause, guard_right_pause, right_pause);
                right_supported = !right_triplet.is_suspicious(
                    state.tracer->suspicious_threshold(right_triplet));
            }

            const bool validated_feature =
                outer_is_suspicious && left_supported && right_supported;

            if (validated_feature) {
                if (left_guard_new && left_guard.has_value()) {
                    state.tracer->promote_budget_point(guard_left_pause);
                    account_progress(left_guard->results);
                }
                if (right_guard_new && right_guard.has_value()) {
                    state.tracer->promote_budget_point(guard_right_pause);
                    account_progress(right_guard->results);
                }
            } else {
                if (left_guard_new && left_guard.has_value()) state.unused_guards[guard_left_pause] = *left_guard;
                if (right_guard_new && right_guard.has_value()) state.unused_guards[guard_right_pause] = *right_guard;
            }

            if (config.verbosity >= 2) {
                std::cout << platform::clear_line();
                std::cout << " Adaptive Validation " << state.label << " pause "
                          << center_pause << ": "
                          << (validated_feature ? "feature" : "corrected")
                          << std::endl;
            }
        };

        auto validate_existing_suspicious_points = [&](AdaptiveModeState& state) {
            bool changed = false;
            do {
                changed = false;
                std::vector<int> pauses = state.tracer->current_points();
                for (size_t i = 1; i + 1 < pauses.size(); ++i) {
                    if (state.tracer->suspicious_triplet_around(pauses[i]).has_value()) {
                        validate_suspicious_point(state, pauses[i]);
                        changed = true;
                        break;
                    }
                }
            } while (changed);
        };

        measure_shared_pause(0);
        measure_shared_pause(1);

        const int step_factor = 10;
        const long long max_pause = 1000000000LL;
        std::vector<int> horizons(states.size(), 1);
        std::vector<bool> horizon_fixed(states.size(), false);
        long long p = 10;
        long long last_discovery_pause = 1;

        while (p <= max_pause) {
            measure_shared_pause(static_cast<int>(p));
            last_discovery_pause = p;

            bool all_fixed = true;
            for (size_t i = 0; i < states.size(); ++i) {
                if (horizon_fixed[i]) {
                    continue;
                }

                const auto& measured_points = states[i].tracer->measured_points();
                auto it = measured_points.find(static_cast<int>(p));
                if (it == measured_points.end()) {
                    all_fixed = false;
                    continue;
                }

                const ProbeStats& sample = it->second;
                if (sample.sigma_bw / std::max(sample.bw, 1e-12) > 0.3) {
                    horizons[i] = std::max(1, static_cast<int>(p / step_factor));
                    horizon_fixed[i] = true;
                } else if (sample.bw < 0.01 * states[i].bw_peak) {
                    horizons[i] = static_cast<int>(p);
                    horizon_fixed[i] = true;
                } else {
                    all_fixed = false;
                }
            }

            if (all_fixed) {
                break;
            }

            p *= step_factor;
        }

        for (size_t i = 0; i < states.size(); ++i) {
            if (!horizon_fixed[i]) {
                horizons[i] = static_cast<int>(std::min(last_discovery_pause, max_pause));
            }
        }

        if (tier.tolerance < 0.05) {
            long long extended = static_cast<long long>(*std::max_element(horizons.begin(), horizons.end())) * 10;
            if (extended <= max_pause) {
                measure_shared_pause(static_cast<int>(extended));
            }
        }

        std::vector<int> skeleton_candidates =
            CurveTracer::compute_skeleton_candidates(states.front().tracer->current_points());
        const bool skeleton_fits = refinement_reserve <= 0 ||
            std::all_of(states.begin(), states.end(), [&](const AdaptiveModeState& state) {
                return state.tracer->point_count() + static_cast<int>(skeleton_candidates.size()) +
                       refinement_reserve <= tier.max_budget;
            });
        if (skeleton_fits) {
            for (int skeleton_pause : skeleton_candidates) measure_shared_pause(skeleton_pause);
        }

        for (auto& state : states) {
            validate_existing_suspicious_points(state);
        }

        for (auto& state : states) {
            state.tracer->prepare_seeded_refinement();
        }
        update_progress_estimate();

        int completed_modes = 0;
        while (completed_modes < static_cast<int>(states.size())) {
            int best_idx = -1;
            double best_error = -1.0;

            for (size_t i = 0; i < states.size(); ++i) {
                auto& state = states[i];
                if (state.completed) {
                    continue;
                }

                if (!state.tracer->has_pending_refinement()) {
                    state.completed = true;
                    completed_modes++;
                    total_measurements = progress.record_adaptive_run(
                        state.tracer->point_count());
                    update_progress_estimate();
                    continue;
                }

                double error = state.tracer->current_worst_error();
                if (error > best_error) {
                    best_error = error;
                    best_idx = static_cast<int>(i);
                }
            }

            if (best_idx < 0) {
                break;
            }

            if (config.verbosity >= 3) {
                std::cout << platform::clear_line();
                std::cout << " Adaptive Refining " << states[best_idx].label
                          << " (worst E=" << std::scientific << std::setprecision(3)
                          << best_error << std::fixed << ")" << std::endl;
            }

            auto& state = states[static_cast<size_t>(best_idx)];
            int inserted_pause = state.tracer->next_refinement_pause();
            if (inserted_pause < 0) {
                state.completed = true;
                completed_modes++;
                total_measurements = progress.record_adaptive_run(
                    state.tracer->point_count());
            } else if (auto guard = state.unused_guards.find(inserted_pause); guard != state.unused_guards.end()) {
                state.tracer->promote_budget_point(inserted_pause);
                account_progress(guard->second.results);
                state.unused_guards.erase(guard);
            } else {
                MeasuredPoint measured =
                    measure_point_for_state(state, inserted_pause, true, false);
                state.tracer->seed_point(measured.stats, true);
                validate_suspicious_point(state, inserted_pause);
                state.tracer->prepare_seeded_refinement();
            }
            update_progress_estimate();
        }

        for (auto& state : states) {
            if (!state.completed) {
                state.completed = true;
                total_measurements = progress.record_adaptive_run(
                    state.tracer->point_count());
            }

            if (config.verbosity >= 2) {
                std::vector<int> discovered = state.tracer->current_points();
                std::cout << platform::clear_line();
                std::cout << " Adaptive " << state.label << " pauses (" << discovered.size()
                          << "): ";
                for (size_t i = 0; i < discovered.size(); ++i) {
                    if (i > 0) {
                        std::cout << ", ";
                    }
                    std::cout << discovered[i];
                }
                std::cout << std::endl;
            }
        }
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "ERROR: Adaptive ratio execution failed: " << ex.what() << std::endl;
        return false;
    }
}

}
