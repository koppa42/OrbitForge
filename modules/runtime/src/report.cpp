#include "report_execution.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numbers>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <picosha2.h>
#include <random>
#include <set>
#include <sstream>
#include <system_error>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

namespace orbitforge::runtime {
namespace {
using namespace application;
using results::Column;
using results::Row;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::invalid_argument(message);
}
void cancelled(const std::atomic_bool *value) {
    if (value && value->load())
        throw ReportCancelled{};
}
using Selection = detail::ReportObject;
std::vector<Selection> select(const Project &project, const ReportDefinition &d) {
    std::vector<Selection> values;
    std::set<std::string> remaining(d.query.object_ids.begin(), d.query.object_ids.end());
    const auto wanted = [&](const std::string &id) {
        return d.query.scope == ReportScope::all_compatible || remaining.erase(id) != 0;
    };
    if (d.query.kind != ReportKind::ground_stations)
        for (const auto &s : project.satellites)
            if (wanted(s.orbit.object_id))
                values.push_back({s.orbit.object_id, s.name(), "Satellite", nullptr, &s});
    if (!report_satellites_only(d.query.kind))
        for (const auto &s : project.ground_stations)
            if (wanted(s.object_id))
                values.push_back({s.object_id, s.name(), "Ground station", &s});
    check(remaining.empty(), "A report object is missing or incompatible with this template");
    return values;
}
std::pair<core::Epoch, core::Epoch> interval(const Project &p, const ReportDefinition &d) {
    if (!report_is_sampled(d.query.kind) || d.query.follow_scenario)
        return {p.first, p.last};
    check(d.query.first.tai_nanoseconds() >= p.first.tai_nanoseconds() &&
              d.query.last.tai_nanoseconds() <= p.last.tai_nanoseconds(),
          "Report interval is outside the scenario");
    return {d.query.first, d.query.last};
}
std::uint64_t ticks_between(core::Epoch first, core::Epoch last) {
    return static_cast<std::uint64_t>(last.tai_nanoseconds()) -
           static_cast<std::uint64_t>(first.tai_nanoseconds());
}
std::string epoch_text(core::Epoch epoch, ReportTimeScale scale, const core::ITimeProvider *time) {
    core::UtcDateTime value{};
    if (scale == ReportTimeScale::utc) {
        check(time != nullptr, "UTC report requires a time provider");
        value = time->to_utc(epoch);
    } else {
        constexpr std::int64_t day = 86400LL * 1'000'000'000;
        auto days = epoch.tai_nanoseconds() / day;
        auto within = epoch.tai_nanoseconds() % day + 43200LL * 1'000'000'000;
        if (scale == ReportTimeScale::tt)
            within += 32'184'000'000;
        if (within < 0) {
            within += day;
            --days;
        }
        if (within >= day) {
            within -= day;
            ++days;
        }
        const std::chrono::year_month_day date{
            std::chrono::sys_days{std::chrono::year{2000} / 1 / 1} + std::chrono::days{days}};
        value = {static_cast<int>(date.year()),
                 static_cast<int>(static_cast<unsigned>(date.month())),
                 static_cast<int>(static_cast<unsigned>(date.day())),
                 static_cast<int>(within / 3'600'000'000'000),
                 static_cast<int>(within / 60'000'000'000 % 60),
                 static_cast<int>(within / 1'000'000'000 % 60),
                 static_cast<int>(within % 1'000'000'000)};
    }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setfill('0') << std::setw(4) << value.year << '-' << std::setw(2) << value.month
        << '-' << std::setw(2) << value.day << 'T' << std::setw(2) << value.hour << ':'
        << std::setw(2) << value.minute << ':' << std::setw(2) << value.second << '.'
        << std::setw(9) << value.nanosecond;
    if (scale == ReportTimeScale::utc)
        out << 'Z';
    return out.str();
}
std::string unit(const ReportDefinition &d, std::string value) {
    if (value == "epoch")
        return d.format.time_scale == ReportTimeScale::utc   ? "UTC"
               : d.format.time_scale == ReportTimeScale::tai ? "TAI"
                                                             : "TT";
    if (d.format.units == ReportUnits::engineering) {
        if (value == "m")
            return "km";
        if (value == "m/s")
            return "km/s";
        if (value == "rad")
            return "deg";
    }
    return value;
}
void csv_field(std::ostream &out, const std::string &value) {
    out << '"';
    for (const char c : value) {
        if (c == '"')
            out << '"';
        out << c;
    }
    out << '"';
}
} // namespace
core::HpopEnvironment ReportContext::force_environment(const application::OrbitInput &input) const {
    core::HpopEnvironment environment{frames, eop};
    environment.time = time;
    if (!input.uses_hpop())
        return environment;
    if (!input.hpop.gravity_model.empty()) {
        const auto found = gravity.find(input.hpop.gravity_model);
        check(found != gravity.end() && found->second, "Missing selected gravity provider");
        validate_gravity_reference(project.force_data, input.hpop.gravity_model,
                                   found->second->info());
        environment.gravity = found->second;
    }
    if (input.hpop.uses_ephemeris()) {
        const auto found = ephemerides.find(input.hpop.ephemeris_model);
        check(found != ephemerides.end() && found->second, "Missing selected ephemeris provider");
        validate_ephemeris_reference(project.force_data, input.hpop.ephemeris_model,
                                     found->second->data_info());
        environment.ephemeris = found->second;
    }
    if (input.hpop.drag.enabled && !input.hpop.drag.weather_model.empty()) {
        const auto found = weather.find(input.hpop.drag.weather_model);
        check(found != weather.end() && found->second, "Missing selected weather provider");
        validate_space_weather_reference(project.force_data, input.hpop.drag.weather_model,
                                         found->second->info());
        environment.weather = found->second;
    }
    if (input.hpop.ocean_tides.active()) {
        const auto found = ocean_tides.find(input.hpop.ocean_tides.model);
        check(found != ocean_tides.end() && found->second, "Missing selected ocean tide model");
        validate_ocean_tide_reference(project.force_data, input.hpop.ocean_tides.model,
                                      found->second->info());
        environment.ocean_tides = found->second;
    }
    return environment;
}
std::string report_input_sha256(const application::Project &project) {
    auto p = project;
    p.reports.clear();
    p.current = p.first;
    for (auto &s : p.satellites) {
        s.display = {};
        s.model_kind = 0;
        s.model_path.clear();
        s.model_diameter_m = 1'000'000;
        s.orbit.input_frame = core::FrameId::gcrf;
        if (!s.orbit.uses_hpop() || !s.orbit.hpop.report_ephemeris_on_fixed_step)
            s.orbit.step_seconds = 60;
        if (s.orbit.propagator == OrbitPropagator::sgp4)
            s.orbit.initial = OrbitInput{}.initial; // Inactive manual draft is not a TLE input.
        for (auto &segment : s.orbit.mission.segments)
            segment.name.clear();
        if (s.orbit.uses_hpop()) {
            // Persist both integrators' drafts, but identify only the active calculation.
            auto &options = s.orbit.hpop;
            core::validate_hpop_options(options);
            // Spherical accelerations use explicit A/m, independent of compatible area and mass.
            options.satellite_mass_kg = core::HpopOptions{}.satellite_mass_kg;
            if (options.interpolation.method != core::OrbitInterpolationMethod::vop)
                options.interpolation.vop_mu = core::OrbitInterpolationOptions{}.vop_mu;
            for (auto &body : options.third_bodies) {
                if (!body.enabled)
                    body = {};
                else {
                    if (body.source != core::ThirdBodyGravitySource::user_specified)
                        body.user_mu.reset();
                    if (body.source != core::ThirdBodyGravitySource::cb_file)
                        body.cb.reset();
                    else if (body.cb)
                        body.cb->name = "Cb file"; // Display name is not a calculation input.
                }
            }
            options.vop = options.uses_vop();
            options.include_secular_variations = options.uses_secular_gravity();
            options.solid_tides =
                core::effective_solid_tide_options(options.solid_tides, options.gravity_degree);
            if (!options.ocean_tides.active())
                options.ocean_tides = {};
            options.srp = core::effective_srp_options(options.srp);
            options.drag = core::effective_drag_options(options.drag);
            if (!options.uses_ephemeris())
                options.ephemeris_model = core::HpopOptions{}.ephemeris_model;
            if (options.integrator != core::HpopIntegrator::gauss_jackson)
                options.correction = core::HpopCorrection::full;
            options.step_control = options.uses_fixed_step()
                                       ? core::HpopStepControl::fixed
                                       : core::HpopStepControl::relative_error;
            const core::HpopOptions defaults;
            if (!options.time_regularization) {
                options.regularization_exponent = defaults.regularization_exponent;
                options.steps_per_orbit = defaults.steps_per_orbit;
            }
            if (options.time_regularization || !options.uses_fixed_step())
                options.fixed_step_seconds = defaults.fixed_step_seconds;
            if (options.uses_fixed_step()) {
                options.relative_tolerance = defaults.relative_tolerance;
                options.position_tolerance_m = defaults.position_tolerance_m;
                options.velocity_tolerance_mps = defaults.velocity_tolerance_mps;
                options.min_step_seconds = defaults.min_step_seconds;
                options.max_step_seconds = defaults.max_step_seconds;
            }
        }
    }
    const auto bytes = application::encode_project(p);
    return picosha2::hash256_hex_string(bytes);
}
std::size_t estimate_report_rows(const Project &p, const ReportDefinition &d) {
    validate_project(p);
    validate_report_definition(d);
    const auto selected = report_is_access(d.query.kind) ? std::vector<Selection>{} : select(p, d);
    if (report_is_access(d.query.kind))
        validate_access_pair(p, d.query.object_ids);
    if (d.query.kind == ReportKind::scenario_summary)
        return 7;
    check(report_is_access(d.query.kind) || !selected.empty(),
          "This report has no compatible objects");
    if (!report_is_sampled(d.query.kind))
        return selected.size();
    const auto [first, last] = interval(p, d);
    const auto span = ticks_between(first, last),
               step = static_cast<std::uint64_t>(d.query.step_nanoseconds);
    // Include start and exact stop, appending one shorter final interval when necessary.
    check(span / step < max_report_rows,
          "Report exceeds 200000 rows; increase the step or shorten the interval");
    const auto count = span / step + 1 + (span % step != 0 ? 1 : 0);
    const auto objects = report_is_access(d.query.kind) ? 1 : selected.size();
    check(count <= max_report_rows / objects,
          "Report exceeds 200000 rows; select fewer objects or increase the step");
    return static_cast<std::size_t>(count * objects);
}
ReportResult generate_report(const ReportContext &context, const ReportDefinition &d,
                             const std::atomic_bool *cancel,
                             const std::function<void(std::size_t, std::size_t)> &progress) {
    const detail::ReportExecution execution{
        [&](const detail::ReportObject &object, core::FrameId) {
            return object.station
                       ? make_ground_station_session(*object.station)
                       : make_orbit_session(object.satellite->orbit, 1, context.time.get(),
                                            context.force_environment(object.satellite->orbit));
        },
        [&](const std::vector<std::string> &ids) {
            AccessSession session{context, ids[0], ids[1]};
            return detail::ReportAccess{session.name(), [session, cancel](core::Epoch epoch) {
                                            return session.at(epoch,
                                                              [cancel] { cancelled(cancel); });
                                        }};
        },
        [&] { cancelled(cancel); }};
    return detail::generate_report(context, d, execution, progress);
}
ReportResult
detail::generate_report(const ReportContext &context, const ReportDefinition &d,
                        const ReportExecution &execution,
                        const std::function<void(std::size_t, std::size_t)> &progress) {
    execution.check_cancelled();
    const auto total = estimate_report_rows(context.project, d);
    const auto [first, last] = interval(context.project, d);
    if (report_uses_utc(d.format, d.query.kind)) {
        (void)epoch_text(first, ReportTimeScale::utc, context.time.get());
        (void)epoch_text(last, ReportTimeScale::utc, context.time.get());
    }
    if (context.project.earth_orientation && context.time)
        check(context.time->data_info().sha256 ==
                  context.project.earth_orientation->time_data.sha256,
              "Report time provider differs from the confirmed project data");
    if (context.eop) {
        check(
            context.project.earth_orientation &&
                matching_project_data(*context.project.earth_orientation, context.eop->data_info()),
            "Report EOP provider differs from the confirmed project data");
        check(context.time &&
                  context.time->data_info().sha256 == context.eop->data_info().time_data.sha256,
              "Report time and EOP providers must use the same UTC data");
    }
    auto columns = report_columns(d.query.kind);
    std::vector<Row> rows;
    rows.reserve(total);
    const auto selected =
        report_is_access(d.query.kind) ? std::vector<Selection>{} : select(context.project, d);
    const auto tick = [&] {
        execution.check_cancelled();
        if (progress)
            progress(rows.size(), total);
        execution.check_cancelled();
    };
    if (report_is_access(d.query.kind)) {
        const auto access = execution.access(d.query.object_ids);
        if (d.query.kind == ReportKind::access_intervals) {
            std::size_t completed = 0;
            const auto intervals = core::find_access_intervals(
                first, last, d.query.step_nanoseconds, [&](core::Epoch epoch) {
                    execution.check_cancelled();
                    completed = std::max(completed,
                                         static_cast<std::size_t>(
                                             ticks_between(first, epoch) /
                                             static_cast<std::uint64_t>(d.query.step_nanoseconds)));
                    if (progress)
                        progress(completed, total);
                    execution.check_cancelled();
                    return access.at(epoch).visible;
                });
            for (const auto &window : intervals)
                rows.push_back({d.query.object_ids[1], access.name, window.first, window.last,
                                window.last.seconds_since(window.first),
                                std::int64_t(window.start_clipped),
                                std::int64_t(window.stop_clipped)});
        } else {
            auto epoch = first;
            for (std::size_t i = 0; i < total; ++i) {
                tick();
                const auto sample = access.at(epoch);
                const auto &o = sample.observation;
                rows.push_back({d.query.object_ids[1], access.name, epoch,
                                o.azimuth_rad ? results::Cell{*o.azimuth_rad} : results::Cell{},
                                o.elevation_rad, o.range_m, o.range_rate_mps,
                                std::int64_t(sample.visible),
                                sample.off_boresight_rad ? results::Cell{*sample.off_boresight_rad}
                                                         : results::Cell{}});
                epoch = ticks_between(epoch, last) <=
                                static_cast<std::uint64_t>(d.query.step_nanoseconds)
                            ? last
                            : core::Epoch{epoch.tai_nanoseconds() + d.query.step_nanoseconds};
            }
        }
    } else if (d.query.kind == ReportKind::scenario_summary) {
        rows = {{std::string("Scenario"), context.project.name},
                {std::string("Start"), first},
                {std::string("Stop"), last},
                {std::string("Time scale"), std::string("TAI")},
                {std::string("Satellites"),
                 static_cast<std::int64_t>(context.project.satellites.size())},
                {std::string("Ground stations"),
                 static_cast<std::int64_t>(context.project.ground_stations.size())},
                {std::string("Central body"), std::string("Earth")}};
    } else {
        if (d.query.kind == ReportKind::object_summary) {
            for (const auto &object : selected) {
                const std::string model =
                    object.station
                        ? "WGS84 fixed"
                        : std::string(propagator_name(object.satellite->orbit.propagator));
                rows.push_back({object.id, object.name, object.kind, model});
            }
        } else if (d.query.kind == ReportKind::ground_stations) {
            for (const auto &object : selected) {
                execution.check_cancelled();
                const auto &location = object.station->location;
                const auto p = core::ground_station_position(location);
                rows.push_back({object.id, object.name, location.longitude_rad,
                                location.latitude_rad, location.height_m, p[0], p[1], p[2]});
            }
        } else {
            // Derived templates have a fixed physical reference frame, independent of the
            // Cartesian template's frame preference saved in the definition.
            const auto target_frame = d.query.kind == ReportKind::geodetic_position
                                          ? core::FrameId::itrf
                                      : report_satellites_only(d.query.kind) ? core::FrameId::gcrf
                                                                             : d.query.frame;
            const auto per_object = total / selected.size();
            for (const auto &object : selected) {
                execution.check_cancelled();
                const auto session = execution.session(object, target_frame);
                auto next = first;
                for (std::size_t offset = 0; offset < per_object; offset += 512) {
                    tick();
                    std::vector<core::Epoch> epochs;
                    const auto count = std::min<std::size_t>(512, per_object - offset);
                    for (std::size_t i = 0; i < count; ++i) {
                        epochs.push_back(next);
                        if (ticks_between(next, last) <=
                            static_cast<std::uint64_t>(d.query.step_nanoseconds))
                            next = last;
                        else
                            next = core::Epoch{next.tai_nanoseconds() + d.query.step_nanoseconds};
                    }
                    const auto batch = session.sample(epochs, execution.check_cancelled);
                    check(batch.states().size() == epochs.size(),
                          "State provider returned the wrong row count");
                    for (std::size_t i = 0; i < epochs.size(); ++i) {
                        execution.check_cancelled();
                        auto state = batch.states()[i];
                        check(state.epoch == epochs[i] && state.origin == core::OriginId::earth,
                              "Invalid report state epoch or origin");
                        if (state.frame != target_frame) {
                            check(context.eop && context.frames,
                                  "Reference-frame conversion requires confirmed EOP data");
                            state = core::transform_state(state, target_frame, *context.frames,
                                                          context.eop->at(state.epoch));
                        }
                        const auto &p = state.position_m, &v = state.velocity_mps;
                        Row row{object.id, object.name, state.epoch,
                                std::string(target_frame == core::FrameId::gcrf ? "GCRF" : "ITRF")};
                        if (d.query.kind == ReportKind::classical_elements) {
                            const auto el = to_keplerian(
                                state, core::GravitationalParameter{object.satellite->orbit.mu()});
                            row.insert(row.end(),
                                       {el.semi_major_axis_m, el.eccentricity, el.inclination_rad,
                                        el.raan_rad, el.argument_of_periapsis_rad,
                                        el.true_anomaly_rad});
                        } else if (d.query.kind == ReportKind::geodetic_position) {
                            const auto geo = itrf_to_wgs84(state);
                            row.insert(row.end(),
                                       {geo.longitude_rad, geo.latitude_rad, geo.height_m});
                        } else if (d.query.kind == ReportKind::orbital_motion) {
                            const auto radius = std::hypot(p[0], p[1], p[2]);
                            check(radius > 0, "Orbital motion is undefined at the Earth centre");
                            const auto radial = (p[0] * v[0] + p[1] * v[1] + p[2] * v[2]) / radius;
                            const auto transverse =
                                std::hypot(p[1] * v[2] - p[2] * v[1], p[2] * v[0] - p[0] * v[2],
                                           p[0] * v[1] - p[1] * v[0]) /
                                radius;
                            row.insert(row.end(), {radius, std::hypot(v[0], v[1], v[2]), radial,
                                                   transverse, std::atan2(radial, transverse)});
                        } else
                            row.insert(row.end(), {p[0], p[1], p[2], v[0], v[1], v[2]});
                        rows.push_back(std::move(row));
                    }
                }
            }
        }
    }
    execution.check_cancelled();
    if (progress)
        progress(total, total);
    execution.check_cancelled();
    ReportResult result{d,
                        results::Table{std::move(columns), std::move(rows)},
                        report_input_sha256(context.project),
                        context.project.data_lock().sha256(),
                        context.time ? context.time->data_info().sha256 : "",
                        report_is_access(d.query.kind) ? "geometric-access-wgs84-v1"
                                                       : "builtin-reports-v2",
                        first,
                        last,
                        context.time};
    auto formatted = result.with_template(d.format);
    execution.check_cancelled();
    return formatted;
}
ReportResult ReportResult::with_template(const ReportTemplate &format) const {
    validate_report_template(format, definition.query.kind);
    auto result = *this;
    result.definition.format = format;
    result.source_table =
        source_table ? source_table : std::make_shared<const results::Table>(table);
    const auto &source = *result.source_table;
    if (format.columns.empty()) {
        result.table = source;
    } else {
        std::vector<Column> columns;
        std::vector<std::size_t> indices;
        bool unchanged = format.columns.size() == source.columns().size();
        for (const auto &spec : format.columns) {
            const auto field = std::ranges::find(source.columns(), spec.field, &Column::id);
            check(field != source.columns().end(), "Report source does not contain this field");
            const auto index = static_cast<std::size_t>(field - source.columns().begin());
            unchanged = unchanged && index == indices.size() &&
                        (spec.title.empty() || spec.title == field->title);
            indices.push_back(index);
            columns.push_back(*field);
            if (!spec.title.empty())
                columns.back().title = spec.title;
        }
        if (unchanged) {
            result.table = source;
        } else {
            std::vector<Row> rows;
            rows.reserve(source.rows().size());
            for (const auto &original : source.rows()) {
                Row row;
                row.reserve(indices.size());
                for (auto index : indices)
                    row.push_back(original[index]);
                rows.push_back(std::move(row));
            }
            result.table = results::Table{std::move(columns), std::move(rows)};
        }
    }
    // Check UTC availability at publication, before any cells are rendered.
    for (std::size_t c = 0; c < result.table.columns().size(); ++c) {
        const auto &field = result.table.columns()[c];
        const bool epoch =
            field.unit == "epoch" ||
            (field.id == "summary.value" && definition.query.kind == ReportKind::scenario_summary);
        const bool utc = field.unit == "epoch" ? result.column_unit(c) == "UTC"
                                               : format.time_scale == ReportTimeScale::utc;
        if (epoch && utc) {
            (void)epoch_text(first, ReportTimeScale::utc, time.get());
            (void)epoch_text(last, ReportTimeScale::utc, time.get());
        }
    }
    return result;
}
std::string ReportResult::column_title(std::size_t index) const {
    const auto &column = table.columns().at(index);
    const auto suffix = column_unit(index);
    return column.title + (suffix.empty() ? "" : " (" + suffix + ")");
}
std::string ReportResult::column_unit(std::size_t column) const {
    const auto &field = table.columns().at(column);
    const auto &specs = definition.format.columns;
    const auto spec = std::ranges::find(specs, field.id, &ReportColumnSpec::field);
    return spec != specs.end() && !spec->unit.empty() ? spec->unit : unit(definition, field.unit);
}
std::optional<double> ReportResult::numeric_value(std::size_t row, std::size_t column) const {
    const auto &cell = table.rows().at(row).at(column);
    if (const auto *integer = std::get_if<std::int64_t>(&cell))
        return static_cast<double>(*integer);
    const auto *real = std::get_if<double>(&cell);
    if (!real)
        return {};
    auto value = *real;
    const auto display_unit = column_unit(column);
    const auto &dimension = table.columns().at(column).unit;
    if ((dimension == "m" && display_unit == "km") ||
        (dimension == "m/s" && display_unit == "km/s"))
        value /= 1000;
    if (dimension == "rad" && display_unit == "deg")
        value *= 180 / std::numbers::pi;
    return value;
}
std::string ReportResult::cell_text(std::size_t row, std::size_t column) const {
    const auto &cell = table.rows().at(row).at(column);
    if (definition.query.kind == ReportKind::scenario_summary && row == 3 &&
        table.columns().at(column).id == "summary.value")
        return unit(definition, "epoch");
    if (const auto *text = std::get_if<std::string>(&cell))
        return *text;
    if (const auto *integer = std::get_if<std::int64_t>(&cell))
        return std::to_string(*integer);
    if (const auto *epoch = std::get_if<core::Epoch>(&cell)) {
        const auto display_unit = column_unit(column);
        const auto scale = display_unit == "UTC"   ? ReportTimeScale::utc
                           : display_unit == "TAI" ? ReportTimeScale::tai
                           : display_unit == "TT"  ? ReportTimeScale::tt
                                                   : definition.format.time_scale;
        return epoch_text(*epoch, scale, time.get());
    }
    if (const auto value = numeric_value(row, column)) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(17) << *value;
        return out.str();
    }
    return {};
}
void export_report_csv(const ReportResult &result, const std::filesystem::path &path,
                       bool overwrite, const std::atomic_bool *cancel) {
    cancelled(cancel);
    const auto destination = std::filesystem::absolute(path);
    check(!std::filesystem::exists(destination) || overwrite, "CSV destination already exists");
    std::filesystem::path staging;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto candidate =
            destination.parent_path() / (".orbitforge-report-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            staging = candidate;
            break;
        }
    }
    check(!staging.empty(), "Cannot create temporary CSV directory");
    const auto temporary = staging / "report.csv";
    try {
        std::ofstream out(temporary, std::ios::binary);
        out.exceptions(std::ios::badbit | std::ios::failbit);
        out << "\xEF\xBB\xBF";
        for (std::size_t c = 0; c < result.table.columns().size(); ++c) {
            if (c)
                out << ',';
            csv_field(out, result.column_title(c));
        }
        out << "\r\n";
        for (std::size_t r = 0; r < result.table.rows().size(); ++r) {
            cancelled(cancel);
            for (std::size_t c = 0; c < result.table.columns().size(); ++c) {
                if (c)
                    out << ',';
                csv_field(out, result.cell_text(r, c));
            }
            out << "\r\n";
        }
        out.close();
        cancelled(cancel);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                         MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0)))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Cannot publish CSV");
#else
        if (overwrite)
            std::filesystem::rename(temporary, destination);
        else {
            std::filesystem::create_hard_link(temporary, destination);
            std::filesystem::remove(temporary);
        }
#endif
        std::filesystem::remove(staging);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        std::filesystem::remove(staging, ignored);
        throw;
    }
}
} // namespace orbitforge::runtime
