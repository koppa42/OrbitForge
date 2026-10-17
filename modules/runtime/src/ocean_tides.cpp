#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <picosha2.h>
#include <sstream>
namespace orbitforge::runtime {
namespace {
void require(bool valid, const std::string &message) {
    if (!valid)
        throw std::runtime_error("Ocean tides: " + message);
}
std::string trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    return first == s.npos ? std::string{}
                           : std::string(s.substr(first, s.find_last_not_of(" \t\r") - first + 1));
}
template <class T> T number(std::string s) {
    if (s.starts_with('+'))
        s.erase(0, 1);
    for (auto &ch : s)
        if (ch == 'd' || ch == 'D')
            ch = 'e';
    T value{};
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    require(!s.empty() && result.ec == std::errc{} && result.ptr == s.data() + s.size() &&
                std::isfinite(static_cast<double>(value)),
            "invalid number");
    return value;
}
std::vector<std::string> fields(const std::string &s) {
    std::istringstream in(s);
    in.imbue(std::locale::classic());
    std::vector<std::string> result;
    for (std::string value; in >> value;)
        result.push_back(value);
    return result;
}
} // namespace
std::shared_ptr<const core::OceanTideModel>
load_ocean_tides(const std::filesystem::path &file, const std::string &url,
                 const std::function<void()> &checkpoint) {
    std::ifstream input(file, std::ios::binary);
    require(input.is_open(), "cannot open file");
    std::string bytes;
    std::array<char, 65536> buffer{};
    while (input) {
        if (checkpoint)
            checkpoint();
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        bytes.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        require(bytes.size() <= 128U * 1024 * 1024, "file exceeds 128 MiB");
    }
    require(input.eof() && !input.bad(), "cannot read file");
    core::OceanTideInfo info;
    info.sha256 = picosha2::hash256_hex_string(bytes);
    info.source_url = url;
    std::istringstream lines(bytes);
    std::string line;
    std::size_t line_number = 0;
    int stage = 0;
    double scale = 0;
    std::size_t records = 0;
    std::map<int, core::OceanTideConstituent> waves;
    constexpr std::string_view units =
        "Coefficients to compute variations in normalized Stokes coefficients (unit = 10^-";
    constexpr std::string_view model_prefix = "Ocean tide model: ";
    constexpr std::string_view extent_prefix = " up to (";
    while (std::getline(lines, line)) {
        ++line_number;
        if (checkpoint && line_number % 256 == 0)
            checkpoint();
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf"))
            line.erase(0, 3);
        line = trim(line);
        if (line.empty() || line.starts_with('#'))
            continue;
        try {
            if (stage == 0) {
                require(line.starts_with(units), "expected normalized potential units");
                const auto exponent = std::string_view(line).substr(units.size());
                require(exponent == "11)" || exponent == "12)",
                        "expected normalized potential units");
                scale = exponent == "11)" ? 1e-11 : 1e-12;
                ++stage;
                continue;
            }
            if (stage == 1) {
                const auto extent_at = line.rfind(extent_prefix);
                require(line.starts_with(model_prefix) && line.ends_with(')') &&
                            extent_at != line.npos && extent_at > model_prefix.size(),
                        "expected model and degree/order header");
                const auto extent = std::string_view(line).substr(extent_at + extent_prefix.size(),
                                                                  line.size() - extent_at -
                                                                      extent_prefix.size() - 1);
                const auto comma = extent.find(',');
                require(comma != extent.npos, "expected degree/order pair");
                const auto degree = extent.substr(0, comma);
                const auto order = extent.substr(comma + 1);
                const auto decimal = [](std::string_view value) {
                    return !value.empty() && std::ranges::all_of(value, [](char ch) {
                        return ch >= '0' && ch <= '9';
                    });
                };
                require(decimal(degree) && decimal(order), "invalid decimal degree/order");
                info.model = line.substr(model_prefix.size(), extent_at - model_prefix.size());
                info.degree = number<int>(std::string(degree));
                info.order = number<int>(std::string(order));
                require(info.degree >= 2 && info.degree <= core::gravity_max_degree &&
                            info.order >= 0 && info.order <= info.degree,
                        "invalid file extent");
                ++stage;
                continue;
            }
            if (stage == 2) {
                require(line.starts_with('(') && line.ends_with(')'),
                        "missing FES model description");
                ++stage;
                continue;
            }
            const auto f = fields(line);
            if (stage == 3) {
                require(f == std::vector<std::string>{"Doodson", "Darw", "l", "m", "DelC+", "DelS+",
                                                      "DelC-", "DelS-"},
                        "unknown coefficient columns");
                ++stage;
                continue;
            }
            require(f.size() == 8 && ++records <= 4000000,
                    "expected eight columns, at most four million records");
            auto d = f[0];
            const auto dot = d.find('.');
            require((dot == 2 || dot == 3) && d.size() == dot + 4,
                    "invalid six-digit Doodson number");
            d.erase(dot, 1);
            require(std::all_of(d.begin(), d.end(), [](char c) { return c >= '0' && c <= '9'; }),
                    "invalid Doodson digits");
            const int doodson = number<int>(d);
            require(waves.contains(doodson) || waves.size() < 4096, "too many constituents");
            auto &wave = waves[doodson];
            wave.doodson = doodson;
            wave.coefficients.push_back({number<int>(f[2]), number<int>(f[3]),
                                         scale * number<double>(f[4]), scale * number<double>(f[5]),
                                         scale * number<double>(f[6]),
                                         scale * number<double>(f[7])});
        } catch (const std::exception &e) {
            throw std::runtime_error("Ocean tide line " + std::to_string(line_number) + ": " +
                                     e.what());
        }
    }
    require(stage == 4 && !waves.empty(), "incomplete file");
    std::vector<core::OceanTideConstituent> constituents;
    for (auto &[id, wave] : waves)
        constituents.push_back(std::move(wave));
    if (checkpoint)
        checkpoint();
    auto result =
        std::make_shared<const core::OceanTideModel>(std::move(info), std::move(constituents));
    if (checkpoint)
        checkpoint();
    return result;
}
application::DataReference ocean_tide_reference(const std::string &selection,
                                                const core::OceanTideInfo &info) {
    const std::string version = "fes-potential-iers2010-original-mean-pole-v1";
    std::ostringstream settings;
    settings.imbue(std::locale::classic());
    settings << version << std::hexfloat << ";mu=" << info.mu << ";radius=" << info.radius_m;
    return {"ocean-tides/" + selection, "ocean-tides-" + info.sha256, info.sha256, version,
            picosha2::hash256_hex_string(settings.str())};
}
void validate_ocean_tide_reference(std::span<const application::DataReference> recorded,
                                   const std::string &selection, const core::OceanTideInfo &info) {
    const auto ref = ocean_tide_reference(selection, info);
    const auto found = std::ranges::find(recorded, ref.role, &application::DataReference::role);
    require(found != recorded.end() && *found == ref,
            "resource differs from project data; use a preparation plan");
}
std::string ocean_tide_download_url() {
    return "https://api.sedoo.fr/formater-catalogue-prod/grace/direct/"
           "fes2014b.v1.Cnm-Snm_Om1%2BOm2C20_with_S1_wave.POT_format.txt";
}
} // namespace orbitforge::runtime
