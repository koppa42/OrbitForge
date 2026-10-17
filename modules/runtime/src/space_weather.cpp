#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <orbitforge/runtime/space_weather.hpp>
#include <picosha2.h>
#include <sstream>

namespace orbitforge::runtime {
namespace {
void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error("Space weather: " + message);
}
std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == s.npos)
        return {};
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}
template <class T> T number(std::string_view s) {
    s = trim(s);
    T value{};
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    require(!s.empty() && result.ec == std::errc{} && result.ptr == s.data() + s.size() &&
                std::isfinite(static_cast<double>(value)),
            "invalid numeric field");
    return value;
}
std::optional<double> optional_number(std::string_view s) {
    return trim(s).empty() ? std::nullopt : std::optional{number<double>(s)};
}
std::optional<double> kp(std::string_view s, bool predicted) {
    if (trim(s).empty())
        return {};
    const int encoded = number<int>(s), index = static_cast<int>(std::lround(encoded * .3));
    require(encoded >= 0 && encoded <= 90, "Kp must be within 0..90 on the ten-times scale");
    if (std::lround(index * (10.0 / 3)) == encoded)
        return index / 3.0;
    // CelesTrak's Ap-derived forecasts also contain intermediate tenths, e.g.
    // 22 for Ap=8, although observed Kp uses rounded thirds (23 for 2+).
    require(predicted, "Observed Kp must use the rounded ten-times one-third scale");
    return encoded / 10.0;
}
std::vector<std::string_view> csv(std::string_view line) {
    std::vector<std::string_view> fields;
    for (;;) {
        const auto comma = line.find(',');
        fields.push_back(trim(line.substr(0, comma)));
        if (comma == line.npos)
            return fields;
        line.remove_prefix(comma + 1);
    }
}
std::vector<std::string> words(std::string_view line, std::size_t count) {
    std::istringstream input{std::string(line)};
    std::vector<std::string> result;
    std::string word;
    while (input >> word)
        result.push_back(word);
    require(result.size() == count, "incorrect CSSI Predicts column count");
    return result;
}
void cssi_month(int year, int month) {
    require(year >= 1600 && year <= 2600 && month >= 1 && month <= 12,
            "invalid CSSI Predicts year/month");
}
std::array<int, 7> cssi_header_values(std::string_view line) {
    const auto values = words(line, 7);
    std::array<int, 7> result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = number<int>(values[i]);
    for (int i = 0; i < 6; i += 2)
        cssi_month(result[i], result[i + 1]);
    require(result[6] >= 1 && result[6] <= 12012 &&
                result[0] * 12 + result[1] <= result[2] * 12 + result[3],
            "invalid CSSI Predicts span/count");
    return result;
}
core::SpaceWeatherDay cssi_day(std::string_view line) {
    const auto values = words(line, 6);
    const int year = number<int>(values[0]), month = number<int>(values[1]);
    cssi_month(year, month);
    for (std::size_t i = 2; i < 6; ++i) {
        const double value = number<double>(values[i]);
        require((i < 4 ? value > 0 : value >= 0) && value <= (i < 4 ? 1000 : 400),
                "CSSI Predicts flux/Ap outside its valid range");
    }
    core::SpaceWeatherDay result;
    result.date = {year, month, 15, 0, 0, 0, 0};
    result.quality = core::WeatherQuality::monthly_prediction;
    result.f107_observed = number<double>(values[3]);
    result.daily_ap = number<double>(values[5]);
    return result;
}
constexpr std::string_view header =
    "DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,"
    "AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,F10.7_DATA_TYPE,F10.7_OBS_CENTER81,"
    "F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81";
core::SpaceWeatherDay csv_day(std::string_view line) {
    const auto f = csv(line);
    require(f.size() == 31, "CSV requires exactly 31 columns");
    require(f[0].size() == 10 && f[0][4] == '-' && f[0][7] == '-', "invalid CSV date");
    core::SpaceWeatherDay d;
    d.date = {number<int>(f[0].substr(0, 4)),
              number<int>(f[0].substr(5, 2)),
              number<int>(f[0].substr(8, 2)),
              0,
              0,
              0,
              0};
    for (std::size_t i = 0; i < 8; ++i) {
        d.kp[i] = kp(f[3 + i], f[26] == "PRD" || f[26] == "PRM");
        d.ap[i] = optional_number(f[12 + i]);
    }
    if (const auto sum = optional_number(f[11]))
        d.daily_kp = *sum / 80;
    d.daily_ap = optional_number(f[20]);
    d.f107_observed = optional_number(f[24]);
    d.f107_adjusted = optional_number(f[25]);
    d.observed_center81 = optional_number(f[27]);
    d.observed_last81 = optional_number(f[28]);
    d.adjusted_center81 = optional_number(f[29]);
    d.adjusted_last81 = optional_number(f[30]);
    if (f[26] == "OBS")
        d.quality = core::WeatherQuality::observed;
    else if (f[26] == "INT")
        d.quality = core::WeatherQuality::interpolated;
    else if (f[26] == "PRD")
        d.quality = core::WeatherQuality::daily_prediction;
    else if (f[26] == "PRM")
        d.quality = core::WeatherQuality::monthly_prediction;
    else
        require(false, "unknown CSV flux quality");
    return d;
}
core::SpaceWeatherDay fixed_day(std::string_view line, const std::string &section) {
    require(line.size() >= 130 && trim(line.substr(130)).empty(), "fixed record needs 130 columns");
    const auto field = [&](std::size_t first, std::size_t width) {
        return line.substr(first, width);
    };
    core::SpaceWeatherDay d;
    d.date = {
        number<int>(field(0, 4)), number<int>(field(5, 2)), number<int>(field(8, 2)), 0, 0, 0, 0};
    for (std::size_t i = 0; i < 8; ++i) {
        d.kp[i] = kp(field(19 + 3 * i, 2), section != "OBSERVED");
        d.ap[i] = optional_number(field(47 + 4 * i, 3));
    }
    if (const auto sum = optional_number(field(43, 3)))
        d.daily_kp = *sum / 80;
    d.daily_ap = optional_number(field(79, 3));
    d.f107_adjusted = optional_number(field(93, 5));
    d.adjusted_center81 = optional_number(field(101, 5));
    d.adjusted_last81 = optional_number(field(107, 5));
    d.f107_observed = optional_number(field(113, 5));
    d.observed_center81 = optional_number(field(119, 5));
    d.observed_last81 = optional_number(field(125, 5));
    if (!trim(field(99, 1)).empty()) {
        d.flux_qualifier = number<int>(field(99, 1));
        require(*d.flux_qualifier >= 0 && *d.flux_qualifier <= 4, "invalid flux qualifier");
    }
    if (section == "MONTHLY_PREDICTED")
        d.quality = core::WeatherQuality::monthly_prediction;
    else if (section == "DAILY_PREDICTED")
        d.quality = core::WeatherQuality::daily_prediction;
    else if (d.flux_qualifier && *d.flux_qualifier >= 2)
        d.quality = core::WeatherQuality::interpolated;
    return d;
}
core::SpaceWeatherDay fxm_day(std::string_view line, std::string_view section) {
    const std::size_t width = section == "OBSERVED" ? 78 : section == "F10_PREDICT" ? 18 : 12;
    require(line.size() >= width && trim(line.substr(width)).empty(), "FXM record width mismatch");
    core::SpaceWeatherDay d;
    d.date = {number<int>(line.substr(0, 4)),
              number<int>(line.substr(4, 2)),
              number<int>(line.substr(6, 2)),
              0,
              0,
              0,
              0};
    if (section == "OBSERVED") {
        for (std::size_t i = 0; i < 8; ++i) {
            d.kp[i] = kp(line.substr(14 + 2 * i, 2), false);
            d.ap[i] = optional_number(line.substr(33 + 3 * i, 3));
        }
        if (const auto sum = optional_number(line.substr(30, 3)))
            d.daily_kp = *sum / 80;
        d.daily_ap = optional_number(line.substr(57, 3));
        d.f107_adjusted = optional_number(line.substr(67, 5));
        d.adjusted_center81 = optional_number(line.substr(73, 5));
        if (!trim(line.substr(72, 1)).empty()) {
            d.flux_qualifier = number<int>(line.substr(72, 1));
            require(*d.flux_qualifier >= 0 && *d.flux_qualifier <= 3, "invalid FXM flux qualifier");
            if (*d.flux_qualifier >= 2)
                d.quality = core::WeatherQuality::interpolated;
            if (*d.flux_qualifier == 3)
                d.f107_adjusted.reset(); // Format explicitly says no observation.
        }
    } else {
        require(line[8] == ' ', "FXM prediction separator must be a space");
        d.quality = core::WeatherQuality::daily_prediction;
        if (section == "F10_PREDICT") {
            require(line[12] == ' ', "FXM prediction separator must be a space");
            d.f107_adjusted = optional_number(line.substr(9, 3));
            d.adjusted_center81 = optional_number(line.substr(13, 5));
        } else
            d.daily_ap = optional_number(line.substr(9, 3));
    }
    return d;
}
int fxm_date(const core::SpaceWeatherDay &d) {
    return d.date.year * 10000 + d.date.month * 100 + d.date.day;
}
} // namespace
std::shared_ptr<const core::SpaceWeather>
load_space_weather(const std::filesystem::path &file, const std::string &url,
                   const std::function<void()> &checkpoint) {
    std::ifstream input(file, std::ios::binary);
    require(input.is_open(), "cannot open input file");
    std::string bytes;
    std::array<char, 65536> buffer{};
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        require(bytes.size() <= 32U * 1024 * 1024, "file exceeds 32 MiB");
    }
    require(input.eof() && !input.bad(), "read failed");
    core::SpaceWeatherInfo info{picosha2::hash256_hex_string(bytes), url, {}};
    std::istringstream lines(bytes);
    std::string line, section;
    std::vector<core::SpaceWeatherDay> days;
    std::map<std::string, std::size_t> declared, observed;
    bool csv_format = false, fxm_format = false, identified = false;
    std::optional<std::array<int, 7>> cssi_header;
    constexpr std::array<std::string_view, 3> fxm_sections{"OBSERVED", "F10_PREDICT", "AP_PREDICT"};
    std::size_t fxm_section = 0;
    int fxm_last_date = 0;
    std::map<int, core::SpaceWeatherDay> predictions;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        if (checkpoint && line_number % 256 == 0)
            checkpoint();
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf"))
            line.erase(0, 3);
        const auto text = trim(line);
        if (text.empty() || text.starts_with('#'))
            continue;
        try {
            if (!identified) {
                if (text.front() >= '0' && text.front() <= '9') {
                    cssi_header = cssi_header_values(text);
                    info.format = "cssi-predicts";
                    info.cssi_predictions = true;
                    identified = true;
                    continue;
                }
                require(text == header || text == "DATATYPE CssiSpaceWeather" ||
                            text == "BEGIN OBSERVED",
                        "unknown format/header");
                csv_format = text == header;
                fxm_format = text == "BEGIN OBSERVED";
                info.format = fxm_format   ? "stk-flux-geomag"
                              : csv_format ? "celestrak-csv"
                                           : "celestrak-fixed";
                info.solar_flux_adjusted = fxm_format;
                identified = true;
                if (fxm_format)
                    section = "OBSERVED";
                continue;
            }
            if (cssi_header) {
                days.push_back(cssi_day(text));
                continue;
            }
            if (fxm_format) {
                if (text.starts_with("BEGIN ")) {
                    require(section.empty() && fxm_section < fxm_sections.size() &&
                                text.substr(6) == fxm_sections[fxm_section],
                            "invalid FXM section order/start");
                    section = std::string(fxm_sections[fxm_section]);
                    fxm_last_date = 0;
                } else if (text.starts_with("END ")) {
                    require(!section.empty() && text.substr(4) == section,
                            "invalid FXM section end");
                    section.clear();
                    ++fxm_section;
                } else {
                    require(!section.empty(), "FXM record outside a section");
                    auto d = fxm_day(line, section);
                    const int key = fxm_date(d);
                    require(key > fxm_last_date, "FXM section dates must be unique and ordered");
                    fxm_last_date = key;
                    if (section == "OBSERVED")
                        days.push_back(std::move(d));
                    else {
                        require(days.empty() || key > fxm_date(days.back()),
                                "FXM prediction overlaps observed data");
                        if (auto found = predictions.find(key); found != predictions.end()) {
                            require(section == "AP_PREDICT", "duplicate FXM prediction");
                            found->second.daily_ap = d.daily_ap;
                        } else
                            predictions.emplace(key, std::move(d));
                    }
                }
                continue;
            }
            if (csv_format) {
                days.push_back(csv_day(text));
                continue;
            }
            if (text.starts_with("NUM_")) {
                require(section.empty(), "count inside data section");
                const auto end = text.find("_POINTS ");
                require(end != text.npos, "invalid section count");
                const std::string name(text.substr(4, end - 4));
                const auto n = number<std::size_t>(text.substr(end + 8));
                require(n <= 366000 &&
                            (name == "OBSERVED" || name == "DAILY_PREDICTED" ||
                             name == "MONTHLY_PREDICTED") &&
                            declared.emplace(name, n).second,
                        "invalid count");
            } else if (text.starts_with("BEGIN ")) {
                const std::string name(text.substr(6));
                require(section.empty() && declared.contains(name) && !observed.contains(name),
                        "invalid section start");
                section = name;
                observed[name] = 0;
            } else if (text.starts_with("END ")) {
                require(!section.empty() && text.substr(4) == section &&
                            observed.at(section) == declared.at(section),
                        "section count/end mismatch");
                section.clear();
            } else if (!section.empty()) {
                days.push_back(fixed_day(line, section));
                ++observed.at(section);
            } else
                require(text.starts_with("VERSION ") || text.starts_with("UPDATED "),
                        "unexpected record outside a data section");
        } catch (const std::exception &e) {
            throw std::runtime_error("Space weather line " + std::to_string(line_number) + ": " +
                                     e.what());
        }
    }
    if (cssi_header) {
        const auto &h = *cssi_header;
        require(days.size() == static_cast<std::size_t>(h[6]),
                "CSSI Predicts record count mismatch");
        require(days.front().date.year == h[0] && days.front().date.month == h[1] &&
                    days.back().date.year == h[2] && days.back().date.month == h[3],
                "CSSI Predicts header span does not match the records");
    }
    if (fxm_format) {
        require(fxm_section == fxm_sections.size() && section.empty(), "incomplete FXM sections");
        for (auto &[key, d] : predictions)
            days.push_back(std::move(d));
    }
    require(identified && section.empty() && !days.empty(), "incomplete file");
    if (!csv_format && !fxm_format)
        for (const auto &[name, count] : declared)
            require(observed.contains(name) && observed.at(name) == count,
                    "missing or incomplete section");
    if (checkpoint)
        checkpoint();
    return std::make_shared<const core::SpaceWeather>(std::move(info), std::move(days));
}
application::DataReference space_weather_reference(const std::string &selection,
                                                   const core::SpaceWeatherInfo &info) {
    std::string version = "spaceweather-utc20-linear-jr-msis-lags-monthly-linear-v2";
    if (info.solar_flux_adjusted)
        version += ";flux=adjusted-au-geometric-utc20-v1";
    if (info.cssi_predictions)
        version += ";cssi=nominal-ap-only;average=centered-81-utc-days-v1";
    return {"space-weather/" + selection, "spaceweather-" + info.sha256, info.sha256, version,
            picosha2::hash256_hex_string(version)};
}
void validate_space_weather_reference(std::span<const application::DataReference> recorded,
                                      const std::string &selection,
                                      const core::SpaceWeatherInfo &info) {
    const auto ref = space_weather_reference(selection, info);
    const auto found = std::ranges::find(recorded, ref.role, &application::DataReference::role);
    require(found != recorded.end() && *found == ref,
            "resource differs from project data; use a preparation plan");
}
std::string space_weather_download_url() { return "https://celestrak.org/SpaceData/SW-All.csv"; }
} // namespace orbitforge::runtime
