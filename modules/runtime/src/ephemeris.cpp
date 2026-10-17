#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <orbitforge/core/propagation/solar_radiation_pressure.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <picosha2.h>
#include <sstream>
#ifdef ORBITFORGE_HAS_SPICE
#include <orbitforge/providers/spice/provider.hpp>
#endif

namespace orbitforge::runtime {
core::CbGravity load_cb_gravity(const std::filesystem::path &path, core::ThirdBody body,
                                double mu_scale) {
    const auto lower = [](std::string value) {
        std::ranges::transform(value, value.begin(),
                               [](unsigned char c) { return std::tolower(c); });
        return value;
    };
    if (!std::isfinite(mu_scale) || mu_scale <= 0)
        throw std::invalid_argument("Cb file GM unit scale must be finite and positive");
    if (lower(path.stem().string()) != lower(std::string(core::third_body_name(body))))
        throw std::invalid_argument(
            "Cb file name must match the selected body (for example Sun.cb)");
    if (std::filesystem::file_size(path) > 1024 * 1024)
        throw std::invalid_argument("Cb file exceeds the 1 MiB limit");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read Cb file");
    const std::string bytes{std::istreambuf_iterator<char>(file), {}};
    if (bytes.find('\0') != std::string::npos)
        throw std::invalid_argument("Cb file is not text");
    std::istringstream lines(bytes);
    std::string line;
    std::optional<double> gm, system_gm;
    while (std::getline(lines, line)) {
        if (auto comment = line.find('#'); comment != std::string::npos)
            line.resize(comment);
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string key;
        row >> key;
        key = lower(key);
        if (key != "gm" && key != "systemgm")
            continue;
        std::string token, extra;
        row >> token;
        if (token == "=")
            row >> token;
        if (token.empty() || row >> extra)
            throw std::invalid_argument("Invalid Cb file GM record");
        std::ranges::replace(token, 'D', 'e');
        std::ranges::replace(token, 'd', 'e');
        std::istringstream number(token);
        number.imbue(std::locale::classic());
        double value = 0;
        if (!(number >> value) || number.peek() != std::char_traits<char>::eof() ||
            !std::isfinite(value * mu_scale) || value * mu_scale <= 0)
            throw std::invalid_argument("Cb file GM must be finite and positive");
        auto &destination = key == "gm" ? gm : system_gm;
        if (destination)
            throw std::invalid_argument("Duplicate Cb file GM record");
        destination = value * mu_scale;
    }
    const auto id = core::third_body_naif_id(body);
    const bool system = id >= 4 && id <= 9;
    if (system && !system_gm)
        throw std::invalid_argument("SystemGm is required for a DE440 planetary system barycentre");
    if (!system && !gm)
        throw std::invalid_argument("Cb file requires Gm for Sun/Moon/Mercury/Venus");
    // Earth's parent is the Sun: use its body-centred Gm. Otherwise prefer
    // a declared system value, as in the Earth HPOP Cb-file convention.
    const auto selected = body != core::ThirdBody::sun && system_gm ? *system_gm : *gm;
    return {selected, path.filename().string(), picosha2::hash256_hex_string(bytes), body};
}
namespace {
class ThirdBodyEphemeris final : public core::IThirdBodyEphemeris {
    std::shared_ptr<const core::IEphemerisProvider> source_;
    core::ThirdBodyDataInfo info_;

  public:
    explicit ThirdBodyEphemeris(std::shared_ptr<const core::IEphemerisProvider> source)
        : source_(std::move(source)) {
        if (!source_)
            throw std::invalid_argument("An ephemeris provider is required");
        const auto &info = source_->data_info();
        if (!info.model.starts_with("DE440") ||
            std::none_of(info.kernels.begin(), info.kernels.end(),
                         [](const auto &k) { return k.kind == "SPK"; }) ||
            std::any_of(info.kernels.begin(), info.kernels.end(), [](const auto &k) {
                return k.kind != "LSK" && k.kind != "SPK_BODY_CENTRES" &&
                       (k.kind != "SPK" || k.source_sha256 != de440s_sha256);
            }))
            throw core::EphemerisDataError(
                "HPOP requires DE440s base ephemerides and validated planet-centre supplements");
        const bool supplemented = std::ranges::any_of(
            info.kernels, [](const auto &kernel) { return kernel.kind == "SPK_BODY_CENTRES"; });
        info_ = {
            info.model,
            info.manifest_sha256,
            std::string(supplemented
                            ? "de440-newtonian-geocentric-icrf-none-gm440-planets-tai-rate-v4;"
                              "body-centres=filtered-spk-v1;"
                            : "de440-newtonian-geocentric-icrf-none-gm440-planets-tai-rate-v3;") +
                info.toolkit_version,
            info.first_epoch,
            info.last_epoch,
            de440_sun_mu,
            de440_moon_mu,
            de440_planet_mu};
    }
    std::array<double, 3> position(core::ThirdBody body, core::Epoch at) const override {
        const auto state = source_->geometric_state(core::third_body_naif_id(body), 399, at);
        if (state.frame != core::FrameId::spice_j2000)
            throw core::EphemerisDataError("DE440 force ephemeris returned unexpected axes");
        return state.position_m;
    }
    std::array<double, 3> eclipsing_body_position(core::ThirdBody body,
                                                  core::Epoch at) const override {
        const auto target = core::srp_eclipsing_body_naif_id(body);
        const auto state = source_->geometric_state(target, 399, at);
        if (state.frame != core::FrameId::spice_j2000 || state.epoch != at ||
            state.target_naif_id != target || state.observer_naif_id != 399)
            throw core::EphemerisDataError(
                "SRP ephemeris returned an unexpected body centre or frame");
        return state.position_m;
    }
    void validate_eclipsing_body_interval(core::Epoch first, core::Epoch last,
                                          std::span<const core::ThirdBody> bodies) const override {
        std::vector<std::int32_t> ids;
        for (auto body : bodies)
            ids.push_back(core::srp_eclipsing_body_naif_id(body));
        source_->validate_earth_gravity_interval(first, last, ids);
    }
    core::GeometricBodyState geometric_state(core::ThirdBody body, core::Epoch at) const override {
        const auto state = source_->geometric_state(core::third_body_naif_id(body), 399, at);
        if (state.frame != core::FrameId::spice_j2000 ||
            state.velocity_time_scale != core::TimeScale::tdb || state.epoch != at ||
            state.target_naif_id != core::third_body_naif_id(body) || state.observer_naif_id != 399)
            throw core::EphemerisDataError(
                "DE440 force ephemeris returned an unexpected state convention");
        const double rate = source_->tdb_seconds_per_tai_second(at);
        if (!std::isfinite(rate) || rate <= 0)
            throw core::EphemerisDataError(
                "DE440 force ephemeris returned an invalid TDB/TAI rate");
        auto velocity = state.velocity_mps;
        for (double &component : velocity)
            component *= rate;
        return {state.position_m, velocity};
    }
    std::array<double, 3> apparent_sun_from_earth(core::Epoch at) const override {
        return source_->apparent_sun_from_earth(at);
    }
    std::array<double, 3>
    apparent_sun_from_spacecraft(core::Epoch at, const std::array<double, 3> &position,
                                 const std::array<double, 3> &velocity) const override {
        return source_->apparent_sun_from_spacecraft(at, position, velocity);
    }
    void validate_spacecraft_apparent_sun_interval(core::Epoch first,
                                                   core::Epoch last) const override {
        source_->validate_spacecraft_apparent_sun_interval(first, last);
    }
    void validate_apparent_sun_interval(core::Epoch first, core::Epoch last) const override {
        source_->validate_apparent_sun_interval(first, last);
    }
    void validate_interval(core::Epoch first, core::Epoch last,
                           std::span<const core::ThirdBody> bodies) const override {
        if (first.tai_nanoseconds() < info_.first.tai_nanoseconds() ||
            last.tai_nanoseconds() > info_.last.tai_nanoseconds())
            throw core::EphemerisCoverageError(
                "Third-body ephemeris coverage is insufficient. Required TAI ns [" +
                std::to_string(first.tai_nanoseconds()) + ", " +
                std::to_string(last.tai_nanoseconds()) + "]; available [" +
                std::to_string(info_.first.tai_nanoseconds()) + ", " +
                std::to_string(info_.last.tai_nanoseconds()) +
                "]. Select covering ephemerides in Data Center or adjust the orbit interval; no "
                "extrapolation.");
        std::vector<std::int32_t> ids;
        for (auto body : bodies)
            ids.push_back(core::third_body_naif_id(body));
        source_->validate_earth_gravity_interval(first, last, ids);
    }
    const core::ThirdBodyDataInfo &data_info() const noexcept override { return info_; }
};
} // namespace
std::shared_ptr<const core::IThirdBodyEphemeris>
make_third_body_ephemeris(std::shared_ptr<const core::IEphemerisProvider> source) {
    return std::make_shared<const ThirdBodyEphemeris>(std::move(source));
}
application::DataReference ephemeris_reference(const std::string &selection,
                                               const core::ThirdBodyDataInfo &info) {
    return {"ephemeris/" + selection, info.model, info.content_sha256, info.interpretation_version,
            picosha2::hash256_hex_string(info.interpretation_version)};
}
void validate_ephemeris_reference(std::span<const application::DataReference> recorded,
                                  const std::string &selection,
                                  const core::ThirdBodyDataInfo &info) {
    const auto selected = ephemeris_reference(selection, info);
    const auto found =
        std::ranges::find(recorded, selected.role, &application::DataReference::role);
    if (found == recorded.end() || *found != selected)
        throw std::invalid_argument(
            "Ephemeris differs from the project's confirmed data; use a preparation plan");
}
bool ephemeris_available() noexcept {
#ifdef ORBITFORGE_HAS_SPICE
    return true;
#else
    return false;
#endif
}
std::unique_ptr<core::IEphemerisProvider>
load_spice_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace) {
#ifdef ORBITFORGE_HAS_SPICE
    return providers::spice::load_snapshot(manifest, workspace);
#else
    (void)manifest;
    (void)workspace;
    throw std::runtime_error("This build does not include the CSPICE ephemeris provider");
#endif
}
} // namespace orbitforge::runtime
