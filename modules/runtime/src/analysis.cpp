#include "access_geometry.hpp"
#include "report_execution.hpp"
#include <algorithm>
#include <map>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <orbitforge/runtime/time_frames.hpp>

namespace orbitforge::runtime {
using namespace application;
namespace {
void check_epoch(const Project &project, core::Epoch epoch) {
    if (epoch.tai_nanoseconds() < project.first.tai_nanoseconds() ||
        epoch.tai_nanoseconds() > project.last.tai_nanoseconds())
        throw std::out_of_range("Query epoch is outside the prepared analysis interval");
}
void check_frame(core::FrameId frame) {
    if (frame != core::FrameId::gcrf && frame != core::FrameId::itrf &&
        frame != core::FrameId::teme)
        throw AnalysisCapabilityError("Analysis states support GCRF, ITRF and TEME");
}
std::uint64_t distance(core::Epoch first, core::Epoch last) {
    return static_cast<std::uint64_t>(last.tai_nanoseconds()) -
           static_cast<std::uint64_t>(first.tai_nanoseconds());
}
void record_data(Project &project, const DataSnapshot &data);
void check_cancelled(const std::function<bool()> &cancelled) {
    if (cancelled && cancelled())
        throw AnalysisCancelled{};
}
std::vector<core::Epoch> sample_epochs(const Project &p, std::chrono::nanoseconds step,
                                       std::optional<core::Epoch> first,
                                       std::optional<core::Epoch> last) {
    const auto begin = first.value_or(p.first), end = last.value_or(p.last);
    check_epoch(p, begin);
    check_epoch(p, end);
    if (step.count() <= 0 || begin.tai_nanoseconds() > end.tai_nanoseconds())
        throw std::invalid_argument("Sampling needs a positive step and an ordered subinterval");
    const auto span = distance(begin, end), stride = static_cast<std::uint64_t>(step.count());
    if (span / stride >= max_analysis_samples)
        throw std::invalid_argument(
            "Query exceeds 200000 samples; increase step or shorten interval");
    const auto count = span / stride + 1 + (span % stride != 0 ? 1 : 0);
    if (count > max_analysis_samples)
        throw std::invalid_argument(
            "Query exceeds 200000 samples; increase step or shorten interval");
    std::vector<core::Epoch> epochs;
    epochs.reserve(static_cast<std::size_t>(count));
    for (auto at = begin;;) {
        epochs.push_back(at);
        if (at == end)
            break;
        at = distance(at, end) <= stride ? end : core::Epoch{at.tai_nanoseconds() + step.count()};
    }
    return epochs;
}
std::string frame_name(core::FrameId frame) {
    switch (frame) {
    case core::FrameId::gcrf:
        return "GCRF";
    case core::FrameId::itrf:
        return "ITRF";
    case core::FrameId::teme:
        return "TEME";
    default:
        throw AnalysisCapabilityError("Unsupported state frame");
    }
}
} // namespace
DataSnapshot::DataSnapshot(std::shared_ptr<const core::ITimeProvider> time,
                           std::shared_ptr<const core::IFrameProvider> frames,
                           std::shared_ptr<const core::IEarthOrientationProvider> eop)
    : time_(std::move(time)), frames_(std::move(frames)), eop_(std::move(eop)) {
    if (static_cast<bool>(frames_) != static_cast<bool>(eop_) || (eop_ && !time_))
        throw std::invalid_argument(
            "Frame conversion requires time, EOP and frame providers together");
    if (time_)
        lock_ = time_data_lock(time_->data_info());
    if (eop_) {
        if (time_data_lock(eop_->data_info().time_data) != lock_)
            throw std::invalid_argument("EOP and time providers use different UTC data");
        lock_ = eop_data_lock(eop_->data_info());
    }
}
DataSnapshot DataSnapshot::builtin_time() { return DataSnapshot{make_builtin_time_provider()}; }
DataSnapshot DataSnapshot::with_gravity(const std::string &selection,
                                        std::shared_ptr<const core::GravityModel> model) const {
    if (!model || selection.empty())
        throw std::invalid_argument("Gravity requires a selection ID and a model");
    auto result = *this;
    const auto ref = gravity_reference(selection, model->info());
    std::vector<DataReference> refs;
    for (const auto &existing : lock_.references())
        if (existing.role != ref.role)
            refs.push_back(existing);
    refs.push_back(ref);
    result.lock_ = DataLock{std::move(refs)};
    result.gravity_[selection] = std::move(model);
    return result;
}
DataSnapshot DataSnapshot::with_gravity_file(const std::string &selection,
                                             const std::filesystem::path &file) const {
    return with_gravity(selection, load_gravity(file));
}
core::HpopEnvironment DataSnapshot::force_environment(const OrbitInput &input) const {
    std::shared_ptr<const core::GravityModel> model;
    std::shared_ptr<const core::IThirdBodyEphemeris> ephemeris;
    if (input.uses_hpop() && input.hpop.uses_ephemeris()) {
        const auto found = ephemerides_.find(input.hpop.ephemeris_model);
        if (found == ephemerides_.end())
            throw AnalysisCapabilityError("Missing ephemeris '" + input.hpop.ephemeris_model +
                                          "'; download it in Data Center or supply a DataSnapshot");
        ephemeris = found->second;
    }
    if (input.uses_hpop() && !input.hpop.gravity_model.empty()) {
        const auto it = gravity_.find(input.hpop.gravity_model);
        if (it == gravity_.end())
            throw AnalysisCapabilityError(
                "Missing gravity model '" + input.hpop.gravity_model +
                "'; download/import it in Data Center or supply it in DataSnapshot");
        model = it->second;
    }
    std::shared_ptr<const core::SpaceWeather> weather;
    if (input.uses_hpop() && input.hpop.drag.enabled && !input.hpop.drag.weather_model.empty()) {
        const auto found = weather_.find(input.hpop.drag.weather_model);
        if (found == weather_.end())
            throw AnalysisCapabilityError("Missing space weather '" +
                                          input.hpop.drag.weather_model +
                                          "'; download/import it in Data Center");
        weather = found->second;
    }
    std::shared_ptr<const core::OceanTideModel> ocean;
    if (input.uses_hpop() && input.hpop.ocean_tides.active()) {
        const auto found = ocean_tides_.find(input.hpop.ocean_tides.model);
        if (found == ocean_tides_.end())
            throw AnalysisCapabilityError("Missing ocean tide model '" +
                                          input.hpop.ocean_tides.model +
                                          "'; download/import it in Data Center");
        ocean = found->second;
    }
    return {
        frames_,         eop_, std::move(model), std::move(ephemeris), time_, std::move(weather),
        std::move(ocean)};
}
DataSnapshot
DataSnapshot::with_ephemeris(const std::string &selection,
                             std::shared_ptr<const core::IThirdBodyEphemeris> ephemeris) const {
    if (!ephemeris || selection.empty())
        throw std::invalid_argument("Ephemeris requires an ID and provider");
    auto result = *this;
    const auto ref = ephemeris_reference(selection, ephemeris->data_info());
    std::vector<DataReference> refs;
    for (const auto &current : lock_.references())
        if (current.role != ref.role)
            refs.push_back(current);
    refs.push_back(ref);
    result.lock_ = DataLock{std::move(refs)};
    result.ephemerides_[selection] = std::move(ephemeris);
    return result;
}
DataSnapshot DataSnapshot::with_ephemeris_file(const std::string &selection,
                                               const std::filesystem::path &manifest,
                                               const std::filesystem::path &workspace) const {
    return with_ephemeris(selection,
                          make_third_body_ephemeris(load_spice_snapshot(manifest, workspace)));
}
DataSnapshot
DataSnapshot::with_space_weather(const std::string &selection,
                                 std::shared_ptr<const core::SpaceWeather> weather) const {
    if (!weather || selection.empty())
        throw std::invalid_argument("Weather requires an ID and data");
    auto result = *this;
    const auto ref = space_weather_reference(selection, weather->info());
    std::vector<DataReference> refs;
    for (const auto &current : lock_.references())
        if (current.role != ref.role)
            refs.push_back(current);
    refs.push_back(ref);
    result.lock_ = DataLock{std::move(refs)};
    result.weather_[selection] = std::move(weather);
    return result;
}
DataSnapshot DataSnapshot::with_space_weather_file(const std::string &selection,
                                                   const std::filesystem::path &file) const {
    return with_space_weather(selection, load_space_weather(file));
}
DataSnapshot
DataSnapshot::with_ocean_tides(const std::string &selection,
                               std::shared_ptr<const core::OceanTideModel> model) const {
    core::OceanTideOptions options;
    options.model = selection;
    core::validate_ocean_tide_options(options);
    if (!model)
        throw std::invalid_argument("Ocean tides require a model");
    auto result = *this;
    const auto ref = ocean_tide_reference(selection, model->info());
    std::vector<DataReference> refs;
    for (const auto &current : lock_.references())
        if (current.role != ref.role)
            refs.push_back(current);
    refs.push_back(ref);
    result.lock_ = DataLock{std::move(refs)};
    result.ocean_tides_[selection] = std::move(model);
    return result;
}
DataSnapshot DataSnapshot::with_ocean_tide_file(const std::string &selection,
                                                const std::filesystem::path &file) const {
    return with_ocean_tides(selection, load_ocean_tides(file));
}
DataSnapshot
DataSnapshot::from_providers(std::shared_ptr<const core::ITimeProvider> time,
                             std::shared_ptr<const core::IFrameProvider> frames,
                             std::shared_ptr<const core::IEarthOrientationProvider> eop) {
    return DataSnapshot{std::move(time), std::move(frames), std::move(eop)};
}
DataSnapshot DataSnapshot::load(const std::filesystem::path &manifest,
                                const std::filesystem::path &time_file, int days) {
    if (manifest.empty() && time_file.empty())
        throw std::invalid_argument(
            "Select an EOP or time file, or use DataSnapshot.builtin_time()");
    if (days < 0 || days > core::max_eop_extrapolation_days || (manifest.empty() && days != 0))
        throw std::invalid_argument(
            "EOP extrapolation requires EOP data and a limit of 0..30 days");
    std::shared_ptr<const core::ITimeProvider> time =
        time_file.empty() ? make_builtin_time_provider() : load_time_snapshot(time_file);
    if (manifest.empty())
        return DataSnapshot{std::move(time)};
    std::shared_ptr<const core::IEarthOrientationProvider> eop =
        load_eop_snapshot_with_time(manifest, *time);
    if (days)
        eop = with_eop_extrapolation(std::move(eop), days, time);
    return DataSnapshot{std::move(time), make_frame_provider(), std::move(eop)};
}

struct Analysis::Impl {
    struct Entry {
        Session session;
        core::FrameId frame;
        std::string name, input_sha256;
        bool force_uses_eop = false;
    };
    ProjectSnapshot snapshot;
    DataSnapshot data;
    std::map<std::string, Entry> entries;
    Impl(ProjectSnapshot s, DataSnapshot d) : snapshot(std::move(s)), data(std::move(d)) {
        const auto &p = *snapshot.project;
        if (data.time()) {
            (void)data.time()->to_utc(p.first);
            (void)data.time()->to_utc(p.last);
        }
        if (data.eop()) {
            const auto &info = data.eop()->data_info();
            for (auto epoch : {p.first, p.last}) {
                if (!core::eop_covers(info, epoch))
                    throw core::EopCoverageError(
                        "Analysis requires TAI ns [" + std::to_string(p.first.tai_nanoseconds()) +
                        ", " + std::to_string(p.last.tai_nanoseconds()) +
                        "]; EOP original range [" +
                        std::to_string(info.first_epoch.tai_nanoseconds()) + ", " +
                        std::to_string(info.last_epoch.tai_nanoseconds()) +
                        "], extrapolation days=" + std::to_string(info.extrapolation_days));
                (void)data.eop()->at(epoch);
            }
        }
        for (const auto &satellite : p.satellites) {
            const auto &input = satellite.orbit;
            if (input.propagator == OrbitPropagator::sgp4 && !data.time())
                throw AnalysisCapabilityError("SGP4 requires explicitly selected UTC time data");
            Project numerical{"analysis-state-input-v1", p.first, p.last};
            numerical.satellites.push_back(satellite);
            numerical.satellites.back().display_name.clear();
            if (input.propagator != OrbitPropagator::sgp4)
                numerical.satellites.back().orbit.tle = {}; // Inactive TLE draft is not an input.
            entries.emplace(input.object_id,
                            Entry{make_orbit_session(input, snapshot.revision, data.time().get(),
                                                     data.force_environment(input)),
                                  input.propagator == OrbitPropagator::sgp4 ? core::FrameId::teme
                                                                            : core::FrameId::gcrf,
                                  satellite.name(), report_input_sha256(numerical),
                                  input.uses_eop_force() ||
                                      (input.uses_hpop() && (!input.hpop.gravity_model.empty() ||
                                                             input.hpop.uses_ephemeris()))});
        }
        for (const auto &station : p.ground_stations) {
            Project numerical{"analysis-state-input-v1", p.first, p.last};
            numerical.ground_stations.push_back(station);
            numerical.ground_stations.back().display_name.clear();
            numerical.ground_stations.back().minimum_elevation_rad = 0; // Not a state dependency.
            entries.emplace(station.object_id,
                            Entry{make_ground_station_session(station, snapshot.revision),
                                  core::FrameId::itrf, station.name(),
                                  report_input_sha256(numerical)});
        }
    }
    const Entry &entry(const ObjectRef &ref, core::FrameId target) const {
        if (!snapshot.owns(ref))
            throw ProjectError("Object '" + ref.id() + "' belongs to another project");
        if (ref.kind() == ObjectKind::sensor)
            throw AnalysisCapabilityError("Query the parent satellite for a sensor's position");
        const auto it = entries.find(ref.id());
        if (it == entries.end())
            throw ObjectNotFoundError("Object '" + ref.id() + "' is absent from this analysis");
        check_frame(target);
        if (target != it->second.frame && !data.frames())
            throw AnalysisCapabilityError("Prepare with EOP data to convert reference frames");
        return it->second;
    }
    std::string dependency_hash(const Entry &entry, core::FrameId target) const {
        if (target != entry.frame || entry.force_uses_eop)
            return data.data_lock().sha256();
        if (entry.frame == core::FrameId::teme)
            return time_data_lock(data.time()->data_info()).sha256();
        return DataLock{}.sha256();
    }
    detail::AccessGeometry geometry(const std::vector<std::string> &ids) const {
        detail::AccessGeometry pair{*snapshot.project, ids};
        if (!data.time() || !data.eop() || !data.frames())
            throw AnalysisCapabilityError("Prepare with UTC and EOP data to query access or AER");
        return pair;
    }
    detail::AccessGeometry geometry(const ObjectRef &station, const ObjectRef &target) const {
        (void)snapshot.read(station);
        (void)snapshot.read(target);
        if (station.kind() != ObjectKind::ground_station ||
            (target.kind() != ObjectKind::satellite && target.kind() != ObjectKind::sensor))
            throw std::invalid_argument(
                "Access/AER requires a ground station and a satellite or sensor");
        return geometry({station.id(), target.id()});
    }
    results::AerSample observe(const detail::AccessGeometry &pair, core::Epoch epoch,
                               const std::function<bool()> &cancelled = {}) const {
        const auto &session = entries.at(pair.satellite.orbit.object_id).session;
        const auto state = core::transform_state(
            session.sample(std::array{epoch}, [&] { check_cancelled(cancelled); }).states().front(),
            core::FrameId::itrf, *data.frames(), data.eop()->at(epoch));
        return pair.at(state);
    }
    results::AccessProvenance provenance(const detail::AccessGeometry &pair) const {
        const auto &p = *snapshot.project;
        return {pair.station.object_id,
                pair.station.name(),
                pair.sensor ? pair.sensor->object_id : pair.satellite.orbit.object_id,
                pair.target_name(),
                snapshot.revision,
                analysis_input_sha256(pair.numerical_inputs(p.first, p.last)),
                data.data_lock().sha256(),
                entries.at(pair.satellite.orbit.object_id).session.provenance().algorithm +
                    ";frames=state-frame-routing-v1;geometric-access-wgs84-v1"};
    }
};
Analysis::Analysis(ProjectSnapshot snapshot, DataSnapshot data)
    : impl_(std::make_shared<const Impl>(std::move(snapshot), std::move(data))) {}
const ProjectSnapshot &Analysis::snapshot() const { return impl_->snapshot; }
const DataSnapshot &Analysis::data() const { return impl_->data; }
core::CartesianState Analysis::state(const ObjectRef &ref, core::Epoch at,
                                     core::FrameId frame) const {
    return states(ref, std::array{at}, frame).states().front();
}
results::Trajectory Analysis::states(const ObjectRef &ref, std::span<const core::Epoch> times,
                                     core::FrameId frame) const {
    const auto &entry = impl_->entry(ref, frame);
    if (times.size() > max_analysis_samples)
        throw std::invalid_argument("State query exceeds 200000 samples; split the query");
    for (auto epoch : times)
        check_epoch(*impl_->snapshot.project, epoch);
    auto series = entry.session.sample(times);
    auto provenance = series.provenance();
    if (frame != entry.frame) {
        auto values = series.states();
        for (auto &state : values)
            state = core::transform_state(state, frame, *impl_->data.frames(),
                                          impl_->data.eop()->at(state.epoch));
        provenance.algorithm += ";frames=state-frame-routing-v1;target=" + frame_name(frame);
        series = results::StateSeries{std::move(provenance), std::move(values)};
    }
    return results::Trajectory{std::move(series),     entry.name,
                               core::OriginId::earth, frame,
                               entry.input_sha256,    impl_->dependency_hash(entry, frame)};
}
results::Trajectory Analysis::states(const ObjectRef &ref, std::chrono::nanoseconds step,
                                     core::FrameId frame, std::optional<core::Epoch> first,
                                     std::optional<core::Epoch> last) const {
    (void)impl_->entry(ref, frame);
    const auto epochs = sample_epochs(*snapshot().project, step, first, last);
    return states(ref, epochs, frame);
}
results::ManeuverSeries Analysis::maneuvers(const ObjectRef &ref,
                                            const std::function<bool()> &cancelled) const {
    check_cancelled(cancelled);
    (void)snapshot().read(ref);
    if (ref.kind() != ObjectKind::satellite)
        throw AnalysisCapabilityError("Maneuver queries require a satellite");
    const auto &entry = impl_->entries.at(ref.id());
    const auto &project = *snapshot().project;
    auto events =
        entry.session.maneuvers(project.first, project.last, [&] { check_cancelled(cancelled); });
    check_cancelled(cancelled);
    return {std::move(events), entry.session.provenance(), entry.input_sha256,
            impl_->dependency_hash(entry, entry.frame)};
}
results::AccessWindows Analysis::access(const ObjectRef &station, const ObjectRef &target,
                                        const AccessOptions &options,
                                        const std::function<bool()> &cancelled) const {
    check_cancelled(cancelled);
    const auto geometry = impl_->geometry(station, target);
    const auto &p = *snapshot().project;
    const auto first = options.first.value_or(p.first), last = options.last.value_or(p.last);
    check_epoch(p, first);
    check_epoch(p, last);
    const auto intervals = core::find_access_intervals(
        first, last, options.search_step.count(),
        [&](core::Epoch epoch) {
            check_cancelled(cancelled);
            return impl_->observe(geometry, epoch, cancelled).visible;
        },
        options.boundary_tolerance.count());
    std::vector<results::AccessWindow> windows;
    windows.reserve(intervals.size());
    for (const auto &interval : intervals)
        windows.emplace_back(interval.first, interval.last, interval.start_clipped,
                             interval.stop_clipped);
    auto provenance = impl_->provenance(geometry);
    provenance.algorithm += ";fixed-grid-bisection-v1";
    check_cancelled(cancelled);
    return {std::move(windows),
            {first, last, options.search_step, options.boundary_tolerance},
            std::move(provenance)};
}
results::AerSeries Analysis::aer(const ObjectRef &station, const ObjectRef &target,
                                 std::span<const core::Epoch> times,
                                 const std::function<bool()> &cancelled) const {
    check_cancelled(cancelled);
    const auto geometry = impl_->geometry(station, target);
    if (times.size() > max_analysis_samples)
        throw std::invalid_argument("AER query exceeds 200000 samples; split the query");
    for (auto epoch : times)
        check_epoch(*snapshot().project, epoch);
    std::vector<results::AerSample> samples;
    samples.reserve(times.size());
    for (auto epoch : times) {
        check_cancelled(cancelled);
        samples.push_back(impl_->observe(geometry, epoch, cancelled));
    }
    auto provenance = impl_->provenance(geometry);
    check_cancelled(cancelled);
    return {std::move(samples), std::move(provenance)};
}
results::AerSeries Analysis::aer(const ObjectRef &station, const ObjectRef &target,
                                 std::chrono::nanoseconds step, std::optional<core::Epoch> first,
                                 std::optional<core::Epoch> last,
                                 const std::function<bool()> &cancelled) const {
    check_cancelled(cancelled);
    const auto epochs = sample_epochs(*snapshot().project, step, first, last);
    return aer(station, target, epochs, cancelled);
}
ReportResult Analysis::report(const application::ReportQuery &query,
                              const application::ReportTemplate &format,
                              const std::function<bool()> &cancelled,
                              const std::function<void(std::size_t, std::size_t)> &progress) const {
    return report(ReportDefinition{"query", "Report", query, format}, cancelled, progress);
}
ReportResult Analysis::report(const ReportDefinition &definition,
                              const std::function<bool()> &cancelled,
                              const std::function<void(std::size_t, std::size_t)> &progress) const {
    // A callback may edit a caller-owned definition; capture it before the first callback.
    const auto captured = definition;
    check_cancelled(cancelled);
    ReportContext context{*snapshot().project,      data().time(),
                          data().frames(),          data().eop(),
                          data().gravity_models(),  data().ephemerides(),
                          data().weather_sources(), data().ocean_tide_models()};
    record_data(context.project, data());
    if (!data().time() && report_uses_utc(captured.format, captured.query.kind))
        throw AnalysisCapabilityError("Prepare with UTC data for a UTC report, or select TAI/TT");
    const detail::ReportExecution execution{
        [&](const detail::ReportObject &object, core::FrameId frame) {
            const auto &entry = impl_->entries.at(object.id);
            if (entry.frame != frame && !data().frames())
                throw AnalysisCapabilityError("Prepare with EOP data to convert reference frames");
            return entry.session;
        },
        [&](const std::vector<std::string> &ids) {
            auto pair = impl_->geometry(ids);
            auto name = pair.station.name() + " → " + pair.target_name();
            return detail::ReportAccess{std::move(name),
                                        [&, pair = std::move(pair)](core::Epoch epoch) {
                                            return impl_->observe(pair, epoch, cancelled);
                                        }};
        },
        [&] { check_cancelled(cancelled); }};
    auto result = detail::generate_report(context, captured, execution, progress);
    result.input_revision = snapshot().revision;
    return result;
}
DataChangedError::DataChangedError(AnalysisPlan plan)
    : std::runtime_error("Selected calculation data differ from the project's recorded data"),
      plan_(std::make_shared<const AnalysisPlan>(std::move(plan))) {}
AnalysisPlan::AnalysisPlan(const ProjectDocument &project, DataSnapshot data)
    : AnalysisPlan(project, Analysis{project.snapshot(), std::move(data)}) {}
AnalysisPlan::AnalysisPlan(const ProjectDocument &project, const DataCatalog &catalog)
    : AnalysisPlan(project, [&] {
          const auto captured = project.snapshot();
          const auto [first, last] = calculation_data_interval(*captured.project);
          auto data = catalog.snapshot(first, last);
          for (const auto &satellite : captured.project->satellites) {
              const auto &input = satellite.orbit;
              if (input.uses_hpop() && input.hpop.uses_ephemeris() &&
                  !data.ephemerides().contains(input.hpop.ephemeris_model))
                  data = data.with_ephemeris(input.hpop.ephemeris_model,
                                             catalog.load_ephemeris(input.hpop.ephemeris_model));
              if (input.uses_hpop() && !input.hpop.gravity_model.empty() &&
                  !data.gravity_models().contains(input.hpop.gravity_model))
                  data = data.with_gravity(input.hpop.gravity_model,
                                           catalog.load_gravity(input.hpop.gravity_model));
              if (input.uses_hpop() && input.hpop.ocean_tides.active() &&
                  !data.ocean_tide_models().contains(input.hpop.ocean_tides.model))
                  data =
                      data.with_ocean_tides(input.hpop.ocean_tides.model,
                                            catalog.load_ocean_tides(input.hpop.ocean_tides.model));
              if (input.uses_hpop() && input.hpop.drag.enabled &&
                  !input.hpop.drag.weather_model.empty() &&
                  !data.weather_sources().contains(input.hpop.drag.weather_model))
                  data = data.with_space_weather(
                      input.hpop.drag.weather_model,
                      catalog.load_space_weather(input.hpop.drag.weather_model));
          }
          return Analysis{captured, std::move(data)};
      }()) {}
AnalysisPlan::AnalysisPlan(const ProjectDocument &project, Analysis analysis)
    : owner_(project), analysis_(std::move(analysis)) {
    const auto recorded = snapshot().project->data_lock();
    for (const auto &selected : analysis_.data().data_lock().references()) {
        const auto previous =
            std::ranges::find(recorded.references(), selected.role, &DataReference::role);
        if (previous != recorded.references().end() && *previous != selected)
            changes_.push_back({selected.role, *previous, selected});
    }
}
Analysis AnalysisPlan::prepare(bool accept) const {
    if (owner_.snapshot().revision != snapshot().revision)
        throw RevisionConflictError(
            "Project changed since preparation; create a new analysis plan");
    if (!accept && !changes_.empty())
        throw DataChangedError{*this};
    return analysis_;
}
Analysis prepare(const ProjectDocument &project, DataSnapshot data) {
    return AnalysisPlan{project, std::move(data)}.prepare();
}
Analysis prepare(const ProjectDocument &project, const DataCatalog &catalog) {
    return AnalysisPlan{project, catalog}.prepare();
}
namespace {
void record_data(Project &project, const DataSnapshot &data) {
    project.earth_orientation = data.eop() ? std::optional{data.eop()->data_info()} : std::nullopt;
    project.time_data =
        !data.eop() && data.time() ? std::optional{data.time()->data_info()} : std::nullopt;
    project.force_data.clear();
    for (const auto &ref : data.data_lock().references())
        if (ref.role.starts_with("gravity/") || ref.role.starts_with("ephemeris/") ||
            ref.role.starts_with("space-weather/") || ref.role.starts_with("ocean-tides/"))
            project.force_data.push_back(ref);
}
} // namespace
void save_project(const ProjectDocument &project, const std::filesystem::path &path,
                  const Analysis &analysis, bool overwrite) {
    auto candidate = project.snapshot();
    if (candidate.scope != analysis.snapshot().scope)
        throw ProjectError("Analysis belongs to another project");
    const auto hash = analysis_input_sha256(*candidate.project);
    if (hash != analysis_input_sha256(*analysis.snapshot().project))
        throw RevisionConflictError(
            "Calculation inputs changed; prepare a new analysis before saving it");
    auto saved = std::make_shared<Project>(*candidate.project);
    record_data(*saved, analysis.data());
    saved->analysis_record = AnalysisRecord{hash, analysis.data().data_lock().sha256()};
    candidate.project = std::move(saved);
    project.save(path, candidate, overwrite);
}
ProjectOpenPlan::ProjectOpenPlan(const std::filesystem::path &path, DataSnapshot data)
    : ProjectOpenPlan(
          AnalysisPlan{ProjectDocument{application::load_project(path)}, std::move(data)}) {}
ProjectOpenPlan::ProjectOpenPlan(const std::filesystem::path &path, const DataCatalog &catalog)
    : ProjectOpenPlan(AnalysisPlan{ProjectDocument{application::load_project(path)}, catalog}) {}
ProjectOpenPlan::ProjectOpenPlan(AnalysisPlan plan) : plan_(std::move(plan)) {
    const auto &stored = *snapshot().project;
    if (stored.earth_orientation && !data().eop())
        throw AnalysisCapabilityError(
            "Opening this project requires EOP data; supply data=DataCatalog or DataSnapshot");
    if (stored.time_data_info() && !data().time())
        throw AnalysisCapabilityError(
            "Opening this project requires UTC data; supply data=DataCatalog or DataSnapshot");
}
ProjectDocument ProjectOpenPlan::open(bool accept) const {
    if (!accept && !data_changes().empty())
        throw OpenDataChangedError{*this};
    (void)plan_.prepare(accept);
    ProjectDocument result{*snapshot().project};
    if (snapshot().project->data_lock() != data().data_lock()) {
        auto selected = *snapshot().project;
        record_data(selected, data());
        auto edit = result.edit();
        edit.analysis_data(selected.earth_orientation, selected.time_data, selected.analysis_record,
                           selected.force_data);
        edit.commit();
    }
    return result;
}
OpenDataChangedError::OpenDataChangedError(ProjectOpenPlan plan)
    : std::runtime_error("Selected calculation data differ from the project's recorded data"),
      plan_(std::make_shared<const ProjectOpenPlan>(std::move(plan))) {}
ProjectDocument open_project(const std::filesystem::path &path, DataSnapshot data) {
    return ProjectOpenPlan{path, std::move(data)}.open();
}
ProjectDocument open_project(const std::filesystem::path &path, const DataCatalog &catalog) {
    return ProjectOpenPlan{path, catalog}.open();
}
void export_trajectory_csv(const results::Trajectory &result, const std::filesystem::path &path,
                           bool overwrite) {
    std::vector<results::Row> rows;
    rows.reserve(result.states().size());
    for (const auto &s : result.states())
        rows.push_back({s.epoch.tai_nanoseconds(), s.position_m[0], s.position_m[1],
                        s.position_m[2], s.velocity_mps[0], s.velocity_mps[1], s.velocity_mps[2],
                        std::string("EARTH"), frame_name(result.frame())});
    results::Table table{{{"epoch_tai_ns", "epoch_tai_ns", ""},
                          {"position.x", "position.x", "m"},
                          {"position.y", "position.y", "m"},
                          {"position.z", "position.z", "m"},
                          {"velocity.x", "velocity.x", "m/s"},
                          {"velocity.y", "velocity.y", "m/s"},
                          {"velocity.z", "velocity.z", "m/s"},
                          {"origin", "origin", ""},
                          {"frame", "frame", ""}},
                         std::move(rows)};
    ReportDefinition definition;
    definition.format.units = ReportUnits::si;
    const ReportResult report{definition,
                              std::move(table),
                              result.input_sha256(),
                              result.data_sha256(),
                              "",
                              result.provenance().algorithm,
                              core::Epoch{0},
                              core::Epoch{0},
                              {}};
    export_report_csv(report, path, overwrite);
}
void export_access_csv(const results::AccessWindows &result, const std::filesystem::path &path,
                       bool overwrite) {
    const auto &provenance = result.provenance();
    std::vector<results::Row> rows;
    rows.reserve(result.windows().size());
    for (const auto &window : result.windows())
        rows.push_back({provenance.station_id, provenance.target_id,
                        window.start().tai_nanoseconds(), window.end().tai_nanoseconds(),
                        window.duration().count(),
                        std::chrono::duration<double>(window.duration()).count(),
                        static_cast<std::int64_t>(window.start_clipped()),
                        static_cast<std::int64_t>(window.end_clipped())});
    results::Table table{{{"station_id", "station_id", ""},
                          {"target_id", "target_id", ""},
                          {"start_tai_ns", "start_tai_ns", ""},
                          {"end_tai_ns", "end_tai_ns", ""},
                          {"duration_ns", "duration_ns", ""},
                          {"duration_s", "duration_s", ""},
                          {"start_clipped", "start_clipped", ""},
                          {"end_clipped", "end_clipped", ""}},
                         std::move(rows)};
    ReportDefinition definition;
    definition.format.units = ReportUnits::si;
    const ReportResult report{
        definition, std::move(table),     provenance.input_sha256, provenance.data_sha256,
        "",         provenance.algorithm, result.query().first,    result.query().last,
        {}};
    export_report_csv(report, path, overwrite);
}
void export_aer_csv(const results::AerSeries &result, const std::filesystem::path &path,
                    bool overwrite) {
    const auto &p = result.provenance();
    std::vector<results::Row> rows;
    rows.reserve(result.samples().size());
    for (const auto &s : result.samples()) {
        const auto &o = s.observation;
        rows.push_back(
            {p.station_id, p.target_id, s.epoch.tai_nanoseconds(),
             o.azimuth_rad ? results::Cell{*o.azimuth_rad} : results::Cell{}, o.elevation_rad,
             o.range_m, o.range_rate_mps, std::int64_t(o.line_of_sight), std::int64_t(s.visible),
             s.off_boresight_rad ? results::Cell{*s.off_boresight_rad} : results::Cell{}});
    }
    results::Table table{{{"station_id", "station_id", ""},
                          {"target_id", "target_id", ""},
                          {"epoch_tai_ns", "epoch_tai_ns", ""},
                          {"azimuth", "azimuth", "rad"},
                          {"elevation", "elevation", "rad"},
                          {"range", "range", "m"},
                          {"range_rate", "range_rate", "m/s"},
                          {"line_of_sight", "line_of_sight", ""},
                          {"visible", "visible", ""},
                          {"off_boresight", "off_boresight", "rad"}},
                         std::move(rows)};
    ReportDefinition definition;
    definition.format.units = ReportUnits::si;
    const ReportResult report{definition, std::move(table), p.input_sha256, p.data_sha256,
                              "",         p.algorithm,      core::Epoch{0}, core::Epoch{0},
                              {},         p.input_revision};
    export_report_csv(report, path, overwrite);
}
} // namespace orbitforge::runtime
