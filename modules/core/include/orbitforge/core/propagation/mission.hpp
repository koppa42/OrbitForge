#pragma once
#include <memory>
#include <optional>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core {
// Each factory receives the exact end of this coast and the current cancellation checkpoint.
using CoastFactory = std::function<std::shared_ptr<const IStateProvider>(
    CartesianState, Epoch, const std::function<void()> &)>;
// Each impulse restarts the supplied coast model with its post-burn state.
// Queries are restricted to [initial, plan end], with the post-burn state at an
// exact burn epoch. Consecutive impulses at one epoch execute in sequence order.
// Optional stop restricts computation to a prefix of the plan (including all burns at stop).
// Immutable configuration; completed arcs are cached under a mutex. Cancellation
// throws without returning partial results and a later query can safely retry.
[[nodiscard]] std::shared_ptr<const IStateProvider> make_mission(CartesianState initial,
                                                                 MissionPlan plan,
                                                                 CoastFactory coast,
                                                                 std::optional<Epoch> stop = {});
} // namespace orbitforge::core
