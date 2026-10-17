#pragma once
#include <filesystem>
#include <functional>
#include <orbitforge/application/data_lock.hpp>
#include <orbitforge/core/propagation/space_weather.hpp>

namespace orbitforge::runtime {
// CelesTrak CSV / fixed-width, STK FXM and CSSI Predicts originals. Hashes all bytes;
// owns parsed values and quality flags. No implicit download or gap filling.
[[nodiscard]] std::shared_ptr<const core::SpaceWeather>
load_space_weather(const std::filesystem::path &file, const std::string &source_url = {},
                   const std::function<void()> &checkpoint = {});
[[nodiscard]] application::DataReference
space_weather_reference(const std::string &selection, const core::SpaceWeatherInfo &info);
void validate_space_weather_reference(std::span<const application::DataReference> recorded,
                                      const std::string &selection,
                                      const core::SpaceWeatherInfo &info);
[[nodiscard]] std::string space_weather_download_url();
} // namespace orbitforge::runtime
