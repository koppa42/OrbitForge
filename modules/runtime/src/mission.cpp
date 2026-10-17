#include <orbitforge/core/propagation/central_body.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/mission.hpp>
#include <orbitforge/runtime/two_body.hpp>

namespace orbitforge::runtime {
namespace {
// Keep the provider boundary behind Session: optional provider dependencies and
// their algorithm identities remain owned by the existing runtime factories.
class CoastSession final : public core::IStateProvider {
  public:
    explicit CoastSession(application::Session session) : session_(std::move(session)) {}
    std::vector<core::Epoch> discontinuities() const override { return session_.discontinuities(); }
    std::vector<core::Epoch>
    discontinuities(const std::function<void()> &checkpoint) const override {
        return session_.discontinuities(checkpoint);
    }
    core::CartesianState state_at(core::Epoch epoch) const override { return state_at(epoch, {}); }
    core::CartesianState state_at(core::Epoch epoch,
                                  const std::function<void()> &checkpoint) const override {
        const std::array epochs{epoch};
        return session_.sample(epochs, checkpoint).states().front();
    }

  private:
    application::Session session_;
};
} // namespace
application::Session make_mission_session(core::CartesianState initial, core::MissionPlan plan,
                                          core::HpopOptions options,
                                          core::HpopEnvironment environment, std::string object_id,
                                          std::uint64_t revision,
                                          std::int64_t reporting_step_nanoseconds,
                                          std::optional<core::Epoch> stop) {
    (void)core::validate_mission(initial.epoch, plan);
    const auto hpop = plan.coast_model == core::MissionCoastModel::hpop;
    const auto make_coast = [hpop, options, environment, reporting_step_nanoseconds](
                                core::CartesianState state, core::Epoch end) {
        return hpop ? make_hpop_interpolated_session(state, state.epoch, end, options, environment,
                                                     reporting_step_nanoseconds)
                    : make_two_body_session(state, core::central_body(state.origin).mu, "coast", 1);
    };
    const auto algorithm =
        "mission-impulsive-gcrf-tai-v2;end=" +
        std::to_string(
            stop.value_or(core::validate_mission(initial.epoch, plan)).tai_nanoseconds()) +
        ";coast-template=" + make_coast(initial, initial.epoch).provenance().algorithm;
    core::CoastFactory factory = [make_coast](core::CartesianState state, core::Epoch end,
                                              const std::function<void()> &checkpoint) {
        if (checkpoint)
            checkpoint();
        return std::make_shared<CoastSession>(make_coast(state, end));
    };
    return {core::make_mission(initial, std::move(plan), std::move(factory), stop),
            {std::move(object_id), revision, algorithm}};
}
} // namespace orbitforge::runtime
