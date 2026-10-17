#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <orbitforge/application/project.hpp>
#include <orbitforge/application/scenario.hpp>
#include <orbitforge/core/frames/frame_provider.hpp>
#include <orbitforge/results/table.hpp>
#include <stdexcept>

namespace orbitforge::runtime {
inline constexpr std::size_t max_report_rows = 200000;
struct ReportContext {
    application::Project project;
    // Owning snapshots; calculation builds sessions from this exact project.
    std::shared_ptr<const core::ITimeProvider> time;
    std::shared_ptr<const core::IFrameProvider> frames;
    std::shared_ptr<const core::IEarthOrientationProvider> eop;
    std::map<std::string, std::shared_ptr<const core::GravityModel>> gravity{};
    std::map<std::string, std::shared_ptr<const core::IThirdBodyEphemeris>> ephemerides{};
    std::map<std::string, std::shared_ptr<const core::SpaceWeather>> weather{};
    std::map<std::string, std::shared_ptr<const core::OceanTideModel>> ocean_tides{};
    [[nodiscard]] core::HpopEnvironment
    force_environment(const application::OrbitInput &input) const;
};
struct ReportResult {
    application::ReportDefinition definition;
    results::Table table;
    std::string input_sha256, data_sha256, time_sha256;
    std::string algorithm = "builtin-reports-v2";
    core::Epoch first{0}, last{0};
    std::shared_ptr<const core::ITimeProvider> time;
    [[nodiscard]] std::string column_title(std::size_t column) const;
    [[nodiscard]] std::string column_unit(std::size_t column) const;
    // Display-unit numeric value shared by tables, CSV and plots; never parses formatted text.
    [[nodiscard]] std::optional<double> numeric_value(std::size_t row, std::size_t column) const;
    [[nodiscard]] std::string cell_text(std::size_t row, std::size_t column) const;
    // Captured document revision for Analysis queries; zero for standalone legacy contexts.
    std::uint64_t input_revision = 0;
    // Canonical SI table retained so a later template can restore omitted fields.
    std::shared_ptr<const results::Table> source_table = {};
    [[nodiscard]] ReportResult with_template(const application::ReportTemplate &format) const;
};
class ReportCancelled : public std::runtime_error {
  public:
    ReportCancelled() : std::runtime_error("Report generation cancelled") {}
};
// Excludes saved reports, playback time and purely graphical settings.
[[nodiscard]] std::string report_input_sha256(const application::Project &project);
[[nodiscard]] std::size_t estimate_report_rows(const application::Project &project,
                                               const application::ReportDefinition &definition);
[[nodiscard]] ReportResult
generate_report(const ReportContext &context, const application::ReportDefinition &definition,
                const std::atomic_bool *cancel = nullptr,
                const std::function<void(std::size_t, std::size_t)> &progress = {});
// RFC-style quoted UTF-8 CSV with a BOM, exact epoch text and round-trip numerical precision.
// Atomic publication. Cancellation/failure leaves the old destination untouched.
void export_report_csv(const ReportResult &result, const std::filesystem::path &path,
                       bool overwrite = false, const std::atomic_bool *cancel = nullptr);
} // namespace orbitforge::runtime
