#pragma once
#include <filesystem>
#include <functional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/core/propagation/gravity.hpp>

namespace orbitforge::runtime {
// Static ICGEM .gfc and STK .grv with optional SecularVariations. Hashes complete
// files and retains coefficients through core::gravity_max_degree. RefEpochTDT is
// a TT Julian date; LinearRate Cos/Sin n m value is per Julian year and follows the
// file's Normalized designation. Converts the epoch to TAI ns, rounded to nearest ns.
// Time-dependent ICGEM records and unsupported STK blocks are rejected. No name-based
// secular model is added to static files.
// EarthIERSTideModel and legacy LoveNumbers blocks override the real k20 only,
// as in STK; their finite imaginary component is parsed but not used. Conflicting
// permanent-tide declarations and unsupported tide/process-noise blocks fail.
// The returned resource owns all computation data; it never reopens its source.
[[nodiscard]] std::shared_ptr<const core::GravityModel>
load_gravity(const std::filesystem::path &file, const std::string &source_url = {},
             const std::function<void()> &checkpoint = {});
[[nodiscard]] application::DataReference gravity_reference(const std::string &selection,
                                                           const core::GravityInfo &info);
// Low-level callers must confirm the same bytes and interpretation as the project.
void validate_gravity_reference(std::span<const application::DataReference> recorded,
                                const std::string &selection, const core::GravityInfo &info);
[[nodiscard]] std::string gravity_download_url(const std::string &model);
} // namespace orbitforge::runtime
