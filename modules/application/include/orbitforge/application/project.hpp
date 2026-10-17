#pragma once
#include <filesystem>
#include <optional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/application/report.hpp>
#include <orbitforge/core/frames/eop_provider.hpp>
#include <orbitforge/core/frames/state.hpp>
#include <orbitforge/core/propagation/central_body.hpp>
#include <orbitforge/core/propagation/ground_station.hpp>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/core/propagation/j4_mean_elements.hpp>
#include <orbitforge/core/propagation/tle.hpp>
#include <span>
#include <vector>

namespace orbitforge::application {
enum class OrbitPropagator { two_body, j2_secular, j4_secular, sgp4, hpop, mission };
[[nodiscard]] inline std::string_view propagator_name(OrbitPropagator propagator) {
    switch (propagator) {
    case OrbitPropagator::two_body:
        return "Two body";
    case OrbitPropagator::j2_secular:
        return "J2 secular";
    case OrbitPropagator::j4_secular:
        return "J4 secular";
    case OrbitPropagator::sgp4:
        return "SGP4 / TLE";
    case OrbitPropagator::hpop:
        return "HPOP / numerical";
    case OrbitPropagator::mission:
        return "Mission sequence";
    }
    throw std::invalid_argument("Unsupported orbit propagator");
}
// Canonical GCRF input. J2/J4 store the auxiliary mean ellipse, not an osculating
// state. Each standard model includes all three secular terms. SGP4 uses tle.
struct OrbitInput {
    core::CartesianState initial{
        core::Epoch{0}, core::OriginId::earth, core::FrameId::gcrf, {7e6, 0, 0}, {0, 6200, 5300}};
    OrbitPropagator propagator = OrbitPropagator::two_body;
    core::FrameId input_frame = core::FrameId::gcrf;
    // SGP4 uses only these two lines for its orbit; initial is the inactive manual draft.
    // TLE epoch is interpreted with the calculation's explicit time provider.
    core::Tle tle;
    core::HpopOptions hpop;
    core::MissionPlan mission;
    [[nodiscard]] bool uses_hpop() const {
        return propagator == OrbitPropagator::hpop ||
               (propagator == OrbitPropagator::mission &&
                mission.coast_model == core::MissionCoastModel::hpop);
    }
    [[nodiscard]] bool uses_eop_force() const { return uses_hpop() && hpop.uses_eop_force(); }
    [[nodiscard]] double mu() const {
        if (uses_hpop())
            return hpop.gravity_mu;
        if (propagator == OrbitPropagator::j4_secular)
            return core::j4_earth().mu.metres_cubed_per_second_squared;
        if (propagator == OrbitPropagator::sgp4)
            return 3.986008e14; // WGS72, matching the TLE propagation model.
        return core::central_body(initial.origin).mu.metres_cubed_per_second_squared;
    }
    std::string object_id = "satellite-1";
    core::Epoch first{0}, last{7'200'000'000'000};
    // Display sampling; also the scientific ephemeris grid when HPOP fixed reporting is enabled.
    // Currently whole SI seconds in [1, 60], independent of report/query sampling.
    int step_seconds = 60;
};
enum class LineStyle : std::uint32_t { solid, dashed, dotted, dash_dot };
struct OrbitDisplay {
    // 0 = one period, 1 = scenario, 2 = custom trail/lead.
    int extent = 0;
    double past_minutes = 45, future_minutes = 45;
    bool track_2d = true, labels_2d = true, track_3d = true;
    // Opaque sRGB, 0xRRGGBB. Width is in logical screen pixels in both views.
    std::uint32_t line_color = 0x73cdee;
    double line_width = 1.5;
    LineStyle line_style = LineStyle::solid;
    bool operator==(const OrbitDisplay &) const = default;
};
struct ProjectSatellite {
    OrbitInput orbit;
    // Empty means use the stable object ID.
    std::string display_name;
    [[nodiscard]] const std::string &name() const {
        return display_name.empty() ? orbit.object_id : display_name;
    }
    OrbitDisplay display;
    // 0 = point, 1 = built-in schematic, 2 = external model.
    int model_kind = 0;
    double model_diameter_m = 1'000'000;
    // UTF-8 path, relative to the project file (or absolute). External assets
    // remain external; a missing asset must not silently change the project.
    std::string model_path;
};
struct ProjectGroundStation {
    std::string object_id = "ground-station-1";
    std::string display_name;
    core::GroundStationLocation location;
    double minimum_elevation_rad = 0; // Geodetic horizon, [0, pi/2].
    [[nodiscard]] const std::string &name() const {
        return display_name.empty() ? object_id : display_name;
    }
};
enum class SensorPointing { nadir, targeted };
struct ProjectSensor {
    std::string object_id = "sensor-1", display_name, parent_satellite_id;
    double half_angle_rad = 0.7853981633974483;      // Simple conic, (0, pi/2].
    SensorPointing pointing = SensorPointing::nadir; // Geocentric nadir.
    std::string target_station_id; // Required for ideal instantaneous targeted pointing.
    [[nodiscard]] const std::string &name() const {
        return display_name.empty() ? object_id : display_name;
    }
};
// A successfully prepared analysis, not a cache of calculated results.
struct AnalysisRecord {
    std::string input_sha256, data_sha256;
    bool operator==(const AnalysisRecord &) const = default;
};
enum class AnalysisStatus { unrecorded, current, stale };
struct Project {
    Project(std::string name, core::Epoch first, core::Epoch last);
    std::string name;
    core::Epoch first, last, current;
    std::vector<ProjectSatellite> satellites;
    std::vector<ProjectGroundStation> ground_stations;
    std::vector<ProjectSensor> sensors;
    std::vector<ReportDefinition> reports;
    // Provenance of the last calculation, not a requirement to load an old file.
    // The desktop checks current data/policy and requires consent for changes.
    std::optional<core::EopDataInfo> earth_orientation;
    // Standalone UTC provenance when EOP is absent. EOP carries its own UTC identity.
    std::optional<core::TimeDataInfo> time_data;
    std::vector<DataReference> force_data;
    std::optional<AnalysisRecord> analysis_record;
    [[nodiscard]] std::optional<core::TimeDataInfo> time_data_info() const;
    [[nodiscard]] AnalysisStatus analysis_status() const;
    [[nodiscard]] DataLock data_lock() const;
    // Updates all object analysis bounds, preserving initial epochs and display settings.
    // Current time is clamped to the new interval. Attached data must cover it;
    // explicitly replace earth_orientation first when selecting different data.
    [[nodiscard]] Project with_settings(std::string name, core::Epoch first,
                                        core::Epoch last) const;
    [[nodiscard]] Project renamed_satellite(const std::string &id, std::string name) const;
    [[nodiscard]] Project without_satellite(const std::string &id) const;
    [[nodiscard]] Project with_satellite_copy(const std::string &id, std::string new_id,
                                              std::string name) const;
    [[nodiscard]] Project renamed_ground_station(const std::string &id, std::string name) const;
    [[nodiscard]] Project without_ground_station(const std::string &id) const;
    [[nodiscard]] Project with_ground_station_copy(const std::string &id, std::string new_id,
                                                   std::string name) const;
};
class ProjectError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
// Editors may temporarily leave references unresolved in a private candidate.
// Published projects and files must always validate with relationships enabled.
void validate_project(const Project &project, bool relationships = true);
// Includes the path from each noncentral HPOP initial epoch to the scenario.
[[nodiscard]] std::pair<core::Epoch, core::Epoch> calculation_data_interval(const Project &project);
// Physical inputs, stable IDs and relationships; excludes names, appearance,
// reports, playback time, inactive orbit drafts and data provenance.
[[nodiscard]] std::string analysis_input_sha256(const Project &project);
[[nodiscard]] bool matching_project_data(const core::EopDataInfo &a, const core::EopDataInfo &b);
// Versioned, little-endian .orbitforge container. No Qt or provider dependency.
[[nodiscard]] std::vector<std::uint8_t> encode_project(const Project &project);
[[nodiscard]] Project decode_project(std::span<const std::uint8_t> bytes);
[[nodiscard]] Project load_project(const std::filesystem::path &path);
// Writes a sibling temporary file, then atomically publishes it. Failure before
// publication leaves an existing destination intact. Does not create parents.
void save_project(const std::filesystem::path &path, const Project &project,
                  bool overwrite = false);
} // namespace orbitforge::application
