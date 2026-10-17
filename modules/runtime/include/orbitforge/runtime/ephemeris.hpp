#pragma once
#include <filesystem>
#include <memory>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/core/ephemeris/ephemeris_provider.hpp>
#include <orbitforge/core/propagation/third_body.hpp>

namespace orbitforge::runtime {
[[nodiscard]] bool ephemeris_available() noexcept;
[[nodiscard]] std::unique_ptr<core::IEphemerisProvider>
load_spice_snapshot(const std::filesystem::path &manifest, const std::filesystem::path &workspace);
// Explicit Newtonian position adapter for DE440 resources, never a CartesianState relabel.
[[nodiscard]] std::shared_ptr<const core::IThirdBodyEphemeris>
make_third_body_ephemeris(std::shared_ptr<const core::IEphemerisProvider> ephemeris);
[[nodiscard]] application::DataReference ephemeris_reference(const std::string &selection,
                                                             const core::ThirdBodyDataInfo &info);
void validate_ephemeris_reference(std::span<const application::DataReference> recorded,
                                  const std::string &selection,
                                  const core::ThirdBodyDataInfo &info);
inline constexpr double de440_sun_mu = 1.3271244004127942e20;
inline constexpr double de440_moon_mu = 4.9028001184575496e12;
// NAIF gm_de440.tpc, converted from km^3/s^2. Mars/outer planets: system GM.
inline constexpr std::array<double, core::third_body_count - 2> de440_planet_mu{
    1.2671276409999998e17, 3.2485859200000000e14, 3.7940584841799997e16, 4.2828375815756102e13,
    2.2031868551400003e13, 5.7945563999999985e15, 6.8365271005803989e15, 9.755e11};
// Reads only the GM subset of an STK .cb file. Input GM units are explicit
// (SI by default; pass 1e9 for km^3/s^2). Does not load the file's ephemeris settings.
// SystemGm is required for Mars/outer planets, whose DE440 positions are barycentres.
[[nodiscard]] core::CbGravity load_cb_gravity(const std::filesystem::path &path,
                                              core::ThirdBody body, double mu_scale = 1.0);
inline constexpr auto de440s_sha256 =
    "c1c7feeab882263fc493a9d5a5b2ddd71b54826cdf65d8d17a76126b260a49f2";
inline constexpr auto de440s_url =
    "https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/planets/de440s.bsp";
} // namespace orbitforge::runtime
