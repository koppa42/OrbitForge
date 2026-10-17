#pragma once
#include <array>
#include <optional>
#include <orbitforge/core/propagation/force_differential.hpp>
#include <orbitforge/core/time/epoch.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace orbitforge::core {
// Ordered like the Earth HPOP table. Mars and outer planets use system barycentres.
enum class ThirdBody { sun, moon, jupiter, venus, saturn, mars, mercury, uranus, neptune, pluto };
inline constexpr std::size_t third_body_count = 10;
inline constexpr std::array third_body_list{
    ThirdBody::sun,  ThirdBody::moon,    ThirdBody::jupiter, ThirdBody::venus,   ThirdBody::saturn,
    ThirdBody::mars, ThirdBody::mercury, ThirdBody::uranus,  ThirdBody::neptune, ThirdBody::pluto};
[[nodiscard]] std::string_view third_body_name(ThirdBody body);
[[nodiscard]] std::int32_t third_body_naif_id(ThirdBody body);
enum class ThirdBodyGravitySource { cb_file, jpl_de, user_specified };
// Captured constants, not a live path. GM in SI; raw file SHA-256 records provenance.
struct CbGravity {
    double mu = 0;
    std::string name, content_sha256;
    ThirdBody body = ThirdBody::sun;
    bool operator==(const CbGravity &) const = default;
};
struct ThirdBodyGravity {
    bool enabled = false;
    ThirdBodyGravitySource source = ThirdBodyGravitySource::jpl_de;
    std::optional<double> user_mu{}; // SI; retained when another source is selected.
    std::optional<CbGravity> cb{};
    bool operator==(const ThirdBodyGravity &) const = default;
};
struct ThirdBodyDataInfo {
    std::string model, content_sha256, interpretation_version;
    Epoch first{0}, last{0};        // Inclusive TAI interval; no extrapolation.
    double sun_mu = 0, moon_mu = 0; // m^3/s^2, matching the ephemeris solution.
    std::array<double, third_body_count - 2> planet_mu{}; // Same order as third_body_list[2:].
    [[nodiscard]] double mu(ThirdBody body) const;
};
[[nodiscard]] double third_body_mu(const ThirdBodyGravity &settings, ThirdBody body,
                                   const ThirdBodyDataInfo &data);
// Geometric Earth-relative positions in ICRF-aligned axes for Newtonian forces.
// This is not a full relativistic BCRS -> GCRS state conversion; no aberration,
// light time or velocity re-labelling. Implementations own immutable resources.
struct GeometricBodyState {
    std::array<double, 3> position_m, velocity_mps; // Velocity per TAI second.
};
class IThirdBodyEphemeris {
  public:
    virtual ~IThirdBodyEphemeris() = default;
    [[nodiscard]] virtual std::array<double, 3> position(ThirdBody body, Epoch epoch) const = 0;
    // Optional physical-centre geometry for SRP. Mars and outer planets MUST NOT
    // use position(), which represents the system barycentre. Unsupported providers
    // reject rather than approximating a physical silhouette by a barycentre.
    [[nodiscard]] virtual std::array<double, 3> eclipsing_body_position(ThirdBody body,
                                                                        Epoch at) const {
        if (body == ThirdBody::mercury || body == ThirdBody::venus)
            return position(body, at);
        throw std::invalid_argument(
            "This ephemeris does not provide the requested SRP body centre");
    }
    virtual void validate_eclipsing_body_interval(Epoch first, Epoch last,
                                                  std::span<const ThirdBody> bodies) const {
        for (auto body : bodies)
            if (body != ThirdBody::mercury && body != ThirdBody::venus)
                throw std::invalid_argument(
                    "This ephemeris cannot certify the requested SRP body centre");
        validate_interval(first, last, bodies);
    }
    // Optional geometric position and velocity, Earth relative, ICRF-aligned axes.
    // The velocity is the derivative of position() per TAI second. This is a
    // Newtonian ephemeris input, not a relativistic coordinate transformation.
    [[nodiscard]] virtual GeometricBodyState geometric_state(ThirdBody, Epoch) const {
        throw std::invalid_argument("This ephemeris does not provide geometric TAI velocities");
    }
    // Optional geocentric apparent Sun: received light, LT+S in the same ICRF axes.
    // Unsupported providers fail explicitly; geometric forces continue to use position().
    [[nodiscard]] virtual std::array<double, 3> apparent_sun_from_earth(Epoch) const {
        throw std::invalid_argument("This ephemeris does not support the apparent Sun");
    }
    // Received Sun vector relative to a moving spacecraft: metres in ICRF-aligned
    // axes, Earth-relative position and velocity per TAI second. The provider
    // converts the velocity rate before forming its Newtonian SSB observer state.
    // This is not a complete relativistic BCRS/GCRS transformation.
    [[nodiscard]] virtual std::array<double, 3>
    apparent_sun_from_spacecraft(Epoch, const std::array<double, 3> &,
                                 const std::array<double, 3> &) const {
        throw std::invalid_argument(
            "This ephemeris does not support spacecraft apparent Sun positions");
    }
    // Certifies Earth chains throughout the receiving interval and Sun chains
    // from the snapshot start through its end. The actual emission epoch is also
    // checked at each query; arbitrary spacecraft distances are not given a fixed
    // geocentric light-time allowance. Unsupported providers must reject this mode.
    virtual void validate_spacecraft_apparent_sun_interval(Epoch, Epoch) const {
        throw std::invalid_argument(
            "This ephemeris cannot certify spacecraft apparent Sun coverage");
    }
    virtual void validate_apparent_sun_interval(Epoch, Epoch) const {
        throw std::invalid_argument("This ephemeris cannot certify apparent Sun coverage");
    }
    virtual void validate_interval(Epoch first, Epoch last,
                                   std::span<const ThirdBody> bodies) const = 0;
    void validate_interval(Epoch first, Epoch last, bool sun, bool moon) const {
        std::array<ThirdBody, 2> bodies{};
        std::size_t count = 0;
        if (sun)
            bodies[count++] = ThirdBody::sun;
        if (moon)
            bodies[count++] = ThirdBody::moon;
        validate_interval(first, last, std::span{bodies}.first(count));
    }
    [[nodiscard]] virtual const ThirdBodyDataInfo &data_info() const noexcept = 0;
};
// Differential gravity: includes the acceleration of the geocentric origin.
// SI positions in the same inertial axes, positive finite GM. Stable for |r| << |rb|.
[[nodiscard]] std::array<double, 3> third_body_acceleration(const std::array<double, 3> &satellite,
                                                            const std::array<double, 3> &body,
                                                            double mu);
// Analytic derivative with respect to the spacecraft state, holding body position
// and GM fixed at the force epoch. Includes the same geocentric differential
// acceleration; its origin term has zero state derivative. The velocity block
// is exactly zero. Throws invalid_argument on invalid or singular inputs/results.
[[nodiscard]] ForceDifferential third_body_differential(const std::array<double, 3> &satellite,
                                                        const std::array<double, 3> &body,
                                                        double mu);
} // namespace orbitforge::core
