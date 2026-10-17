#pragma once
#include <orbitforge/core/frames/state.hpp>
#include <orbitforge/core/time/time_provider.hpp>
#include <span>
#include <vector>

namespace orbitforge::core {
class EphemerisDataError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class EphemerisQueryError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
class EphemerisCoverageError : public std::out_of_range {
  public:
    using std::out_of_range::out_of_range;
};

// Double precision TDB seconds past 2000-01-01 12:00:00 TDB (SPICE ET).
// This numerical query time does not preserve Epoch's nanosecond resolution.
struct TdbTime {
    double seconds_past_j2000;
};
struct EphemerisState {
    Epoch epoch;
    std::int32_t target_naif_id;
    std::int32_t observer_naif_id;
    FrameId frame;
    std::array<double, 3> position_m;
    std::array<double, 3> velocity_mps;
    TimeScale velocity_time_scale; // Derivative is per TDB second, not per TAI second.
    TdbTime tdb;
    std::string source_id;
};
struct KernelDataInfo {
    std::string file;
    std::string kind;
    std::string sha256;
    std::string source_url;
    std::string source_sha256;
    std::string producer;
};
struct EphemerisDataInfo {
    std::string id;
    std::string manifest_sha256;
    std::string model;
    std::string toolkit_version;
    Epoch first_epoch;
    Epoch last_epoch; // Closed policy window; actual SPK coverage is checked on query.
    std::vector<KernelDataInfo> kernels; // Load order is significant.
};
class IEphemerisProvider {
  public:
    virtual ~IEphemerisProvider() = default;
    [[nodiscard]] virtual TdbTime to_tdb(Epoch epoch) const = 0;
    [[nodiscard]] virtual Epoch from_tdb(TdbTime time) const = 0;
    // Local derivative dTDB/dTAI from this immutable snapshot's time conversion.
    // Required before using geometric ephemeris velocities in TAI/TT dynamics.
    // Unsupported providers reject explicitly; no implicit finite differencing.
    [[nodiscard]] virtual double tdb_seconds_per_tai_second(Epoch) const {
        throw EphemerisDataError("This ephemeris does not provide the TDB/TAI rate");
    }
    [[nodiscard]] virtual EphemerisState geometric_state(std::int32_t target, std::int32_t observer,
                                                         Epoch epoch) const = 0;
    [[nodiscard]] virtual std::vector<EphemerisState>
    geometric_states(std::int32_t target, std::int32_t observer,
                     std::span<const Epoch> epochs) const = 0;
    [[nodiscard]] virtual const EphemerisDataInfo &data_info() const noexcept = 0;
    // Optional received apparent Sun from Earth's centre, including one-way light
    // time and stellar aberration (CSPICE LT+S), in the provider's ICRF/J2000 axes.
    // The certified interval includes the earlier epochs needed by light time.
    [[nodiscard]] virtual std::array<double, 3> apparent_sun_from_earth(Epoch) const {
        throw EphemerisDataError("This ephemeris does not support apparent Sun positions");
    }
    // Received Sun vector relative to a moving spacecraft: metres in ICRF-aligned
    // axes, Earth-relative position and velocity per TAI second. The provider
    // converts the velocity rate before forming its Newtonian SSB observer state.
    // This is not a complete relativistic BCRS/GCRS transformation.
    [[nodiscard]] virtual std::array<double, 3>
    apparent_sun_from_spacecraft(Epoch, const std::array<double, 3> &,
                                 const std::array<double, 3> &) const {
        throw EphemerisDataError(
            "This ephemeris does not support spacecraft apparent Sun positions");
    }
    // Certifies Earth chains throughout the receiving interval and Sun chains
    // from the snapshot start through its end. The actual emission epoch is also
    // checked at each query; arbitrary spacecraft distances are not given a fixed
    // geocentric light-time allowance. Unsupported providers must reject this mode.
    virtual void validate_spacecraft_apparent_sun_interval(Epoch, Epoch) const {
        throw EphemerisDataError("This ephemeris cannot certify spacecraft apparent Sun coverage");
    }
    virtual void validate_apparent_sun_interval(Epoch, Epoch) const {
        throw EphemerisDataError("This ephemeris cannot certify apparent Sun coverage");
    }
    // Validate continuous DE-style Earth/target segment chains, including inertial
    // axes and centres. Providers that cannot certify this must reject HPOP use.
    virtual void validate_earth_gravity_interval(Epoch, Epoch,
                                                 std::span<const std::int32_t>) const {
        throw EphemerisDataError(
            "This ephemeris cannot validate Earth third-body gravity coverage");
    }
    void validate_earth_gravity_interval(Epoch first, Epoch last, bool sun, bool moon) const {
        std::array<std::int32_t, 2> ids{};
        std::size_t count = 0;
        if (sun)
            ids[count++] = 10;
        if (moon)
            ids[count++] = 301;
        validate_earth_gravity_interval(first, last, std::span{ids}.first(count));
    }
};
} // namespace orbitforge::core
