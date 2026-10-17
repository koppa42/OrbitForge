#pragma once
#include <functional>
#include <memory>
#include <orbitforge/application/project.hpp>
#include <variant>

namespace orbitforge::application {
enum class ObjectKind { satellite, ground_station, sensor };
class ObjectNotFoundError : public ProjectError {
    using ProjectError::ProjectError;
};
class RevisionConflictError : public ProjectError {
    using ProjectError::ProjectError;
};
class EditClosedError : public ProjectError {
    using ProjectError::ProjectError;
};
namespace detail {
struct DocumentState;
struct EditState;
} // namespace detail

// A value identity, never a pointer into a vector. Only a manager can mint one.
class ObjectRef {
  public:
    [[nodiscard]] const std::string &id() const { return id_; }
    [[nodiscard]] ObjectKind kind() const { return kind_; }
    bool operator==(const ObjectRef &) const = default;

  private:
    ObjectRef(std::uint64_t scope, std::string id, ObjectKind kind)
        : scope_(scope), id_(std::move(id)), kind_(kind) {}
    std::uint64_t scope_;
    std::string id_;
    ObjectKind kind_;
    friend class ObjectManager;
    friend struct ProjectSnapshot;
};

namespace objects {
struct Satellite {
    std::string name;
    OrbitInput orbit = {};
    OrbitDisplay display = {};
};
struct GroundStation {
    std::string name;
    core::GroundStationLocation location = {};
    double minimum_elevation_rad = 0;
};
// A complete pointing value: null target means geocentric nadir.
struct Pointing {
    std::optional<ObjectRef> target = {};
};
struct Sensor {
    std::string name;
    ObjectRef parent;
    double half_angle_rad = 0.7853981633974483;
    Pointing pointing = {};
};
struct SatelliteUpdate {
    std::optional<std::string> name = {};
    std::optional<OrbitInput> orbit = {};
    std::optional<OrbitDisplay> display = {};
};
struct GroundStationUpdate {
    std::optional<std::string> name = {};
    std::optional<core::GroundStationLocation> location = {};
    std::optional<double> minimum_elevation_rad = {};
};
struct SensorUpdate {
    std::optional<std::string> name = {};
    std::optional<double> half_angle_rad = {};
    std::optional<Pointing> pointing = {};
};
} // namespace objects

using ObjectState = std::variant<ProjectSatellite, ProjectGroundStation, ProjectSensor>;
struct ProjectSnapshot {
    std::uint64_t revision;
    std::shared_ptr<const Project> project;
    // Process-local document identity, never persisted or part of an input hash.
    std::uint64_t scope = 0;
    [[nodiscard]] bool owns(const ObjectRef &ref) const noexcept { return scope == ref.scope_; }
    [[nodiscard]] ObjectState read(const ObjectRef &ref) const;
};
struct ObjectChange {
    std::string id;
    // Stable semantic fields, consumed by future dependency/Qt adapters.
    std::vector<std::string> fields;
};
struct ChangeSet {
    std::uint64_t before_revision, revision;
    std::vector<std::string> added, removed;
    std::vector<ObjectChange> updated;
    bool settings_changed = false;
    // Exact snapshots remain available even when an observer commits again.
    ProjectSnapshot before, after;
};
struct RemovalPlan {
    std::uint64_t revision;
    std::vector<std::string> removed, blocking_sensors, affected_reports;
};

class ObjectManager {
  public:
    [[nodiscard]] ObjectRef get(const std::string &id) const;
    [[nodiscard]] std::vector<ObjectRef> find(const std::optional<std::string> &name = {},
                                              std::optional<ObjectKind> kind = {}) const;
    [[nodiscard]] std::vector<ObjectRef> children(const ObjectRef &parent) const;
    [[nodiscard]] ObjectState read(const ObjectRef &ref) const;
    [[nodiscard]] ObjectRef create(const objects::Satellite &definition);
    [[nodiscard]] ObjectRef create(const objects::GroundStation &definition);
    [[nodiscard]] ObjectRef create(const objects::Sensor &definition);
    void update(const ObjectRef &ref, const objects::SatelliteUpdate &patch);
    void update(const ObjectRef &ref, const objects::GroundStationUpdate &patch);
    void update(const ObjectRef &ref, const objects::SensorUpdate &patch);
    void rename(const ObjectRef &ref, const std::string &name);
    [[nodiscard]] ObjectRef duplicate(const ObjectRef &ref, const std::string &name);
    void reparent(const ObjectRef &sensor, const ObjectRef &parent);
    [[nodiscard]] RemovalPlan plan_remove(const ObjectRef &ref) const;
    void remove(const ObjectRef &ref);

  private:
    ObjectManager(std::shared_ptr<detail::DocumentState> state,
                  std::shared_ptr<detail::EditState> edit = {})
        : state_(std::move(state)), edit_(std::move(edit)) {}
    [[nodiscard]] ProjectSnapshot snapshot() const;
    void check_ref(const ObjectRef &ref, const Project &project,
                   std::optional<ObjectKind> kind = {}) const;
    void mutate(const std::function<void(Project &)> &operation);
    [[nodiscard]] ObjectRef allocate(ObjectKind kind) const;
    std::shared_ptr<detail::DocumentState> state_;
    std::shared_ptr<detail::EditState> edit_;
    friend class ProjectDocument;
    friend class ProjectEdit;
};

class ProjectEdit {
  public:
    ProjectEdit(const ProjectEdit &) = delete;
    ProjectEdit &operator=(const ProjectEdit &) = delete;
    ProjectEdit(ProjectEdit &&) noexcept = default;
    ProjectEdit &operator=(ProjectEdit &&) noexcept = delete;
    ~ProjectEdit();
    [[nodiscard]] ObjectManager objects() const;
    [[nodiscard]] ProjectSnapshot snapshot() const;
    void settings(std::optional<std::string> name = {}, std::optional<core::Epoch> first = {},
                  std::optional<core::Epoch> last = {}, std::optional<core::Epoch> current = {});
    // Metadata bridge while data/report editing migrates to the new facade.
    void earth_orientation(std::optional<core::EopDataInfo> data);
    void analysis_data(std::optional<core::EopDataInfo> eop, std::optional<core::TimeDataInfo> time,
                       std::optional<AnalysisRecord> record,
                       std::vector<DataReference> forces = {});
    void reports(std::vector<ReportDefinition> reports);
    // Closes the edit on success, validation failure or revision conflict.
    void commit();
    void discard() noexcept;

  private:
    explicit ProjectEdit(std::shared_ptr<detail::DocumentState> state);
    std::shared_ptr<detail::DocumentState> state_;
    std::shared_ptr<detail::EditState> edit_;
    friend class ProjectDocument;
    friend class ObjectManager;
};

class ProjectDocument {
  public:
    explicit ProjectDocument(Project project);
    [[nodiscard]] ProjectSnapshot snapshot() const;
    [[nodiscard]] ObjectManager objects() const { return ObjectManager{state_}; }
    [[nodiscard]] ProjectEdit edit() const { return ProjectEdit{state_}; }
    [[nodiscard]] bool dirty() const;
    // Only mark the exact revision that was successfully saved.
    void mark_saved(std::uint64_t revision);
    void save(const std::filesystem::path &path, bool overwrite = false) const;
    // Atomically save and publish a validated candidate from this document's
    // current revision. Conflicts and file failures publish no document change.
    void save(const std::filesystem::path &path, const ProjectSnapshot &candidate,
              bool overwrite = false) const;
    // Ordered, synchronous post-publication delivery; callbacks may edit again.
    // Throwing observers are isolated and reported by observer_errors().
    [[nodiscard]] std::uint64_t subscribe(std::function<void(const ChangeSet &)> observer);
    void unsubscribe(std::uint64_t token);
    [[nodiscard]] std::vector<std::string> observer_errors() const;

  private:
    std::shared_ptr<detail::DocumentState> state_;
};
} // namespace orbitforge::application
