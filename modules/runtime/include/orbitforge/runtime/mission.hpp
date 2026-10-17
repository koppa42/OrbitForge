#pragma once
#include <orbitforge/application/session.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/core/propagation/mission.hpp>

namespace orbitforge::runtime {
[[nodiscard]] application::Session
make_mission_session(core::CartesianState initial, core::MissionPlan plan,
                     core::HpopOptions options = {}, core::HpopEnvironment environment = {},
                     std::string object_id = "satellite-1", std::uint64_t input_revision = 1,
                     std::int64_t reporting_step_nanoseconds = 0,
                     std::optional<core::Epoch> stop = {});
} // namespace orbitforge::runtime
