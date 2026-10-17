#include <fstream>
#include <iostream>
#include <orbitforge/orbitforge.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <orbitforge/runtime/time_frames.hpp>

namespace of = orbitforge;
using namespace std::chrono_literals;

int main(int argc, char **argv) try {
    if (argc != 2)
        return 1;
    const std::filesystem::path path{argv[1]};
    of::Project project{"SDK facade", of::Epoch{0}, of::Epoch{120'000'000'000}};
    auto edit = project.edit();
    const auto station = edit.objects().create(
        of::objects::GroundStation{.name = "Station", .location = {0, 0, 100}});
    edit.commit();
    const auto analysis = project.prepare(of::DataSnapshot::from_providers({}));
    const auto trajectory = analysis.states(station, 30s, of::FrameId::itrf);
    const auto report = analysis.report(
        {.kind = of::ReportKind::cartesian_state,
         .object_ids = {station.id()},
         .frame = of::FrameId::itrf},
        {.columns = {{"state.x", "km", "Position X"}}, .time_scale = of::ReportTimeScale::tai});
    if (trajectory.states().size() != 5 ||
        report.numeric_value(0, 0) != trajectory.states().front().position_m[0] / 1000)
        return 2;
    project.objects().rename(station, "Renamed");
    project.save(path, analysis, true);
    auto plan = of::Project::plan_open(path);
    auto reopened = plan.open();
    if (project.dirty() || reopened.dirty() || reopened.name() != "SDK facade" ||
        reopened.objects().find("Renamed").size() != 1 ||
        reopened.objects().get(station.id()).kind() != of::ObjectKind::ground_station ||
        reopened.analysis_status() != of::AnalysisStatus::current)
        return 3;
    const auto current = reopened.prepare().state(reopened.objects().get(station.id()),
                                                  reopened.first(), of::FrameId::itrf);
    if (current.position_m != trajectory.states().front().position_m)
        return 4;
    const auto relativity = of::core::evaluate_relativity(
        {7e6, 0, 0}, {0, 6200, 5300}, 3.986004415e14, {1.496e11, 0, 0}, {0, 30000, 0},
        1.3271244004127942e20, {0, 0, 1});
    if (relativity.schwarzschild_mps2[0] <= 0 || relativity.lense_thirring_mps2[0] <= 0 ||
        relativity.de_sitter_mps2[0] >= 0 || relativity.acceleration_mps2[0] <= 0)
        return 13;
    if (of::runtime::time_frames_available()) {
        const auto file = path.parent_path() / "sdk-cssi.dat";
        std::ofstream output(file);
        output << "2000 1 2000 6 1999 12 6\n";
        for (int month = 1; month <= 6; ++month)
            output << "2000 " << month << " 170 150 20 15\n";
        output.close();
        const auto weather = of::runtime::load_space_weather(file);
        const auto time = of::runtime::make_builtin_time_provider();
        const auto at = time->from_utc({2000, 3, 15, 20, 0, 0, 0});
        const auto sample =
            weather->sample(at, *time, of::core::AtmosphereModel::nrlmsise00,
                            of::core::GeomagneticUpdate::daily, of::core::GeomagneticSource::kp);
        if (!weather->info().cssi_predictions || sample.f107 != 150 || sample.f107_average != 150 ||
            sample.ap != 15)
            return 38;
    }
    of::OrbitInput numerical;
    numerical.propagator = of::OrbitPropagator::hpop;
    numerical.hpop.gravity_degree = numerical.hpop.gravity_order = 0;
    numerical.hpop.integrator = of::HpopIntegrator::gauss_jackson;
    numerical.hpop.correction = of::HpopCorrection::pseudo;
    numerical.hpop.step_control = of::HpopStepControl::fixed;
    numerical.hpop.time_regularization = true;
    numerical.hpop.regularization_exponent = 1.75;
    numerical.hpop.steps_per_orbit = 1024;
    numerical.hpop.drag.geomagnetic_update = of::core::GeomagneticUpdate::three_hourly_interpolated;
    const auto satellite =
        reopened.objects().create(of::objects::Satellite{.name = "HPOP", .orbit = numerical});
    const auto numerical_analysis = reopened.prepare();
    const auto numerical_result = numerical_analysis.states(satellite, 30s, of::FrameId::gcrf);
    if (numerical_result.states().size() != 5 ||
        numerical_result.provenance().algorithm.find(
            "gauss-jackson12-summed-bs14to18-startup-pseudo-v3") == std::string::npos)
        return 5;
    if (numerical_result.provenance().algorithm.find("time=sundman-initial-ellipse-bounded-v1") ==
            std::string::npos ||
        numerical_result.provenance().algorithm.find("steps-per-orbit=1024") == std::string::npos)
        return 39;
    reopened.save(path, numerical_analysis, true);
    auto numerical_file = of::Project::open(path);
    if (numerical_file.to_data().satellites.at(0).orbit.hpop != numerical.hpop)
        return 6;
    std::filesystem::remove(path);
    of::OrbitInput mission;
    mission.propagator = of::OrbitPropagator::mission;
    mission.hpop.gravity_degree = mission.hpop.gravity_order = 0;
    mission.mission = {of::MissionCoastModel::hpop,
                       {of::MissionSegment::coast(60s),
                        of::MissionSegment::impulse({10, 0, 0}, of::ManeuverFrame::vnc, "Burn"),
                        of::MissionSegment::coast(60s)}};
    const auto spacecraft =
        reopened.objects().create(of::objects::Satellite{.name = "Mission", .orbit = mission});
    const auto mission_analysis = reopened.prepare();
    const auto maneuvers = mission_analysis.maneuvers(spacecraft);
    if (maneuvers.events().size() != 1 || maneuvers.events()[0].cumulative_delta_v_mps != 10 ||
        mission_analysis.states(spacecraft, 30s, of::FrameId::gcrf).states().size() != 5)
        return 7;
    reopened.save(path, mission_analysis, true);
    if (of::Project::open(path).to_data().satellites.back().orbit.mission != mission.mission)
        return 8;
    std::filesystem::remove(path);
    const auto gravity_file = path.parent_path() / "sdk-gravity.grv";
    {
        std::ofstream file(gravity_file);
        file << "stk.v.6.0\nModel SDK\nDegree 2\nOrder 2\nGm 3.986004415e14\n"
                "RefDistance 6378136.3\nNormalized Yes\nIncludesPermTide No\n"
                "Begin SecularVariations\nRefEpochTDT 2451545\n"
                "LinearRate Cos 2 0 1e-11\nEnd SecularVariations\n"
                "BEGIN Coefficients\n2 0 -4.84165371736e-4 0\nEND Coefficients\n";
    }
    const auto resource = of::runtime::load_gravity(gravity_file);
    if (resource->info().degree != 2 || resource->info().sha256.size() != 64)
        return 9;
    if (!resource->info().secular_variations ||
        resource->info().secular_variations->reference_epoch != of::Epoch{-32184000000LL})
        return 14;
    const auto secular = resource->secular_corrections(of::Epoch{31557567816000000LL}, 2, 0);
    if (secular.size() != 1 || secular[0].c != 1e-11 || secular[0].s != 0 ||
        resource->acceleration({7e6, 0, 0}, 2, 0, {}, of::Epoch{31557567816000000LL}) ==
            resource->acceleration({7e6, 0, 0}, 2, 0))
        return 15;
    const auto tide = of::core::solid_tide_corrections(
        resource->info(), {of::core::SolidTideMode::permanent, false}, 0, 0);
    if (tide.size() != 1 || tide.front().c >= 0 ||
        resource->acceleration({7e6, 0, 0}, 0, 0, tide) ==
            resource->acceleration({7e6, 0, 0}, 0, 0))
        return 12;
    numerical.hpop.gravity_model = "custom";
    numerical.hpop.include_secular_variations = true;
    auto gravity_edit = reopened.edit();
    gravity_edit.objects().update(satellite, of::objects::SatelliteUpdate{.orbit = numerical});
    gravity_edit.commit();
    const auto data = of::DataSnapshot::from_providers({}).with_gravity("custom", resource);
    const auto file_analysis = reopened.prepare(data);
    reopened.save(path, file_analysis, true);
    const auto file_project = of::Project::open(path, data);
    if (file_project.to_data().force_data.size() != 1 ||
        file_analysis.states(satellite, 30s, of::FrameId::gcrf).states().size() != 5)
        return 10;
    std::filesystem::remove(path);
    std::filesystem::remove(gravity_file);
} catch (const std::exception &error) {
    std::cerr << "Installed project API failed: " << error.what() << '\n';
    return 11;
}
