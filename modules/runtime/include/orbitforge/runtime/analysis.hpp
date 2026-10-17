#pragma once
#include <chrono>
#include <functional>
#include <map>
#include <orbitforge/application/project_document.hpp>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/results/access_windows.hpp>
#include <orbitforge/results/aer_series.hpp>
#include <orbitforge/results/maneuver_series.hpp>
#include <orbitforge/results/trajectory.hpp>
#include <orbitforge/runtime/report.hpp>

namespace orbitforge::runtime {
class DataCatalog;
class AnalysisCapabilityError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};
class AnalysisCancelled : public std::runtime_error {
  public:
    AnalysisCancelled() : std::runtime_error("Analysis query cancelled") {}
};
struct AccessOptions {
    std::chrono::nanoseconds search_step;
    std::chrono::nanoseconds boundary_tolerance{1'000'000};
    std::optional<core::Epoch> first = {}, last = {};
};

// Already selected, immutable resources. No directory lookup or network during queries.
// Loaders fully own their parsed bytes and the built-in frame model.
class DataSnapshot {
  public:
    DataSnapshot() = default;
    [[nodiscard]] static DataSnapshot builtin_time();
    // C++ adapter entry point for already loaded, immutable providers. Retains
    // shared ownership; never reloads files. Providers must stay immutable and
    // support concurrent reads. EOP and time must identify the same UTC resource.
    // Empty, time-only and full triples are valid. Incomplete triples or different
    // UTC identities throw invalid_argument; Analysis preparation checks coverage.
    [[nodiscard]] static DataSnapshot
    from_providers(std::shared_ptr<const core::ITimeProvider> time,
                   std::shared_ptr<const core::IFrameProvider> frames = {},
                   std::shared_ptr<const core::IEarthOrientationProvider> eop = {});
    // An EOP file uses the explicit time_file, or the built-in time table when omitted.
    // At least one path is required. Both files are parsed now, never reopened by Analysis.
    [[nodiscard]] static DataSnapshot load(const std::filesystem::path &eop_manifest = {},
                                           const std::filesystem::path &time_file = {},
                                           int extrapolation_days = 0);
    [[nodiscard]] const auto &time() const { return time_; }
    [[nodiscard]] const auto &frames() const { return frames_; }
    [[nodiscard]] const auto &eop() const { return eop_; }
    [[nodiscard]] DataSnapshot with_gravity(const std::string &selection,
                                            std::shared_ptr<const core::GravityModel> model) const;
    [[nodiscard]] DataSnapshot with_gravity_file(const std::string &selection,
                                                 const std::filesystem::path &file) const;
    [[nodiscard]] const auto &gravity_models() const { return gravity_; }
    [[nodiscard]] DataSnapshot
    with_ephemeris(const std::string &selection,
                   std::shared_ptr<const core::IThirdBodyEphemeris> ephemeris) const;
    [[nodiscard]] DataSnapshot with_ephemeris_file(const std::string &selection,
                                                   const std::filesystem::path &manifest,
                                                   const std::filesystem::path &workspace) const;
    [[nodiscard]] const auto &ephemerides() const { return ephemerides_; }
    [[nodiscard]] DataSnapshot
    with_space_weather(const std::string &selection,
                       std::shared_ptr<const core::SpaceWeather> weather) const;
    [[nodiscard]] DataSnapshot with_space_weather_file(const std::string &selection,
                                                       const std::filesystem::path &file) const;
    [[nodiscard]] const auto &weather_sources() const { return weather_; }
    [[nodiscard]] DataSnapshot
    with_ocean_tides(const std::string &selection,
                     std::shared_ptr<const core::OceanTideModel> model) const;
    [[nodiscard]] DataSnapshot with_ocean_tide_file(const std::string &selection,
                                                    const std::filesystem::path &file) const;
    [[nodiscard]] const auto &ocean_tide_models() const { return ocean_tides_; }

    [[nodiscard]] core::HpopEnvironment
    force_environment(const application::OrbitInput &input) const;
    [[nodiscard]] const application::DataLock &data_lock() const { return lock_; }
    // Catalog diagnostics do not participate in the calculation identity.
    [[nodiscard]] const std::vector<std::string> &selection_notes() const {
        return selection_notes_;
    }

  private:
    explicit DataSnapshot(std::shared_ptr<const core::ITimeProvider> time,
                          std::shared_ptr<const core::IFrameProvider> frames = {},
                          std::shared_ptr<const core::IEarthOrientationProvider> eop = {});
    std::shared_ptr<const core::ITimeProvider> time_;
    std::shared_ptr<const core::IFrameProvider> frames_;
    std::shared_ptr<const core::IEarthOrientationProvider> eop_;
    std::map<std::string, std::shared_ptr<const core::GravityModel>> gravity_;
    std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> ephemerides_;
    std::map<std::string, std::shared_ptr<const core::SpaceWeather>> weather_;
    std::map<std::string, std::shared_ptr<const core::OceanTideModel>> ocean_tides_;
    application::DataLock lock_;
    std::vector<std::string> selection_notes_;
    friend class DataCatalog;
};

inline constexpr std::size_t max_analysis_samples = 200000;
class AnalysisPlan;
class Analysis {
  public:
    [[nodiscard]] results::ManeuverSeries
    maneuvers(const application::ObjectRef &satellite,
              const std::function<bool()> &cancelled = {}) const;
    [[nodiscard]] const application::ProjectSnapshot &snapshot() const;
    [[nodiscard]] const DataSnapshot &data() const;
    [[nodiscard]] core::CartesianState state(const application::ObjectRef &object, core::Epoch at,
                                             core::FrameId frame) const;
    // Explicit queries preserve order and duplicates, and accept an empty span.
    [[nodiscard]] results::Trajectory states(const application::ObjectRef &object,
                                             std::span<const core::Epoch> times,
                                             core::FrameId frame) const;
    // Positive, exact TAI step. Includes both endpoints, also when not evenly divisible.
    // A single-epoch subinterval is valid. The safety limit is checked before allocation.
    [[nodiscard]] results::Trajectory states(const application::ObjectRef &object,
                                             std::chrono::nanoseconds step, core::FrameId frame,
                                             std::optional<core::Epoch> first = {},
                                             std::optional<core::Epoch> last = {}) const;
    // Station -> satellite/sensor. Reuses prepared sessions and selected UTC/EOP data.
    // Fixed discovery grid; windows/gaps shorter than search_step may be missed.
    // Checks cancelled at each evaluation (including refinement); never returns partial results.
    [[nodiscard]] results::AccessWindows access(const application::ObjectRef &station,
                                                const application::ObjectRef &target,
                                                const AccessOptions &options,
                                                const std::function<bool()> &cancelled = {}) const;
    // Samples all requested epochs, including when the target is not visible.
    [[nodiscard]] results::AerSeries aer(const application::ObjectRef &station,
                                         const application::ObjectRef &target,
                                         std::span<const core::Epoch> times,
                                         const std::function<bool()> &cancelled = {}) const;
    [[nodiscard]] results::AerSeries
    aer(const application::ObjectRef &station, const application::ObjectRef &target,
        std::chrono::nanoseconds step, std::optional<core::Epoch> first = {},
        std::optional<core::Epoch> last = {}, const std::function<bool()> &cancelled = {}) const;
    // Existing report templates, evaluated against this captured project and its data.
    [[nodiscard]] ReportResult
    report(const application::ReportDefinition &definition,
           const std::function<bool()> &cancelled = {},
           const std::function<void(std::size_t, std::size_t)> &progress = {}) const;
    [[nodiscard]] ReportResult
    report(const application::ReportQuery &query, const application::ReportTemplate &format = {},
           const std::function<bool()> &cancelled = {},
           const std::function<void(std::size_t, std::size_t)> &progress = {}) const;

  private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    Analysis(application::ProjectSnapshot snapshot, DataSnapshot data);
    friend class AnalysisPlan;
};
struct DataChange {
    std::string role;
    application::DataReference recorded, selected;
};
class DataChangedError : public std::runtime_error {
  public:
    explicit DataChangedError(AnalysisPlan plan);
    [[nodiscard]] const AnalysisPlan &plan() const { return *plan_; }

  private:
    std::shared_ptr<const AnalysisPlan> plan_;
};
// Preparing a plan validates its exact resources and creates its immutable sessions.
// Accepting a data change never bypasses coverage/format/revision validation.
class AnalysisPlan {
  public:
    AnalysisPlan(const application::ProjectDocument &project, DataSnapshot data = {});
    AnalysisPlan(const application::ProjectDocument &project, const DataCatalog &catalog);
    [[nodiscard]] const std::vector<DataChange> &data_changes() const { return changes_; }
    [[nodiscard]] const application::ProjectSnapshot &snapshot() const {
        return analysis_.snapshot();
    }
    [[nodiscard]] const DataSnapshot &data() const { return analysis_.data(); }
    [[nodiscard]] Analysis prepare(bool accept_data_changes = false) const;

  private:
    AnalysisPlan(const application::ProjectDocument &project, Analysis analysis);
    application::ProjectDocument owner_;
    Analysis analysis_;
    std::vector<DataChange> changes_;
};
[[nodiscard]] Analysis prepare(const application::ProjectDocument &project, DataSnapshot data = {});
[[nodiscard]] Analysis prepare(const application::ProjectDocument &project,
                               const DataCatalog &catalog);
// Save the current inputs with the supplied analysis's exact data provenance.
// Names/appearance may change; physical inputs and project identity must match.
void save_project(const application::ProjectDocument &project, const std::filesystem::path &path,
                  const Analysis &analysis, bool overwrite = false);

class ProjectOpenPlan {
  public:
    ProjectOpenPlan(const std::filesystem::path &path, DataSnapshot data = {});
    ProjectOpenPlan(const std::filesystem::path &path, const DataCatalog &catalog);
    [[nodiscard]] const auto &data_changes() const { return plan_.data_changes(); }
    [[nodiscard]] const auto &data() const { return plan_.data(); }
    [[nodiscard]] const auto &snapshot() const { return plan_.snapshot(); }
    // Each call returns an independent document. Files/catalogs are not reopened.
    // Changed data marks the new document dirty; no results are persisted implicitly.
    [[nodiscard]] application::ProjectDocument open(bool accept_data_changes = false) const;

  private:
    explicit ProjectOpenPlan(AnalysisPlan plan);
    AnalysisPlan plan_;
};
class OpenDataChangedError : public std::runtime_error {
  public:
    explicit OpenDataChangedError(ProjectOpenPlan plan);
    [[nodiscard]] const ProjectOpenPlan &plan() const { return *plan_; }

  private:
    std::shared_ptr<const ProjectOpenPlan> plan_;
};
[[nodiscard]] application::ProjectDocument open_project(const std::filesystem::path &path,
                                                        DataSnapshot data = {});
[[nodiscard]] application::ProjectDocument open_project(const std::filesystem::path &path,
                                                        const DataCatalog &catalog);
// Shares the report CSV formatter and atomic publisher, without any recomputation.
void export_trajectory_csv(const results::Trajectory &result, const std::filesystem::path &path,
                           bool overwrite = false);
void export_access_csv(const results::AccessWindows &result, const std::filesystem::path &path,
                       bool overwrite = false);
void export_aer_csv(const results::AerSeries &result, const std::filesystem::path &path,
                    bool overwrite = false);
} // namespace orbitforge::runtime
