#pragma once
#include <filesystem>
#include <functional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/core/propagation/ocean_tides.hpp>
#include <string_view>
namespace orbitforge::runtime {
// FES normalized C+/S+/C-/S- potential format, units 10^-11 or 10^-12.
// Water-height and unknown headers are rejected. IERS 2010 GM/radius scaling
// is explicit in metadata; no implicit inference of missing minor constituents.
// Reads/hashes/owns the entire file (128 MiB bound); errors/cancellation publish nothing.
[[nodiscard]] std::shared_ptr<const core::OceanTideModel>
load_ocean_tides(const std::filesystem::path &file, const std::string &source_url = {},
                 const std::function<void()> &checkpoint = {});
[[nodiscard]] application::DataReference ocean_tide_reference(const std::string &selection,
                                                              const core::OceanTideInfo &info);
void validate_ocean_tide_reference(std::span<const application::DataReference> recorded,
                                   const std::string &selection, const core::OceanTideInfo &info);
[[nodiscard]] std::string ocean_tide_download_url();
inline constexpr std::string_view fes2014bv1_sha256 =
    "74156304aca8f66ed1f67775de3d4a9d24ecb94abb47127ef40d5bb4630f719f";
} // namespace orbitforge::runtime
