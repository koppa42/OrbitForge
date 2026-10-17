#pragma once
#include "ground_map.hpp"
#include <orbitforge/runtime/project.hpp>
#include <vsg/nodes/MatrixTransform.h>

namespace orbitforge::desktop {
struct PreparedProject {
    application::Project project;
    application::Scenario scenario;
    std::shared_ptr<const core::IEarthOrientationProvider> eop;
    std::vector<std::shared_ptr<const OrbitScene>> orbits;
    std::vector<vsg::ref_ptr<vsg::MatrixTransform>> models;
    GroundMap::PreparedTracks ground_tracks;
    std::map<std::string, std::shared_ptr<const core::GravityModel>> gravity{};
    std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> ephemerides{};
    std::map<std::string, std::shared_ptr<const core::SpaceWeather>> weather{};
    std::map<std::string, std::shared_ptr<const core::OceanTideModel>> ocean_tides{};
};
// Preparation selects current data covering the interval; saved hashes are provenance.
// Inputs and providers are captured for the whole calculation, leaving the live workbench intact.
[[nodiscard]] std::shared_ptr<PreparedProject> prepare_project_scene(
    application::Project project, const std::filesystem::path &path,
    std::shared_ptr<const core::IFrameProvider> frames,
    const std::vector<std::shared_ptr<const core::IEarthOrientationProvider>> &installed_data,
    const std::atomic_bool &cancelled,
    std::optional<application::Scenario> retained_sessions = std::nullopt,
    const std::optional<std::string> &edited_object = std::nullopt,
    std::shared_ptr<const core::ITimeProvider> time = {},
    const std::map<std::string, std::shared_ptr<const core::GravityModel>> &gravity = {},
    const std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> &ephemerides = {},
    const std::map<std::string, std::shared_ptr<const core::SpaceWeather>> &weather = {},
    const std::map<std::string, std::shared_ptr<const core::OceanTideModel>> &ocean_tides = {});
} // namespace orbitforge::desktop
