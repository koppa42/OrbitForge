#pragma once
#include <memory>
#include <orbitforge/core/propagation/state_provider.hpp>
#include <orbitforge/results/state_series.hpp>
#include <span>

namespace orbitforge::application {
// Immutable provider session. Scenario replaces whole sessions on an input revision.
class Session {
  public:
    Session(std::shared_ptr<const core::IStateProvider> provider, results::Provenance provenance);
    [[nodiscard]] results::StateSeries sample(std::span<const core::Epoch> epochs,
                                              const std::function<void()> &checkpoint = {}) const;
    [[nodiscard]] const results::Provenance &provenance() const noexcept { return provenance_; }
    [[nodiscard]] std::vector<core::Epoch>
    discontinuities(const std::function<void()> &checkpoint = {}) const {
        return provider_->discontinuities(checkpoint);
    }
    [[nodiscard]] std::vector<core::ManeuverEvent>
    maneuvers(core::Epoch first, core::Epoch last,
              const std::function<void()> &checkpoint = {}) const {
        return provider_->maneuvers(first, last, checkpoint);
    }

  private:
    std::shared_ptr<const core::IStateProvider> provider_;
    results::Provenance provenance_;
};
} // namespace orbitforge::application
