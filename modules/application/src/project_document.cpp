#include <algorithm>
#include <atomic>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <orbitforge/application/project_document.hpp>
#include <random>
#include <sstream>

namespace orbitforge::application {
namespace detail {
struct DocumentState {
    std::mutex mutex;
    std::shared_ptr<const Project> project;
    std::uint64_t scope, revision = 0, saved_revision = 0, next_id = 0, next_observer = 0;
    std::string prefix;
    std::map<std::uint64_t, std::function<void(const ChangeSet &)>> observers;
    struct Notification {
        ChangeSet changes;
        std::vector<std::function<void(const ChangeSet &)>> observers;
    };
    std::deque<Notification> pending;
    bool notifying = false;
    std::vector<std::string> errors;
};
struct EditState {
    ProjectSnapshot base;
    std::shared_ptr<const Project> candidate;
    bool closed = false;
};
} // namespace detail
namespace {
std::atomic<std::uint64_t> next_scope{0};
const std::string &id_of(const ProjectSatellite &s) { return s.orbit.object_id; }
template <typename T> const std::string &id_of(const T &s) { return s.object_id; }
template <typename T> auto &lookup(std::vector<T> &values, const std::string &id) {
    const auto found = std::ranges::find_if(values, [&](const auto &v) { return id_of(v) == id; });
    if (found == values.end())
        throw ObjectNotFoundError("Object '" + id + "' no longer exists");
    return *found;
}
template <typename T> const auto &lookup(const std::vector<T> &values, const std::string &id) {
    const auto found = std::ranges::find_if(values, [&](const auto &v) { return id_of(v) == id; });
    if (found == values.end())
        throw ObjectNotFoundError("Object '" + id + "' no longer exists");
    return *found;
}
ObjectState read_object(const Project &project, const std::string &id, ObjectKind kind) {
    switch (kind) {
    case ObjectKind::satellite:
        return lookup(project.satellites, id);
    case ObjectKind::ground_station:
        return lookup(project.ground_stations, id);
    case ObjectKind::sensor:
        return lookup(project.sensors, id);
    }
    throw ProjectError("Invalid object kind");
}
void require_open(const detail::EditState &edit) {
    if (edit.closed)
        throw EditClosedError("The project edit is closed");
}
bool same_orbit(const OrbitInput &a, const OrbitInput &b) {
    return a.initial.epoch == b.initial.epoch && a.initial.origin == b.initial.origin &&
           a.initial.frame == b.initial.frame && a.initial.position_m == b.initial.position_m &&
           a.initial.velocity_mps == b.initial.velocity_mps && a.propagator == b.propagator &&
           a.input_frame == b.input_frame && a.tle.line1 == b.tle.line1 &&
           a.tle.line2 == b.tle.line2 && a.first == b.first && a.last == b.last &&
           a.step_seconds == b.step_seconds && (!a.uses_hpop() || a.hpop == b.hpop) &&
           (a.propagator != OrbitPropagator::mission || a.mission == b.mission);
}
std::vector<std::string> fields(const ProjectSatellite &a, const ProjectSatellite &b) {
    std::vector<std::string> result;
    if (a.display_name != b.display_name)
        result.emplace_back("name");
    if (!same_orbit(a.orbit, b.orbit))
        result.emplace_back("orbit");
    if (a.display != b.display)
        result.emplace_back("display");
    if (a.model_kind != b.model_kind || a.model_diameter_m != b.model_diameter_m ||
        a.model_path != b.model_path)
        result.emplace_back("model");
    return result;
}
std::vector<std::string> fields(const ProjectGroundStation &a, const ProjectGroundStation &b) {
    std::vector<std::string> result;
    if (a.display_name != b.display_name)
        result.emplace_back("name");
    if (a.location != b.location)
        result.emplace_back("location");
    if (a.minimum_elevation_rad != b.minimum_elevation_rad)
        result.emplace_back("minimum_elevation");
    return result;
}
std::vector<std::string> fields(const ProjectSensor &a, const ProjectSensor &b) {
    std::vector<std::string> result;
    if (a.display_name != b.display_name)
        result.emplace_back("name");
    if (a.parent_satellite_id != b.parent_satellite_id)
        result.emplace_back("parent");
    if (a.half_angle_rad != b.half_angle_rad)
        result.emplace_back("field_of_view");
    if (a.pointing != b.pointing || a.target_station_id != b.target_station_id)
        result.emplace_back("pointing");
    return result;
}
template <typename T>
void diff_objects(const std::vector<T> &before, const std::vector<T> &after, ChangeSet &changes) {
    std::map<std::string, const T *> old;
    for (const auto &value : before)
        old.emplace(id_of(value), &value);
    for (const auto &value : after) {
        const auto found = old.find(id_of(value));
        if (found == old.end())
            changes.added.push_back(id_of(value));
        else {
            auto changed = fields(*found->second, value);
            if (!changed.empty())
                changes.updated.push_back({id_of(value), std::move(changed)});
            old.erase(found);
        }
    }
    for (const auto &value : before)
        if (old.contains(id_of(value)))
            changes.removed.push_back(id_of(value));
}
auto metadata_bytes(Project value) {
    value.satellites.clear();
    value.ground_stations.clear();
    value.sensors.clear();
    return encode_project(value);
}
void notify(const std::shared_ptr<detail::DocumentState> &state) {
    for (;;) {
        detail::DocumentState::Notification item;
        {
            std::lock_guard lock(state->mutex);
            if (state->pending.empty()) {
                state->notifying = false;
                return;
            }
            item = std::move(state->pending.front());
            state->pending.pop_front();
        }
        for (const auto &observer : item.observers) {
            std::string error;
            try {
                observer(item.changes);
            } catch (const std::exception &e) {
                error = e.what();
            } catch (...) {
                error = "Unknown project observer failure";
            }
            if (!error.empty()) {
                std::lock_guard lock(state->mutex);
                if (state->errors.size() == 32)
                    state->errors.erase(state->errors.begin());
                state->errors.push_back(std::move(error));
            }
        }
    }
}
} // namespace

ProjectDocument::ProjectDocument(Project project)
    : state_(std::make_shared<detail::DocumentState>()) {
    validate_project(project);
    state_->project = std::make_shared<const Project>(std::move(project));
    state_->scope = ++next_scope;
    // Per-document random namespace also prevents reusing deleted IDs after reopen.
    std::random_device random;
    std::ostringstream prefix;
    prefix << "object-" << std::hex << std::setfill('0');
    for (int i = 0; i != 4; ++i)
        prefix << std::setw(8) << static_cast<std::uint32_t>(random());
    state_->prefix = prefix.str() + "-";
}
ProjectSnapshot ProjectDocument::snapshot() const {
    std::lock_guard lock(state_->mutex);
    return {state_->revision, state_->project, state_->scope};
}
ObjectState ProjectSnapshot::read(const ObjectRef &ref) const {
    if (!owns(ref))
        throw ProjectError("Object '" + ref.id() + "' belongs to another project");
    if (!project)
        throw ProjectError("Missing project snapshot");
    return read_object(*project, ref.id(), ref.kind());
}
bool ProjectDocument::dirty() const {
    std::lock_guard lock(state_->mutex);
    return state_->revision != state_->saved_revision;
}
void ProjectDocument::mark_saved(std::uint64_t revision) {
    std::lock_guard lock(state_->mutex);
    if (revision > state_->revision)
        throw ProjectError("Cannot mark a future revision saved");
    state_->saved_revision = revision;
}
void ProjectDocument::save(const std::filesystem::path &path, bool overwrite) const {
    save(path, snapshot(), overwrite);
}
void ProjectDocument::save(const std::filesystem::path &path, const ProjectSnapshot &candidate,
                           bool overwrite) const {
    const auto before = snapshot();
    if (candidate.scope != before.scope || candidate.revision != before.revision ||
        !candidate.project)
        throw RevisionConflictError(
            "Save candidate does not match this project's current revision");
    // Callers may have built the candidate from a mutable shared_ptr. Never
    // publish an alias that they could mutate after this operation returns.
    const auto owned = std::make_shared<const Project>(*candidate.project);
    validate_project(*owned);
    const bool changed = encode_project(*before.project) != encode_project(*owned);
    const auto revision = before.revision + (changed ? 1 : 0);
    ChangeSet changes{
        before.revision, revision, {}, {}, {}, false, before, {revision, owned, before.scope}};
    if (changed) {
        diff_objects(before.project->satellites, owned->satellites, changes);
        diff_objects(before.project->ground_stations, owned->ground_stations, changes);
        diff_objects(before.project->sensors, owned->sensors, changes);
        changes.settings_changed = metadata_bytes(*before.project) != metadata_bytes(*owned);
    }
    detail::DocumentState::Notification pending{std::move(changes), {}};
    bool dispatch = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->revision != before.revision)
            throw RevisionConflictError("Project changed while preparing the save; retry saving");
        if (changed) {
            for (const auto &[token, observer] : state_->observers) {
                (void)token;
                pending.observers.push_back(observer);
            }
            // Allocate notification storage before publishing the file. No allocation
            // or user code may fail between file publication and document publication.
            state_->pending.push_back(std::move(pending));
        }
        try {
            save_project(path, *owned, overwrite);
        } catch (...) {
            if (changed) {
                pending = std::move(state_->pending.back());
                state_->pending.pop_back();
            }
            throw;
        }
        state_->project = owned;
        state_->revision = state_->saved_revision = revision;
        if (changed) {
            dispatch = !state_->notifying;
            state_->notifying = true;
        }
    }
    if (dispatch)
        notify(state_);
}
std::uint64_t ProjectDocument::subscribe(std::function<void(const ChangeSet &)> observer) {
    if (!observer)
        throw ProjectError("Project observer must be callable");
    std::lock_guard lock(state_->mutex);
    const auto token = ++state_->next_observer;
    state_->observers.emplace(token, std::move(observer));
    return token;
}
void ProjectDocument::unsubscribe(std::uint64_t token) {
    std::function<void(const ChangeSet &)> retired;
    {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->observers.find(token);
        if (found == state_->observers.end())
            return;
        retired = std::move(found->second);
        state_->observers.erase(found);
    }
    // User callback destructors may acquire the GIL or call back into a document.
}
std::vector<std::string> ProjectDocument::observer_errors() const {
    std::lock_guard lock(state_->mutex);
    return state_->errors;
}
ProjectEdit::ProjectEdit(std::shared_ptr<detail::DocumentState> state)
    : state_(std::move(state)), edit_(std::make_shared<detail::EditState>()) {
    std::lock_guard lock(state_->mutex);
    edit_->base = {state_->revision, state_->project, state_->scope};
    edit_->candidate = state_->project;
}
ProjectEdit::~ProjectEdit() { discard(); }
void ProjectEdit::discard() noexcept {
    if (edit_)
        edit_->closed = true;
}
ObjectManager ProjectEdit::objects() const {
    require_open(*edit_);
    return {state_, edit_};
}
ProjectSnapshot ProjectEdit::snapshot() const {
    require_open(*edit_);
    return {edit_->base.revision, edit_->candidate, state_->scope};
}
void ProjectEdit::settings(std::optional<std::string> name, std::optional<core::Epoch> first,
                           std::optional<core::Epoch> last, std::optional<core::Epoch> current) {
    objects().mutate([&](Project &next) {
        if (name)
            next.name = *name;
        if (first)
            next.first = *first;
        if (last)
            next.last = *last;
        if (next.first.tai_nanoseconds() >= next.last.tai_nanoseconds())
            throw ProjectError("Invalid project analysis interval");
        next.current = current.value_or(
            core::Epoch{std::clamp(next.current.tai_nanoseconds(), next.first.tai_nanoseconds(),
                                   next.last.tai_nanoseconds())});
        for (auto &s : next.satellites) {
            s.orbit.first = next.first;
            s.orbit.last = next.last;
        }
    });
}
void ProjectEdit::earth_orientation(std::optional<core::EopDataInfo> data) {
    objects().mutate([&](Project &next) { next.earth_orientation = std::move(data); });
}
void ProjectEdit::analysis_data(std::optional<core::EopDataInfo> eop,
                                std::optional<core::TimeDataInfo> time,
                                std::optional<AnalysisRecord> record,
                                std::vector<DataReference> forces) {
    objects().mutate([&](Project &next) {
        next.earth_orientation = std::move(eop);
        next.time_data = std::move(time);
        next.analysis_record = std::move(record);
        next.force_data = std::move(forces);
    });
}
void ProjectEdit::reports(std::vector<ReportDefinition> reports) {
    objects().mutate([&](Project &next) { next.reports = std::move(reports); });
}
void ProjectEdit::commit() {
    require_open(*edit_);
    edit_->closed = true;
    const auto &before = *edit_->base.project;
    const auto &after = *edit_->candidate;
    validate_project(after);
    const bool changed = encode_project(before) != encode_project(after);
    ChangeSet changes{edit_->base.revision,
                      edit_->base.revision + (changed ? 1 : 0),
                      {},
                      {},
                      {},
                      false,
                      edit_->base,
                      {edit_->base.revision + (changed ? 1 : 0), edit_->candidate, state_->scope}};
    if (changed) {
        diff_objects(before.satellites, after.satellites, changes);
        diff_objects(before.ground_stations, after.ground_stations, changes);
        diff_objects(before.sensors, after.sensors, changes);
        changes.settings_changed = metadata_bytes(before) != metadata_bytes(after);
    }
    bool dispatch = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->revision != edit_->base.revision)
            throw RevisionConflictError("Project changed since this edit began; start a new edit");
        if (!changed)
            return;
        detail::DocumentState::Notification pending{std::move(changes), {}};
        for (const auto &[token, observer] : state_->observers) {
            (void)token;
            pending.observers.push_back(observer);
        }
        state_->pending.push_back(std::move(pending));
        state_->project = edit_->candidate;
        ++state_->revision;
        dispatch = !state_->notifying;
        state_->notifying = true;
    }
    if (dispatch)
        notify(state_);
}

ProjectSnapshot ObjectManager::snapshot() const {
    if (edit_) {
        require_open(*edit_);
        return {edit_->base.revision, edit_->candidate, state_->scope};
    }
    std::lock_guard lock(state_->mutex);
    return {state_->revision, state_->project, state_->scope};
}
void ObjectManager::check_ref(const ObjectRef &ref, const Project &project,
                              std::optional<ObjectKind> kind) const {
    if (ref.scope_ != state_->scope)
        throw ProjectError("Object '" + ref.id() + "' belongs to another project");
    if (kind && ref.kind() != *kind)
        throw ProjectError("Wrong object type for '" + ref.id() + "'");
    (void)read_object(project, ref.id(), ref.kind());
}
ObjectRef ObjectManager::get(const std::string &id) const {
    const auto snap = snapshot();
    for (const auto &s : snap.project->satellites)
        if (id_of(s) == id)
            return {state_->scope, id, ObjectKind::satellite};
    for (const auto &s : snap.project->ground_stations)
        if (id_of(s) == id)
            return {state_->scope, id, ObjectKind::ground_station};
    for (const auto &s : snap.project->sensors)
        if (id_of(s) == id)
            return {state_->scope, id, ObjectKind::sensor};
    throw ObjectNotFoundError("Object '" + id + "' no longer exists");
}
std::vector<ObjectRef> ObjectManager::find(const std::optional<std::string> &name,
                                           std::optional<ObjectKind> kind) const {
    const auto snap = snapshot();
    std::vector<ObjectRef> found;
    const auto append = [&](const auto &values, ObjectKind k) {
        if (kind && kind != k)
            return;
        for (const auto &s : values)
            if (!name || s.name() == *name)
                found.push_back({state_->scope, id_of(s), k});
    };
    append(snap.project->satellites, ObjectKind::satellite);
    append(snap.project->ground_stations, ObjectKind::ground_station);
    append(snap.project->sensors, ObjectKind::sensor);
    return found;
}
ObjectState ObjectManager::read(const ObjectRef &ref) const {
    const auto snap = snapshot();
    check_ref(ref, *snap.project);
    return read_object(*snap.project, ref.id(), ref.kind());
}
std::vector<ObjectRef> ObjectManager::children(const ObjectRef &parent) const {
    const auto snap = snapshot();
    check_ref(parent, *snap.project);
    std::vector<ObjectRef> result;
    for (const auto &s : snap.project->sensors)
        if (s.parent_satellite_id == parent.id())
            result.push_back({state_->scope, s.object_id, ObjectKind::sensor});
    return result;
}
ObjectRef ObjectManager::allocate(ObjectKind kind) const {
    std::lock_guard lock(state_->mutex);
    return {state_->scope, state_->prefix + std::to_string(++state_->next_id), kind};
}
void ObjectManager::mutate(const std::function<void(Project &)> &operation) {
    if (!edit_) {
        ProjectEdit transaction{state_};
        transaction.objects().mutate(operation);
        transaction.commit();
        return;
    }
    require_open(*edit_);
    auto next = std::make_shared<Project>(*edit_->candidate);
    operation(*next);
    validate_project(*next, false);
    edit_->candidate = std::move(next);
}
ObjectRef ObjectManager::create(const objects::Satellite &definition) {
    const auto ref = allocate(ObjectKind::satellite);
    mutate([&](Project &next) {
        ProjectSatellite value;
        value.orbit = definition.orbit;
        value.display_name = definition.name;
        value.display = definition.display;
        value.orbit.object_id = ref.id();
        value.orbit.first = next.first;
        value.orbit.last = next.last;
        next.satellites.push_back(std::move(value));
    });
    return ref;
}
ObjectRef ObjectManager::create(const objects::GroundStation &definition) {
    const auto ref = allocate(ObjectKind::ground_station);
    mutate([&](Project &next) {
        next.ground_stations.push_back(
            {ref.id(), definition.name, definition.location, definition.minimum_elevation_rad});
    });
    return ref;
}
ObjectRef ObjectManager::create(const objects::Sensor &definition) {
    const auto ref = allocate(ObjectKind::sensor);
    mutate([&](Project &next) {
        check_ref(definition.parent, next, ObjectKind::satellite);
        ProjectSensor value{ref.id(),
                            definition.name,
                            definition.parent.id(),
                            definition.half_angle_rad,
                            SensorPointing::nadir,
                            {}};
        if (definition.pointing.target) {
            check_ref(*definition.pointing.target, next, ObjectKind::ground_station);
            value.pointing = SensorPointing::targeted;
            value.target_station_id = definition.pointing.target->id();
        }
        next.sensors.push_back(std::move(value));
    });
    return ref;
}
void ObjectManager::update(const ObjectRef &ref, const objects::SatelliteUpdate &patch) {
    mutate([&](Project &next) {
        check_ref(ref, next, ObjectKind::satellite);
        auto &value = lookup(next.satellites, ref.id());
        if (patch.name && *patch.name != value.name())
            value.display_name = *patch.name;
        if (patch.orbit) {
            value.orbit = *patch.orbit;
            value.orbit.object_id = ref.id();
            value.orbit.first = next.first;
            value.orbit.last = next.last;
        }
        if (patch.display)
            value.display = *patch.display;
    });
}
void ObjectManager::update(const ObjectRef &ref, const objects::GroundStationUpdate &patch) {
    mutate([&](Project &next) {
        check_ref(ref, next, ObjectKind::ground_station);
        auto &value = lookup(next.ground_stations, ref.id());
        if (patch.name && *patch.name != value.name())
            value.display_name = *patch.name;
        if (patch.location)
            value.location = *patch.location;
        if (patch.minimum_elevation_rad)
            value.minimum_elevation_rad = *patch.minimum_elevation_rad;
    });
}
void ObjectManager::update(const ObjectRef &ref, const objects::SensorUpdate &patch) {
    mutate([&](Project &next) {
        check_ref(ref, next, ObjectKind::sensor);
        auto &value = lookup(next.sensors, ref.id());
        if (patch.name && *patch.name != value.name())
            value.display_name = *patch.name;
        if (patch.half_angle_rad)
            value.half_angle_rad = *patch.half_angle_rad;
        if (patch.pointing) {
            const auto &target = patch.pointing->target;
            if (target)
                check_ref(*target, next, ObjectKind::ground_station);
            value.pointing = target ? SensorPointing::targeted : SensorPointing::nadir;
            value.target_station_id = target ? target->id() : "";
        }
    });
}
void ObjectManager::rename(const ObjectRef &ref, const std::string &name) {
    switch (ref.kind()) {
    case ObjectKind::satellite:
        update(ref, objects::SatelliteUpdate{name, {}, {}});
        break;
    case ObjectKind::ground_station:
        update(ref, objects::GroundStationUpdate{name, {}, {}});
        break;
    case ObjectKind::sensor:
        update(ref, objects::SensorUpdate{name, {}, {}});
        break;
    }
}
void ObjectManager::reparent(const ObjectRef &sensor, const ObjectRef &parent) {
    mutate([&](Project &next) {
        check_ref(sensor, next, ObjectKind::sensor);
        check_ref(parent, next, ObjectKind::satellite);
        lookup(next.sensors, sensor.id()).parent_satellite_id = parent.id();
    });
}
ObjectRef ObjectManager::duplicate(const ObjectRef &ref, const std::string &name) {
    const auto copy = allocate(ref.kind());
    mutate([&](Project &next) {
        check_ref(ref, next);
        std::visit(
            [&](auto value) {
                value.display_name = name;
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ProjectSatellite>) {
                    value.orbit.object_id = copy.id();
                    next.satellites.push_back(value);
                    std::vector<ProjectSensor> children;
                    for (auto child : next.sensors)
                        if (child.parent_satellite_id == ref.id()) {
                            child.object_id = allocate(ObjectKind::sensor).id();
                            child.parent_satellite_id = copy.id();
                            children.push_back(std::move(child));
                        }
                    next.sensors.insert(next.sensors.end(), children.begin(), children.end());
                } else {
                    value.object_id = copy.id();
                    if constexpr (std::is_same_v<T, ProjectGroundStation>)
                        next.ground_stations.push_back(value);
                    else
                        next.sensors.push_back(value);
                }
            },
            read_object(next, ref.id(), ref.kind()));
    });
    return copy;
}
RemovalPlan ObjectManager::plan_remove(const ObjectRef &ref) const {
    const auto snap = snapshot();
    check_ref(ref, *snap.project);
    RemovalPlan result{snap.revision, {ref.id()}, {}, {}};
    for (const auto &s : snap.project->sensors) {
        if (ref.kind() == ObjectKind::satellite && s.parent_satellite_id == ref.id())
            result.removed.push_back(s.object_id);
        if (ref.kind() == ObjectKind::ground_station && s.pointing == SensorPointing::targeted &&
            s.target_station_id == ref.id())
            result.blocking_sensors.push_back(s.object_id);
    }
    for (const auto &r : snap.project->reports)
        if (std::ranges::any_of(r.query.object_ids, [&](const auto &id) {
                return std::ranges::find(result.removed, id) != result.removed.end();
            }))
            result.affected_reports.push_back(r.id);
    return result;
}
void ObjectManager::remove(const ObjectRef &ref) {
    mutate([&](Project &next) {
        check_ref(ref, next);
        std::erase_if(next.satellites, [&](const auto &s) { return id_of(s) == ref.id(); });
        std::erase_if(next.ground_stations, [&](const auto &s) { return id_of(s) == ref.id(); });
        std::erase_if(next.sensors, [&](const auto &s) {
            return id_of(s) == ref.id() || s.parent_satellite_id == ref.id();
        });
    });
}
} // namespace orbitforge::application
