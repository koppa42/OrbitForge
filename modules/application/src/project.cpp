#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <orbitforge/application/project.hpp>
#include <picosha2.h>
#include <random>
#include <set>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace orbitforge::application {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
constexpr std::size_t max_file = 64 * 1024 * 1024, max_text = 16384, max_objects = 10000;
constexpr std::array<std::uint8_t, 8> magic{'O', 'R', 'B', 'F', 'P', 'R', 'J', '\n'};
constexpr std::uint32_t project_version = 40;
constexpr std::uint32_t scene_tag = 0x454e4353, objects_tag = 0x534a424f, data_tag = 0x41544144,
                        lock_tag = 0x4b434f4c, stations_tag = 0x4e545347, reports_tag = 0x53545052,
                        styles_tag = 0x4c595453, sensors_tag = 0x52534e53, time_tag = 0x454d4954,
                        analysis_tag = 0x594c4e41;
[[noreturn]] void invalid(const char *message) { throw ProjectError(message); }
void check(bool condition, const char *message) {
    if (!condition)
        invalid(message);
}
struct SectionSchema {
    std::uint32_t tag;
    unsigned version;
    const char *missing_error;
};
// One current schema, shared by the reader and writer. Null means an optional section.
constexpr std::array section_schemas{
    SectionSchema{scene_tag, 1, "Missing scene section"},
    SectionSchema{objects_tag, 31, "Missing satellites section"},
    SectionSchema{styles_tag, 1, "Missing track styles section"},
    SectionSchema{stations_tag, 2, "Missing ground stations section"},
    SectionSchema{sensors_tag, 1, "Missing sensors section"},
    SectionSchema{reports_tag, 2, "Missing reports section"},
    SectionSchema{data_tag, 2, nullptr},
    SectionSchema{time_tag, 1, nullptr},
    SectionSchema{analysis_tag, 1, nullptr},
    SectionSchema{lock_tag, 1, "Missing scenario data lock"}};
const SectionSchema &section_schema(std::uint32_t tag) {
    const auto found = std::ranges::find(section_schemas, tag, &SectionSchema::tag);
    check(found != section_schemas.end(), "Unsupported project section");
    return *found;
}
bool valid_utf8(const std::string &text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c == 0)
            return false;
        if (c < 0x80)
            continue;
        unsigned count = 0, code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            code = c & 0x1f;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            code = c & 0x0f;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            code = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size() - i)
            return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80)
                return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
    }
    return true;
}
void text_check(const std::string &value, bool required = false) {
    check(value.size() <= max_text && valid_utf8(value), "Invalid or oversized UTF-8 project text");
    if (required)
        check(!value.empty() && !std::all_of(value.begin(), value.end(),
                                             [](unsigned char c) { return std::isspace(c) != 0; }),
              "Project names and object IDs must not be blank");
}
bool hash_valid(const std::string &value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xffffffff;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
    }
    return ~crc;
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    void raw(std::span<const std::uint8_t> data) {
        check(data.size() <= max_file - bytes.size(), "Project exceeds 64 MiB");
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void number(std::uint64_t value, unsigned width = 8) {
        std::array<std::uint8_t, 8> data{};
        for (unsigned i = 0; i < width; ++i)
            data[i] = static_cast<std::uint8_t>(value >> (8 * i));
        raw(std::span(data).first(width));
    }
    void text(const std::string &value) {
        text_check(value);
        number(value.size(), 4);
        raw({reinterpret_cast<const std::uint8_t *>(value.data()), value.size()});
    }
    void epoch(core::Epoch value) { number(std::bit_cast<std::uint64_t>(value.tai_nanoseconds())); }
    void real(double value) { number(std::bit_cast<std::uint64_t>(value)); }
    void section(std::uint32_t tag, const Writer &data) {
        number(tag, 4);
        number(section_schema(tag).version, 4);
        number(data.bytes.size());
        number(crc32(data.bytes), 4);
        raw(data.bytes);
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::span<const std::uint8_t> raw(std::size_t length) {
        check(length <= bytes.size(), "Truncated project file");
        const auto result = bytes.first(length);
        bytes = bytes.subspan(length);
        return result;
    }
    std::uint64_t number(unsigned width = 8) {
        const auto data = raw(width);
        std::uint64_t value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(data[i]) << (8 * i);
        return value;
    }
    std::string text() {
        const auto size = number(4);
        check(size <= max_text, "Oversized project string");
        const auto value = raw(static_cast<std::size_t>(size));
        std::string result(reinterpret_cast<const char *>(value.data()), value.size());
        text_check(result);
        return result;
    }
    core::Epoch epoch() { return core::Epoch{std::bit_cast<std::int64_t>(number())}; }
    double real() { return std::bit_cast<double>(number()); }
    bool boolean() {
        const auto value = number(1);
        check(value <= 1, "Invalid project boolean");
        return value != 0;
    }
    void done() const { check(bytes.empty(), "Unexpected trailing project data"); }
};
void write_srp(Writer &out, const core::SrpOptions &o, bool draft = true) {
    out.number(o.enabled, 1);
    if (!draft && !o.enabled)
        return;
    out.number(static_cast<unsigned>(o.shadow_model), 4);
    out.number(static_cast<unsigned>(o.sun_position_method), 4);
    out.number(o.earth_shadow, 1);
    out.number(o.moon_shadow, 1);
    out.number(o.boundary_mitigation, 1);
    for (double value :
         {o.cr, o.area_mass_ratio_m2_per_kg, o.atmospheric_altitude_m, o.pressure_at_reference_pa,
          o.reference_distance_m, o.sun_radius_m, o.earth_radius_m, o.moon_radius_m})
        out.real(value);
    out.number(o.additional_eclipsing_bodies.size(), 4);
    for (const auto &body : o.additional_eclipsing_bodies) {
        out.number(static_cast<unsigned>(body.body), 4);
        out.real(body.radius_m);
    }
}
core::SrpOptions read_srp(Reader &in) {
    core::SrpOptions o;
    o.enabled = in.boolean();
    const auto shadow = in.number(4);
    check(shadow <= 2, "Unsupported SRP shadow model");
    o.shadow_model = static_cast<core::SrpShadowModel>(shadow);
    const auto sun = in.number(4);
    check(sun <= 2, "Unsupported SRP Sun position method");
    o.sun_position_method = static_cast<core::SrpSunPositionMethod>(sun);
    o.earth_shadow = in.boolean();
    o.moon_shadow = in.boolean();
    o.boundary_mitigation = in.boolean();
    o.cr = in.real();
    o.area_mass_ratio_m2_per_kg = in.real();
    o.atmospheric_altitude_m = in.real();
    o.pressure_at_reference_pa = in.real();
    o.reference_distance_m = in.real();
    o.sun_radius_m = in.real();
    o.earth_radius_m = in.real();
    o.moon_radius_m = in.real();
    const auto count = in.number(4);
    check(count <= core::third_body_count - 2, "Too many additional SRP occultors");
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto body = in.number(4);
        check(body >= 2 && body < core::third_body_count, "Invalid additional SRP occultor");
        o.additional_eclipsing_bodies.push_back({static_cast<core::ThirdBody>(body), in.real()});
    }
    try {
        core::validate_srp_options(o);
    } catch (const std::invalid_argument &error) {
        throw ProjectError(error.what());
    }
    return o;
}
void write_drag(Writer &out, const core::DragOptions &d, bool draft = true) {
    out.number(d.enabled, 1);
    if (!draft && !d.enabled)
        return;
    out.real(d.cd);
    out.real(d.area_mass_ratio_m2_per_kg);
    out.number(static_cast<unsigned>(d.atmosphere), 4);
    out.number(static_cast<unsigned>(d.low_altitude_model), 4);
    for (double value : {d.blending_range_m, d.f107, d.f107_average, d.kp})
        out.real(value);
    out.number(d.use_apparent_sun_position, 1);
    out.text(d.weather_model);
    out.number(static_cast<unsigned>(d.geomagnetic_update), 4);
    out.number(static_cast<unsigned>(d.geomagnetic_source), 4);
    out.number(d.use_approximate_altitude, 1);
}
core::DragOptions read_drag(Reader &in) {
    core::DragOptions d;
    d.enabled = in.boolean();
    d.cd = in.real();
    d.area_mass_ratio_m2_per_kg = in.real();
    const auto model = in.number(4), lower = in.number(4);
    check(model <= 1 && lower <= 1, "Unsupported atmospheric density model");
    d.atmosphere = static_cast<core::AtmosphereModel>(model);
    d.low_altitude_model = static_cast<core::LowAltitudeDensityModel>(lower);
    d.blending_range_m = in.real();
    d.f107 = in.real();
    d.f107_average = in.real();
    d.kp = in.real();
    d.use_apparent_sun_position = in.boolean();
    d.weather_model = in.text();
    const auto update = in.number(4), source = in.number(4);
    check(update <= 3 && source <= 1, "Unsupported weather update/source");
    d.geomagnetic_update = static_cast<core::GeomagneticUpdate>(update);
    d.geomagnetic_source = static_cast<core::GeomagneticSource>(source);
    d.use_approximate_altitude = in.boolean();
    return d;
}
void write_mission(Writer &out, const core::MissionPlan &plan, bool names = true) {
    out.number(static_cast<unsigned>(plan.coast_model), 4);
    out.number(plan.segments.size(), 4);
    for (const auto &segment : plan.segments) {
        out.number(static_cast<unsigned>(segment.kind), 4);
        out.text(names ? segment.name : "");
        if (segment.kind == core::MissionSegmentKind::coast) {
            out.number(static_cast<std::uint64_t>(segment.duration_nanoseconds));
        } else {
            out.number(static_cast<unsigned>(segment.frame), 4);
            for (auto component : segment.delta_v_mps)
                out.real(component);
        }
    }
}
core::MissionPlan read_mission(Reader &in) {
    core::MissionPlan result;
    const auto model = in.number(4), count = in.number(4);
    check(model <= 1, "Unsupported mission coast model");
    check(count > 0 && count <= 256, "A mission requires 1..256 segments");
    result.coast_model = static_cast<core::MissionCoastModel>(model);
    for (std::uint64_t i = 0; i < count; ++i) {
        core::MissionSegment segment;
        const auto kind = in.number(4);
        check(kind <= 1, "Unsupported mission segment kind");
        segment.kind = static_cast<core::MissionSegmentKind>(kind);
        segment.name = in.text();
        if (segment.kind == core::MissionSegmentKind::coast) {
            segment.duration_nanoseconds = std::bit_cast<std::int64_t>(in.number());
        } else {
            const auto frame = in.number(4);
            check(frame <= 2, "Unsupported maneuver reference axes");
            segment.frame = static_cast<core::ManeuverFrame>(frame);
            for (auto &component : segment.delta_v_mps)
                component = in.real();
        }
        result.segments.push_back(std::move(segment));
    }
    return result;
}
void write_data(Writer &out, const core::EopDataInfo &data) {
    out.text(data.id);
    out.text(data.sha256);
    out.text(data.source_url);
    out.text(data.reference_frame);
    out.text(data.interpolation);
    out.epoch(data.first_epoch);
    out.epoch(data.last_epoch);
    out.text(data.time_data.id);
    out.text(data.time_data.sha256);
    out.text(data.time_data.source_url);
    out.number(static_cast<std::uint32_t>(data.time_data.utc_valid_from_mjd), 4);
    out.number(static_cast<std::uint32_t>(data.time_data.utc_valid_until_mjd), 4);
}
core::EopDataInfo read_data(Reader &in) {
    core::EopDataInfo result{
        in.text(), in.text(),  in.text(),  in.text(),
        in.text(), in.epoch(), in.epoch(), {in.text(), in.text(), in.text(), 0, 0}};
    result.time_data.utc_valid_from_mjd = static_cast<int>(in.number(4));
    result.time_data.utc_valid_until_mjd = static_cast<int>(in.number(4));
    return result;
}
} // namespace
Project::Project(std::string project_name, core::Epoch begin, core::Epoch end)
    : name(std::move(project_name)), first(begin), last(end), current(begin) {}
DataLock Project::data_lock() const {
    try {
        const auto base = earth_orientation ? eop_data_lock(*earth_orientation)
                          : time_data       ? time_data_lock(*time_data)
                                            : DataLock{};
        std::vector<DataReference> refs(base.references().begin(), base.references().end());
        refs.insert(refs.end(), force_data.begin(), force_data.end());
        return DataLock{std::move(refs)};
    } catch (const std::invalid_argument &error) {
        throw ProjectError(error.what());
    }
}
std::optional<core::TimeDataInfo> Project::time_data_info() const {
    return earth_orientation ? std::optional{earth_orientation->time_data} : time_data;
}
AnalysisStatus Project::analysis_status() const {
    if (!analysis_record)
        return AnalysisStatus::unrecorded;
    return analysis_record->input_sha256 == analysis_input_sha256(*this) &&
                   analysis_record->data_sha256 == data_lock().sha256()
               ? AnalysisStatus::current
               : AnalysisStatus::stale;
}
std::string analysis_input_sha256(const Project &project) {
    Writer out;
    out.text("orbitforge-analysis-input-v1");
    out.epoch(project.first);
    out.epoch(project.last);
    // Collection order is presentation, not a physical dependency.
    std::map<std::string, const ProjectSatellite *> satellites;
    for (const auto &s : project.satellites)
        satellites.emplace(s.orbit.object_id, &s);
    out.number(satellites.size(), 4);
    for (const auto &[id, satellite] : satellites) {
        const auto &orbit = satellite->orbit;
        out.text(id);
        out.number(static_cast<unsigned>(orbit.propagator), 4);
        if (orbit.propagator == OrbitPropagator::mission)
            write_mission(out, orbit.mission, false);
        if (orbit.uses_hpop()) {
            const auto &o = orbit.hpop;
            core::validate_hpop_options(o);
            out.text(o.gravity_model.empty() ? std::string(core::egm96_model_id())
                                             : o.gravity_model);
            out.real(o.gravity_mu);
            for (const auto &body : o.third_bodies) {
                out.number(body.enabled, 1);
                if (!body.enabled)
                    continue;
                out.number(static_cast<unsigned>(body.source), 4);
                if (body.source == core::ThirdBodyGravitySource::user_specified)
                    out.real(*body.user_mu);
                else if (body.source == core::ThirdBodyGravitySource::cb_file) {
                    out.real(body.cb->mu);
                    out.text(body.cb->content_sha256);
                }
            }
            write_srp(out, core::effective_srp_options(o.srp), false);
            // Spherical accelerations depend on A/m. Mass changes only compatible areas.
            // Include mass here when a future mass-dependent model becomes active.
            write_drag(out, core::effective_drag_options(o.drag), false);
            if (o.uses_ephemeris())
                out.text(o.ephemeris_model);
            out.number(static_cast<unsigned>(o.gravity_degree), 4);
            out.number(static_cast<unsigned>(o.gravity_order), 4);
            out.number(o.uses_secular_gravity(), 1);
            if (o.uses_earth_fixed_gravity()) {
                const auto tides =
                    core::effective_solid_tide_options(o.solid_tides, o.gravity_degree);
                out.number(static_cast<unsigned>(tides.mode), 4);
                out.number(tides.truncate_to_gravity, 1);
                out.number(tides.include_time_dependent_terms, 1);
            }
            out.number(o.ocean_tides.active(), 1);
            if (o.ocean_tides.active()) {
                out.text(o.ocean_tides.model);
                out.number(static_cast<unsigned>(o.ocean_tides.degree), 4);
                out.number(static_cast<unsigned>(o.ocean_tides.order), 4);
            }
            out.number(o.include_relativistic_accelerations, 1);
            out.number(static_cast<unsigned>(o.integrator), 4);
            out.number(o.uses_vop(), 1);
            out.real(o.minimum_altitude_m);
            out.number(static_cast<unsigned>(o.interpolation.method), 4);
            out.number(static_cast<unsigned>(o.interpolation.order), 4);
            if (o.interpolation.method == core::OrbitInterpolationMethod::vop)
                out.real(o.interpolation.vop_mu);
            out.number(o.report_ephemeris_on_fixed_step, 1);
            if (o.report_ephemeris_on_fixed_step)
                out.number(static_cast<unsigned>(orbit.step_seconds), 4);
            if (o.integrator == core::HpopIntegrator::gauss_jackson)
                out.number(static_cast<unsigned>(o.correction), 4);
            out.number(o.time_regularization, 1);
            if (o.time_regularization) {
                out.real(o.regularization_exponent);
                out.number(static_cast<unsigned>(o.steps_per_orbit), 4);
            }
            out.number(o.uses_fixed_step(), 1);
            if (o.uses_fixed_step()) {
                if (!o.time_regularization)
                    out.real(o.fixed_step_seconds);
            } else {
                out.real(o.relative_tolerance);
                out.real(o.position_tolerance_m);
                out.real(o.velocity_tolerance_mps);
                out.real(o.min_step_seconds);
                out.real(o.max_step_seconds);
            }
        }
        if (orbit.propagator == OrbitPropagator::sgp4) {
            out.text(orbit.tle.line1);
            out.text(orbit.tle.line2);
        } else {
            out.epoch(orbit.initial.epoch);
            out.number(static_cast<unsigned>(orbit.initial.origin), 4);
            out.number(static_cast<unsigned>(orbit.initial.frame), 4);
            for (const auto value : orbit.initial.position_m)
                out.real(value);
            for (const auto value : orbit.initial.velocity_mps)
                out.real(value);
        }
    }
    std::map<std::string, const ProjectGroundStation *> stations;
    for (const auto &s : project.ground_stations)
        stations.emplace(s.object_id, &s);
    out.number(stations.size(), 4);
    for (const auto &[id, s] : stations) {
        out.text(id);
        out.real(s->location.longitude_rad);
        out.real(s->location.latitude_rad);
        out.real(s->location.height_m);
        out.real(s->minimum_elevation_rad);
    }
    std::map<std::string, const ProjectSensor *> sensors;
    for (const auto &s : project.sensors)
        sensors.emplace(s.object_id, &s);
    out.number(sensors.size(), 4);
    for (const auto &[id, s] : sensors) {
        out.text(id);
        out.text(s->parent_satellite_id);
        out.real(s->half_angle_rad);
        out.number(static_cast<unsigned>(s->pointing), 4);
        out.text(s->pointing == SensorPointing::targeted ? s->target_station_id : "");
    }
    return picosha2::hash256_hex_string(out.bytes);
}
std::pair<core::Epoch, core::Epoch> calculation_data_interval(const Project &project) {
    auto first = project.first, last = project.last;
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        if (!input.uses_eop_force())
            continue;
        if (input.initial.epoch.tai_nanoseconds() < first.tai_nanoseconds())
            first = input.initial.epoch;
        if (input.initial.epoch.tai_nanoseconds() > last.tai_nanoseconds())
            last = input.initial.epoch;
    }
    return {first, last};
}
void validate_project(const Project &project, bool relationships) {
    text_check(project.name, true);
    const auto first = project.first.tai_nanoseconds(), last = project.last.tai_nanoseconds();
    check(first < last && project.last.seconds_since(project.first) < 9e9,
          "Invalid project analysis interval");
    check(project.current.tai_nanoseconds() >= first && project.current.tai_nanoseconds() <= last,
          "Project current time is outside its analysis interval");
    check(project.satellites.size() + project.ground_stations.size() + project.sensors.size() <=
              max_objects,
          "Project exceeds 10000 objects");
    std::set<std::string> ids;
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        text_check(input.object_id, true);
        text_check(satellite.display_name, !satellite.display_name.empty());
        check(ids.insert(input.object_id).second, "Duplicate project object ID");
        check(input.first == project.first && input.last == project.last,
              "Satellite interval must match the project");
        check(input.propagator == OrbitPropagator::two_body ||
                  input.propagator == OrbitPropagator::j2_secular ||
                  input.propagator == OrbitPropagator::j4_secular ||
                  input.propagator == OrbitPropagator::sgp4 ||
                  input.propagator == OrbitPropagator::hpop ||
                  input.propagator == OrbitPropagator::mission,
              "Unsupported project propagator");
        if (input.propagator == OrbitPropagator::sgp4) {
            try {
                (void)core::parse_tle(input.tle);
            } catch (const std::invalid_argument &error) {
                throw ProjectError(error.what());
            }
        } else {
            check(input.tle.line1.empty() && input.tle.line2.empty(),
                  "TLE input is only valid for the SGP4 propagator");
        }
        check(input.initial.origin == core::OriginId::earth &&
                  input.initial.frame == core::FrameId::gcrf &&
                  (input.input_frame == core::FrameId::gcrf ||
                   input.input_frame == core::FrameId::itrf),
              "Unsupported project origin or reference frame");
        for (const auto value : input.initial.position_m)
            check(std::isfinite(value), "Nonfinite project position");
        for (const auto value : input.initial.velocity_mps)
            check(std::isfinite(value), "Nonfinite project velocity");
        check(input.step_seconds >= 1 && input.step_seconds <= 60, "Invalid orbit step");
        if (input.propagator == OrbitPropagator::mission) {
            try {
                const auto end = core::validate_mission(input.initial.epoch, input.mission);
                check(input.initial.epoch.tai_nanoseconds() <= first &&
                          end.tai_nanoseconds() >= last,
                      "Mission sequence must cover the scenario; extend its coast segments");
                for (const auto &segment : input.mission.segments)
                    text_check(segment.name);
            } catch (const std::invalid_argument &error) {
                throw ProjectError(error.what());
            }
        }
        if (input.uses_hpop()) {
            try {
                core::validate_hpop_options(input.hpop);
            } catch (const std::invalid_argument &error) {
                throw ProjectError(error.what());
            }
            check(std::abs(input.first.seconds_since(input.initial.epoch)) <=
                          core::hpop_max_duration_seconds &&
                      std::abs(input.last.seconds_since(input.initial.epoch)) <=
                          core::hpop_max_duration_seconds,
                  "HPOP analysis must stay within 366 days of the initial epoch");
        }
        const auto &display = satellite.display;
        check(display.extent >= 0 && display.extent <= 2 && std::isfinite(display.past_minutes) &&
                  std::isfinite(display.future_minutes) && display.past_minutes >= 0 &&
                  display.future_minutes >= 0 && display.past_minutes <= 10080 &&
                  display.future_minutes <= 10080 &&
                  (display.extent != 2 || display.past_minutes + display.future_minutes > 0),
              "Invalid project track extent");
        check(display.line_color <= 0xffffff && std::isfinite(display.line_width) &&
                  display.line_width >= 0.5 && display.line_width <= 8 &&
                  static_cast<std::uint32_t>(display.line_style) <= 3,
              "Invalid project track style");
        text_check(satellite.model_path);
        check(satellite.model_kind >= 0 && satellite.model_kind <= 2 &&
                  std::isfinite(satellite.model_diameter_m) && satellite.model_diameter_m > 0 &&
                  (satellite.model_kind != 2 || !satellite.model_path.empty()),
              "Invalid project model settings");
    }
    for (const auto &station : project.ground_stations) {
        text_check(station.object_id, true);
        text_check(station.display_name, !station.display_name.empty());
        check(ids.insert(station.object_id).second, "Duplicate project object ID");
        check(std::isfinite(station.minimum_elevation_rad) && station.minimum_elevation_rad >= 0 &&
                  station.minimum_elevation_rad <= std::numbers::pi / 2,
              "Station minimum elevation must be in [0, 90] degrees");
        try {
            (void)core::ground_station_position(station.location);
        } catch (const std::invalid_argument &error) {
            throw ProjectError(error.what());
        }
    }
    for (const auto &sensor : project.sensors) {
        text_check(sensor.object_id, true);
        text_check(sensor.display_name, !sensor.display_name.empty());
        text_check(sensor.parent_satellite_id, true);
        text_check(sensor.target_station_id);
        check(ids.insert(sensor.object_id).second, "Duplicate project object ID");
        check(!relationships || std::ranges::any_of(project.satellites,
                                                    [&](const auto &satellite) {
                                                        return satellite.orbit.object_id ==
                                                               sensor.parent_satellite_id;
                                                    }),
              "Sensor parent satellite does not exist");
        check(std::isfinite(sensor.half_angle_rad) && sensor.half_angle_rad > 0 &&
                  sensor.half_angle_rad <= std::numbers::pi / 2,
              "Sensor cone half angle must be in (0, 90] degrees");
        check(sensor.pointing == SensorPointing::nadir ||
                  sensor.pointing == SensorPointing::targeted,
              "Unsupported sensor pointing");
        check(!relationships || sensor.pointing != SensorPointing::targeted ||
                  std::ranges::any_of(project.ground_stations,
                                      [&](const auto &station) {
                                          return station.object_id == sensor.target_station_id;
                                      }),
              "Sensor target ground station does not exist; reassign the sensor first");
    }
    check(project.reports.size() <= 128, "Project exceeds 128 report definitions");
    std::set<std::string> report_ids;
    for (const auto &report : project.reports) {
        text_check(report.id, true);
        text_check(report.name, true);
        check(report_ids.insert(report.id).second, "Duplicate report ID");
        for (const auto &id : report.query.object_ids)
            text_check(id, true);
        for (const auto &column : report.format.columns) {
            text_check(column.field, true);
            text_check(column.unit);
            text_check(column.title);
        }
        try {
            validate_report_definition(report);
        } catch (const std::invalid_argument &error) {
            throw ProjectError(error.what());
        }
    }
    for (const auto &ref : project.force_data) {
        check(ref.role.starts_with("gravity/") || ref.role.starts_with("ephemeris/") ||
                  ref.role.starts_with("space-weather/") || ref.role.starts_with("ocean-tides/"),
              "Unsupported force-data role");
        text_check(ref.snapshot_id, true);
        text_check(ref.interpretation_version, true);
    }
    (void)project.data_lock();
    if (project.time_data) {
        const auto &data = *project.time_data;
        text_check(data.id, true);
        text_check(data.source_url);
        check(hash_valid(data.sha256), "Invalid time data SHA256");
        check(data.utc_valid_from_mjd >= 0 && data.utc_valid_until_mjd > data.utc_valid_from_mjd,
              "Invalid time data validity interval");
    }
    if (project.analysis_record)
        check(hash_valid(project.analysis_record->input_sha256) &&
                  hash_valid(project.analysis_record->data_sha256),
              "Invalid analysis record SHA256");
    if (project.earth_orientation) {
        const auto &data = *project.earth_orientation;
        for (const auto *value :
             {&data.id, &data.reference_frame, &data.interpolation, &data.time_data.id})
            text_check(*value, true);
        text_check(data.source_url);
        text_check(data.time_data.source_url);
        check(hash_valid(data.sha256) && hash_valid(data.time_data.sha256), "Invalid data SHA256");
        check(core::eop_covers(data, project.first) && core::eop_covers(data, project.last) &&
                  data.time_data.utc_valid_from_mjd >= 0 &&
                  data.time_data.utc_valid_until_mjd > data.time_data.utc_valid_from_mjd,
              "Project data does not cover the analysis interval");
    }
}
bool matching_project_data(const core::EopDataInfo &a, const core::EopDataInfo &b) {
    return a.id == b.id && a.sha256 == b.sha256 && a.reference_frame == b.reference_frame &&
           a.interpolation == b.interpolation && a.first_epoch == b.first_epoch &&
           a.last_epoch == b.last_epoch && a.extrapolation_days == b.extrapolation_days &&
           a.time_data.id == b.time_data.id && a.time_data.sha256 == b.time_data.sha256 &&
           a.time_data.utc_valid_from_mjd == b.time_data.utc_valid_from_mjd &&
           a.time_data.utc_valid_until_mjd == b.time_data.utc_valid_until_mjd;
}
namespace {
std::size_t satellite_index(const Project &project, const std::string &id) {
    const auto found = std::find_if(project.satellites.begin(), project.satellites.end(),
                                    [&](const auto &s) { return s.orbit.object_id == id; });
    check(found != project.satellites.end(), "Project object ID does not exist");
    return static_cast<std::size_t>(found - project.satellites.begin());
}
std::size_t station_index(const Project &project, const std::string &id) {
    const auto found = std::find_if(project.ground_stations.begin(), project.ground_stations.end(),
                                    [&](const auto &s) { return s.object_id == id; });
    check(found != project.ground_stations.end(), "Project ground station ID does not exist");
    return static_cast<std::size_t>(found - project.ground_stations.begin());
}
} // namespace
Project Project::with_settings(std::string new_name, core::Epoch begin, core::Epoch end) const {
    check(begin.tai_nanoseconds() < end.tai_nanoseconds(),
          "Scenario analysis interval must have positive duration");
    auto result = *this;
    result.name = std::move(new_name);
    result.first = begin;
    result.last = end;
    result.current = core::Epoch{
        std::clamp(current.tai_nanoseconds(), begin.tai_nanoseconds(), end.tai_nanoseconds())};
    for (auto &satellite : result.satellites) {
        satellite.orbit.first = begin;
        satellite.orbit.last = end;
    }
    validate_project(result);
    return result;
}
Project Project::renamed_satellite(const std::string &id, std::string new_name) const {
    text_check(new_name, true);
    auto result = *this;
    result.satellites.at(satellite_index(*this, id)).display_name = std::move(new_name);
    validate_project(result);
    return result;
}
Project Project::without_satellite(const std::string &id) const {
    auto result = *this;
    std::erase_if(result.sensors, [&](const auto &s) { return s.parent_satellite_id == id; });
    result.satellites.erase(result.satellites.begin() +
                            static_cast<std::ptrdiff_t>(satellite_index(*this, id)));
    validate_project(result);
    return result;
}
Project Project::with_satellite_copy(const std::string &id, std::string new_id,
                                     std::string new_name) const {
    text_check(new_name, true);
    auto satellite = satellites.at(satellite_index(*this, id));
    satellite.orbit.object_id = std::move(new_id);
    satellite.display_name = std::move(new_name);
    auto result = *this;
    result.satellites.push_back(std::move(satellite));
    for (const auto &sensor : sensors) {
        if (sensor.parent_satellite_id != id)
            continue;
        auto copy = sensor;
        copy.parent_satellite_id = result.satellites.back().orbit.object_id;
        const auto base = copy.parent_satellite_id + "-sensor-";
        unsigned suffix = 1;
        do {
            copy.object_id = base + std::to_string(suffix++);
        } while (
            std::ranges::any_of(result.sensors,
                                [&](const auto &s) { return s.object_id == copy.object_id; }) ||
            std::ranges::any_of(
                result.satellites,
                [&](const auto &s) { return s.orbit.object_id == copy.object_id; }) ||
            std::ranges::any_of(result.ground_stations,
                                [&](const auto &s) { return s.object_id == copy.object_id; }));
        result.sensors.push_back(std::move(copy));
    }
    validate_project(result);
    return result;
}
Project Project::renamed_ground_station(const std::string &id, std::string new_name) const {
    text_check(new_name, true);
    auto result = *this;
    result.ground_stations.at(station_index(*this, id)).display_name = std::move(new_name);
    validate_project(result);
    return result;
}
Project Project::without_ground_station(const std::string &id) const {
    auto result = *this;
    result.ground_stations.erase(result.ground_stations.begin() +
                                 static_cast<std::ptrdiff_t>(station_index(*this, id)));
    validate_project(result);
    return result;
}
Project Project::with_ground_station_copy(const std::string &id, std::string new_id,
                                          std::string new_name) const {
    text_check(new_name, true);
    auto station = ground_stations.at(station_index(*this, id));
    station.object_id = std::move(new_id);
    station.display_name = std::move(new_name);
    auto result = *this;
    result.ground_stations.push_back(std::move(station));
    validate_project(result);
    return result;
}
std::vector<std::uint8_t> encode_project(const Project &project) {
    validate_project(project);
    Writer scene, objects, stations, payload, file;
    scene.text(project.name);
    scene.epoch(project.first);
    scene.epoch(project.last);
    scene.epoch(project.current);
    scene.text(std::string(core::central_body(core::OriginId::earth).model));
    payload.section(scene_tag, scene);
    objects.number(project.satellites.size(), 4);
    for (const auto &satellite : project.satellites) {
        const auto &input = satellite.orbit;
        objects.text(input.object_id);
        objects.text(satellite.display_name);
        objects.number(static_cast<unsigned>(input.propagator) + 1, 4);
        objects.number(input.input_frame == core::FrameId::gcrf ? 1 : 2, 4);
        objects.epoch(input.initial.epoch);
        for (const auto value : input.initial.position_m)
            objects.real(value);
        for (const auto value : input.initial.velocity_mps)
            objects.real(value);
        objects.number(static_cast<unsigned>(input.step_seconds), 4);
        objects.number(static_cast<unsigned>(satellite.display.extent), 4);
        objects.real(satellite.display.past_minutes);
        objects.real(satellite.display.future_minutes);
        objects.number(satellite.display.track_2d, 1);
        objects.number(satellite.display.labels_2d, 1);
        objects.number(satellite.display.track_3d, 1);
        objects.number(static_cast<unsigned>(satellite.model_kind), 4);
        objects.real(satellite.model_diameter_m);
        objects.text(satellite.model_path);
        objects.text(input.tle.line1);
        objects.text(input.tle.line2);
        if (input.propagator == OrbitPropagator::mission)
            write_mission(objects, input.mission);
        if (input.uses_hpop()) {
            const auto &o = input.hpop;
            objects.text(o.gravity_model);
            objects.real(o.gravity_mu);
            objects.text(o.ephemeris_model);
            for (const auto &body : o.third_bodies) {
                objects.number(body.enabled, 1);
                objects.number(static_cast<unsigned>(body.source), 4);
                objects.number(body.user_mu.has_value(), 1);
                if (body.user_mu)
                    objects.real(*body.user_mu);
                objects.number(body.cb.has_value(), 1);
                if (body.cb) {
                    objects.real(body.cb->mu);
                    objects.text(body.cb->name);
                    objects.text(body.cb->content_sha256);
                    objects.number(static_cast<unsigned>(body.cb->body), 4);
                }
            }
            objects.number(static_cast<unsigned>(o.gravity_degree), 4);
            objects.number(static_cast<unsigned>(o.gravity_order), 4);
            objects.number(o.include_secular_variations, 1);
            objects.number(static_cast<unsigned>(o.solid_tides.mode), 4);
            objects.number(o.solid_tides.truncate_to_gravity, 1);
            objects.number(o.solid_tides.include_time_dependent_terms, 1);
            objects.number(o.ocean_tides.enabled, 1);
            objects.text(o.ocean_tides.model);
            objects.number(static_cast<unsigned>(o.ocean_tides.degree), 4);
            objects.number(static_cast<unsigned>(o.ocean_tides.order), 4);
            objects.number(o.include_relativistic_accelerations, 1);
            objects.real(o.satellite_mass_kg);
            objects.number(static_cast<unsigned>(o.integrator), 4);
            objects.number(o.vop, 1);
            objects.number(static_cast<unsigned>(o.correction), 4);
            objects.number(static_cast<unsigned>(o.step_control), 4);
            for (double v : {o.relative_tolerance, o.position_tolerance_m, o.velocity_tolerance_mps,
                             o.min_step_seconds, o.max_step_seconds, o.fixed_step_seconds,
                             o.minimum_altitude_m})
                objects.real(v);
            objects.number(static_cast<unsigned>(o.interpolation.method), 4);
            objects.number(static_cast<unsigned>(o.interpolation.order), 4);
            objects.real(o.interpolation.vop_mu);
            objects.number(o.report_ephemeris_on_fixed_step, 1);
            objects.number(o.time_regularization, 1);
            objects.real(o.regularization_exponent);
            objects.number(static_cast<unsigned>(o.steps_per_orbit), 4);
            write_srp(objects, o.srp);
            write_drag(objects, o.drag);
        }
    }
    payload.section(objects_tag, objects);
    Writer styles;
    styles.number(project.satellites.size(), 4);
    for (const auto &satellite : project.satellites) {
        styles.text(satellite.orbit.object_id);
        styles.number(satellite.display.line_color, 4);
        styles.number(static_cast<std::uint32_t>(satellite.display.line_style), 4);
        styles.real(satellite.display.line_width);
    }
    payload.section(styles_tag, styles);
    stations.number(project.ground_stations.size(), 4);
    for (const auto &station : project.ground_stations) {
        stations.text(station.object_id);
        stations.text(station.display_name);
        stations.real(station.location.longitude_rad);
        stations.real(station.location.latitude_rad);
        stations.real(station.location.height_m);
        stations.real(station.minimum_elevation_rad);
    }
    payload.section(stations_tag, stations);
    Writer sensors;
    sensors.number(project.sensors.size(), 4);
    for (const auto &sensor : project.sensors) {
        sensors.text(sensor.object_id);
        sensors.text(sensor.display_name);
        sensors.text(sensor.parent_satellite_id);
        sensors.real(sensor.half_angle_rad);
        sensors.number(static_cast<unsigned>(sensor.pointing), 4);
        sensors.text(sensor.target_station_id);
    }
    payload.section(sensors_tag, sensors);
    Writer reports;
    reports.number(project.reports.size(), 4);
    for (const auto &report : project.reports) {
        reports.text(report.id);
        reports.text(report.name);
        reports.number(static_cast<unsigned>(report.query.kind), 4);
        reports.number(static_cast<unsigned>(report.query.scope), 4);
        reports.number(report.query.object_ids.size(), 4);
        for (const auto &id : report.query.object_ids)
            reports.text(id);
        reports.number(report.query.follow_scenario, 1);
        reports.epoch(report.query.first);
        reports.epoch(report.query.last);
        reports.number(static_cast<std::uint64_t>(report.query.step_nanoseconds));
        reports.number(report.query.frame == core::FrameId::gcrf ? 1 : 2, 4);
        reports.number(static_cast<unsigned>(report.format.units), 4);
        reports.number(static_cast<unsigned>(report.format.time_scale), 4);
        reports.number(report.format.columns.size(), 4);
        for (const auto &column : report.format.columns) {
            reports.text(column.field);
            reports.text(column.unit);
            reports.text(column.title);
        }
    }
    payload.section(reports_tag, reports);
    if (project.earth_orientation) {
        Writer data;
        write_data(data, *project.earth_orientation);
        data.number(static_cast<unsigned>(project.earth_orientation->extrapolation_days), 4);
        payload.section(data_tag, data);
    }
    if (project.time_data) {
        Writer time;
        time.text(project.time_data->id);
        time.text(project.time_data->sha256);
        time.text(project.time_data->source_url);
        time.number(static_cast<unsigned>(project.time_data->utc_valid_from_mjd), 4);
        time.number(static_cast<unsigned>(project.time_data->utc_valid_until_mjd), 4);
        payload.section(time_tag, time);
    }
    if (project.analysis_record) {
        Writer analysis;
        analysis.text(project.analysis_record->input_sha256);
        analysis.text(project.analysis_record->data_sha256);
        payload.section(analysis_tag, analysis);
    }
    const auto lock = project.data_lock();
    Writer locked;
    locked.raw(lock.canonical_bytes());
    locked.text(lock.sha256());
    payload.section(lock_tag, locked);
    file.raw(magic);
    file.number(project_version, 4);
    file.number(payload.bytes.size());
    file.number(crc32(payload.bytes), 4);
    file.raw(payload.bytes);
    return std::move(file.bytes);
}
Project decode_project(std::span<const std::uint8_t> bytes) {
    check(bytes.size() <= max_file, "Project exceeds 64 MiB");
    Reader file{bytes};
    check(std::ranges::equal(file.raw(magic.size()), magic), "Not an OrbitForge project file");
    const auto version = file.number(4);
    if (version != project_version)
        throw ProjectError("Unsupported OrbitForge project version " + std::to_string(version) +
                           "; this build reads only version " + std::to_string(project_version) +
                           ". Use a matching OrbitForge build or recreate the project.");
    const auto length = file.number();
    const auto checksum = file.number(4);
    check(length == file.bytes.size(), "Project length mismatch");
    check(crc32(file.bytes) == checksum, "Project checksum mismatch");
    std::map<std::uint32_t, std::span<const std::uint8_t>> sections;
    while (!file.bytes.empty()) {
        const auto tag = static_cast<std::uint32_t>(file.number(4));
        const auto &schema = section_schema(tag);
        check(file.number(4) == schema.version, "Unsupported project section version");
        const auto size = file.number();
        const auto section_crc = file.number(4);
        check(size <= file.bytes.size(), "Truncated project section");
        const auto section = file.raw(static_cast<std::size_t>(size));
        check(crc32(section) == section_crc, "Project section checksum mismatch");
        check(sections.emplace(tag, section).second, "Duplicate project section");
    }
    for (const auto &schema : section_schemas)
        if (schema.missing_error)
            check(sections.contains(schema.tag), schema.missing_error);
    Reader scene{sections.at(scene_tag)};
    const auto name = scene.text();
    const auto first = scene.epoch(), last = scene.epoch();
    Project project{name, first, last};
    project.current = scene.epoch();
    check(scene.text() == core::central_body(core::OriginId::earth).model,
          "Unsupported project central-body model");
    scene.done();
    Reader objects{sections.at(objects_tag)};
    const auto count = objects.number(4);
    check(count <= max_objects, "Project exceeds 10000 satellites");
    for (std::uint64_t i = 0; i < count; ++i) {
        ProjectSatellite satellite;
        auto &input = satellite.orbit;
        input.object_id = objects.text();
        satellite.display_name = objects.text();
        const auto propagator = objects.number(4), frame = objects.number(4);
        check(propagator >= 1 && propagator <= 6, "Unsupported project propagator");
        check(frame == 1 || frame == 2, "Unsupported input reference frame");
        input.propagator = static_cast<OrbitPropagator>(propagator - 1);
        input.input_frame = frame == 1 ? core::FrameId::gcrf : core::FrameId::itrf;
        input.initial.epoch = objects.epoch();
        for (auto &value : input.initial.position_m)
            value = objects.real();
        for (auto &value : input.initial.velocity_mps)
            value = objects.real();
        input.first = first;
        input.last = last;
        input.step_seconds = static_cast<int>(objects.number(4));
        satellite.display.extent = static_cast<int>(objects.number(4));
        satellite.display.past_minutes = objects.real();
        satellite.display.future_minutes = objects.real();
        satellite.display.track_2d = objects.boolean();
        satellite.display.labels_2d = objects.boolean();
        satellite.display.track_3d = objects.boolean();
        satellite.model_kind = static_cast<int>(objects.number(4));
        satellite.model_diameter_m = objects.real();
        satellite.model_path = objects.text();
        input.tle = {objects.text(), objects.text()};
        if (input.propagator == OrbitPropagator::mission)
            input.mission = read_mission(objects);
        if (input.uses_hpop()) {
            auto &o = input.hpop;
            o.gravity_model = objects.text();
            o.gravity_mu = objects.real();
            o.ephemeris_model = objects.text();
            for (auto &body : o.third_bodies) {
                body.enabled = objects.boolean();
                const auto source = objects.number(4);
                check(source <= 2, "Unsupported third-body gravity source");
                body.source = static_cast<core::ThirdBodyGravitySource>(source);
                if (objects.boolean())
                    body.user_mu = objects.real();
                if (objects.boolean()) {
                    body.cb = core::CbGravity{objects.real(), objects.text(), objects.text()};
                    const auto cb_body = objects.number(4);
                    check(cb_body < core::third_body_count, "Unsupported Cb file body");
                    body.cb->body = static_cast<core::ThirdBody>(cb_body);
                }
            }
            o.gravity_degree = static_cast<int>(objects.number(4));
            o.gravity_order = static_cast<int>(objects.number(4));
            o.include_secular_variations = objects.boolean();
            const auto tide_mode = objects.number(4);
            check(tide_mode <= 2, "Unsupported solid tide mode");
            o.solid_tides.mode = static_cast<core::SolidTideMode>(tide_mode);
            o.solid_tides.truncate_to_gravity = objects.boolean();
            o.solid_tides.include_time_dependent_terms = objects.boolean();
            o.ocean_tides.enabled = objects.boolean();
            o.ocean_tides.model = objects.text();
            o.ocean_tides.degree = static_cast<int>(objects.number(4));
            o.ocean_tides.order = static_cast<int>(objects.number(4));
            o.include_relativistic_accelerations = objects.boolean();
            o.satellite_mass_kg = objects.real();
            const auto integrator = objects.number(4);
            check(integrator <= 4, "Unsupported HPOP integrator");
            o.integrator = static_cast<core::HpopIntegrator>(integrator);
            o.vop = objects.boolean();
            const auto correction = objects.number(4);
            check(correction <= 1, "Unsupported HPOP predictor corrector scheme");
            o.correction = static_cast<core::HpopCorrection>(correction);
            const auto step_control = objects.number(4);
            check(step_control <= 1, "Unsupported HPOP step size control");
            o.step_control = static_cast<core::HpopStepControl>(step_control);
            o.relative_tolerance = objects.real();
            o.position_tolerance_m = objects.real();
            o.velocity_tolerance_mps = objects.real();
            o.min_step_seconds = objects.real();
            o.max_step_seconds = objects.real();
            o.fixed_step_seconds = objects.real();
            o.minimum_altitude_m = objects.real();
            const auto interpolation = objects.number(4);
            check(interpolation <= 2, "Unsupported HPOP interpolation method");
            o.interpolation.method = static_cast<core::OrbitInterpolationMethod>(interpolation);
            const auto interpolation_order = objects.number(4);
            check(interpolation_order <= 29, "Unsupported HPOP interpolation order");
            o.interpolation.order = static_cast<int>(interpolation_order);
            o.interpolation.vop_mu = objects.real();
            o.report_ephemeris_on_fixed_step = objects.boolean();
            o.time_regularization = objects.boolean();
            o.regularization_exponent = objects.real();
            const auto steps_per_orbit = objects.number(4);
            check(steps_per_orbit >= 1 &&
                      steps_per_orbit <= static_cast<unsigned>(std::numeric_limits<int>::max()),
                  "HPOP steps per orbit must be a positive int");
            o.steps_per_orbit = static_cast<int>(steps_per_orbit);
            o.srp = read_srp(objects);
            o.drag = read_drag(objects);
        }
        project.satellites.push_back(std::move(satellite));
    }
    objects.done();
    {
        Reader styles{sections.at(styles_tag)};
        check(styles.number(4) == count, "Track styles must match the satellite count");
        std::map<std::string, OrbitDisplay *> displays;
        for (auto &satellite : project.satellites)
            check(displays.emplace(satellite.orbit.object_id, &satellite.display).second,
                  "Duplicate project object ID");
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto found = displays.find(styles.text());
            check(found != displays.end(), "Unknown or duplicate track style object ID");
            auto &display = *found->second;
            display.line_color = static_cast<std::uint32_t>(styles.number(4));
            display.line_style = static_cast<LineStyle>(styles.number(4));
            display.line_width = styles.real();
            displays.erase(found);
        }
        styles.done();
    }
    {
        Reader stations{sections.at(stations_tag)};
        const auto station_count = stations.number(4);
        check(station_count + count <= max_objects, "Project exceeds 10000 objects");
        for (std::uint64_t i = 0; i < station_count; ++i) {
            ProjectGroundStation station;
            station.object_id = stations.text();
            station.display_name = stations.text();
            station.location = {stations.real(), stations.real(), stations.real()};
            station.minimum_elevation_rad = stations.real();
            project.ground_stations.push_back(std::move(station));
        }
        stations.done();
    }
    {
        Reader sensors{sections.at(sensors_tag)};
        const auto sensor_count = sensors.number(4);
        check(sensor_count + project.satellites.size() + project.ground_stations.size() <=
                  max_objects,
              "Project exceeds 10000 objects");
        for (std::uint64_t i = 0; i < sensor_count; ++i) {
            ProjectSensor sensor;
            sensor.object_id = sensors.text();
            sensor.display_name = sensors.text();
            sensor.parent_satellite_id = sensors.text();
            sensor.half_angle_rad = sensors.real();
            const auto pointing = sensors.number(4);
            check(pointing <= 1, "Unsupported sensor pointing");
            sensor.pointing = static_cast<SensorPointing>(pointing);
            sensor.target_station_id = sensors.text();
            project.sensors.push_back(std::move(sensor));
        }
        sensors.done();
    }
    {
        Reader reports{sections.at(reports_tag)};
        const auto report_count = reports.number(4);
        check(report_count <= 128, "Project exceeds 128 report definitions");
        for (std::uint64_t i = 0; i < report_count; ++i) {
            ReportDefinition report;
            report.id = reports.text();
            report.name = reports.text();
            const auto kind = reports.number(4);
            check(kind <= 8, "Unsupported report template");
            report.query.kind = static_cast<ReportKind>(kind);
            const auto scope = reports.number(4);
            check(scope <= 1, "Unsupported report scope");
            report.query.scope = static_cast<ReportScope>(scope);
            const auto bindings = reports.number(4);
            check(bindings <= max_objects, "Report exceeds 10000 object bindings");
            for (std::uint64_t j = 0; j < bindings; ++j)
                report.query.object_ids.push_back(reports.text());
            report.query.follow_scenario = reports.boolean();
            report.query.first = reports.epoch();
            report.query.last = reports.epoch();
            const auto step = reports.number();
            check(step >= 1 && step <= 86400ULL * 1'000'000'000, "Invalid report step");
            report.query.step_nanoseconds = static_cast<std::int64_t>(step);
            const auto frame = reports.number(4), units = reports.number(4),
                       scale = reports.number(4);
            check((frame == 1 || frame == 2) && units <= 1 && scale <= 2,
                  "Unsupported report format");
            report.query.frame = frame == 1 ? core::FrameId::gcrf : core::FrameId::itrf;
            report.format.units = static_cast<ReportUnits>(units);
            report.format.time_scale = static_cast<ReportTimeScale>(scale);
            const auto columns = reports.number(4);
            check(columns <= 64, "Report exceeds 64 display columns");
            for (std::uint64_t j = 0; j < columns; ++j)
                report.format.columns.push_back({reports.text(), reports.text(), reports.text()});
            project.reports.push_back(std::move(report));
        }
        reports.done();
    }
    if (sections.contains(data_tag)) {
        Reader data{sections.at(data_tag)};
        project.earth_orientation = read_data(data);
        const auto days = data.number(4);
        check(days <= core::max_eop_extrapolation_days, "Invalid EOP extrapolation limit");
        project.earth_orientation->extrapolation_days = static_cast<int>(days);
        data.done();
    }
    if (sections.contains(time_tag)) {
        Reader time{sections.at(time_tag)};
        core::TimeDataInfo info{time.text(), time.text(), time.text(), 0, 0};
        const auto from = time.number(4), until = time.number(4);
        check(from <= std::numeric_limits<int>::max() && until <= std::numeric_limits<int>::max(),
              "Invalid time data validity interval");
        info.utc_valid_from_mjd = static_cast<int>(from);
        info.utc_valid_until_mjd = static_cast<int>(until);
        project.time_data = std::move(info);
        time.done();
    }
    if (sections.contains(analysis_tag)) {
        Reader analysis{sections.at(analysis_tag)};
        project.analysis_record = AnalysisRecord{analysis.text(), analysis.text()};
        analysis.done();
    }
    validate_project(project);
    {
        Reader locked{sections.at(lock_tag)};
        check(locked.text() == "orbitforge-data-lock-v1", "Unsupported data lock encoding");
        const auto reference_count = locked.number(4);
        check(reference_count <= 64, "Data lock exceeds 64 references");
        std::vector<DataReference> references;
        for (std::uint64_t i = 0; i < reference_count; ++i)
            references.push_back(
                {locked.text(), locked.text(), locked.text(), locked.text(), locked.text()});
        try {
            const DataLock lock(std::move(references));
            for (const auto &ref : lock.references())
                if (ref.role.starts_with("gravity/") || ref.role.starts_with("ephemeris/") ||
                    ref.role.starts_with("space-weather/") || ref.role.starts_with("ocean-tides/"))
                    project.force_data.push_back(ref);
            check(lock.sha256() == locked.text(), "Scenario data lock SHA256 mismatch");
            check(lock == project.data_lock(),
                  "Scenario data references disagree with project metadata");
        } catch (const std::invalid_argument &error) {
            throw ProjectError(error.what());
        }
        locked.done();
    }
    validate_project(project);
    return project;
}
Project load_project(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    check(input.is_open(), "Cannot open project file");
    const auto size = input.tellg();
    check(size >= 0 && size <= static_cast<std::streamoff>(max_file), "Invalid project file size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(input), "Cannot read complete project file");
    check(input.peek() == std::char_traits<char>::eof(), "Project changed while reading");
    return decode_project(bytes);
}
void save_project(const std::filesystem::path &path, const Project &project, bool overwrite) {
    const auto bytes = encode_project(project);
    const auto target = std::filesystem::absolute(path);
    check(!target.filename().empty(), "A project filename is required");
    std::filesystem::path temporary;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate = target.parent_path() / (".orbitforge-save-" + std::to_string(random()) +
                                                 "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            temporary = std::move(candidate);
            break;
        }
    }
    check(!temporary.empty(), "Cannot create project temporary directory");
    const auto staging = temporary / "project";
    const auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        std::filesystem::remove(temporary, ignored);
    };
    try {
        std::ofstream output(staging, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        check(static_cast<bool>(output), "Cannot write complete project file");
#ifdef _WIN32
        const auto handle = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        check(handle != INVALID_HANDLE_VALUE, "Cannot flush project file");
        const bool flushed = FlushFileBuffers(handle) != 0;
        CloseHandle(handle);
        check(flushed, "Cannot flush project file");
        check(MoveFileExW(staging.c_str(), target.c_str(),
                          MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)) !=
                  0,
              "Cannot publish project file (destination exists, is locked, or is not writable)");
#else
        const auto descriptor = ::open(staging.c_str(), O_RDONLY);
        check(descriptor >= 0, "Cannot flush project file");
        const auto flushed = ::fsync(descriptor);
        ::close(descriptor);
        check(flushed == 0, "Cannot flush project file");
        if (overwrite)
            std::filesystem::rename(staging, target);
        else
            check(::link(staging.c_str(), target.c_str()) == 0,
                  "Cannot publish project file (destination exists or is not writable)");
#endif
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
} // namespace orbitforge::application
