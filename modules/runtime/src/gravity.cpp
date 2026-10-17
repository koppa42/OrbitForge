#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <orbitforge/runtime/gravity.hpp>
#include <picosha2.h>
#include <sstream>

namespace orbitforge::runtime {
namespace {
void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string lower(std::string_view s) {
    std::string result(s);
    for (auto &c : result)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return result;
}
template <class T> T number(std::string_view s) {
    if (s.starts_with('+'))
        s.remove_prefix(1);
    T value{};
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    require(result.ec == std::errc{} && result.ptr == s.data() + s.size() &&
                std::isfinite(static_cast<double>(value)),
            "Invalid gravity numeric field");
    return value;
}
double real(std::string_view text) {
    std::string value(text);
    std::replace(value.begin(), value.end(), 'D', 'E');
    std::replace(value.begin(), value.end(), 'd', 'e');
    return number<double>(value);
}
// Parse TDT (= TT) JD by separating its integral day before rounding to Epoch
// nanoseconds. Converting the whole Julian date to double would lose tens of us.
core::Epoch secular_epoch(std::string_view text) {
    const double check = real(text);
    require(check > 2e6 && check < 3e6, "Secular reference TT Julian date is outside Epoch range");
    std::string digits(text);
    if (digits.starts_with('+'))
        digits.erase(0, 1);
    const auto exp_at = digits.find_first_of("eEdD");
    int exponent = 0;
    if (exp_at != digits.npos) {
        exponent = number<int>(std::string_view(digits).substr(exp_at + 1));
        digits.resize(exp_at);
    }
    const auto dot = digits.find('.');
    const auto integral = dot == digits.npos ? digits.size() : dot;
    if (dot != digits.npos)
        digits.erase(dot, 1);
    const auto point = static_cast<std::int64_t>(integral) + exponent;
    require(point > 0 && point <= 4096, "Invalid secular reference TT Julian date");
    if (point > static_cast<std::int64_t>(digits.size()))
        digits.append(static_cast<std::size_t>(point) - digits.size(), '0');
    const auto whole = number<std::int64_t>(std::string_view(digits).substr(0, point));
    const double fraction = point == static_cast<std::int64_t>(digits.size())
                                ? 0
                                : real("0." + digits.substr(static_cast<std::size_t>(point)));
    constexpr std::int64_t day_ns = 86400000000000LL;
    auto days = whole - 2451545;
    auto partial = static_cast<std::int64_t>(std::llround(fraction * day_ns)) - 32184000000LL;
    // Make the day part closer to zero so the earliest representable fractional day
    // is accepted without multiplying an out-of-range negative integral day.
    if (days < 0) {
        ++days;
        partial -= day_ns;
    }
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    require(days >= low / day_ns && days <= high / day_ns,
            "Secular reference TT Julian date is outside Epoch range");
    const auto base = days * day_ns;
    require(!((partial > 0 && base > high - partial) || (partial < 0 && base < low - partial)),
            "Secular reference TT Julian date is outside Epoch range");
    return core::Epoch{base + partial};
}
double normalize(double v, int n, int m) {
    if (v == 0)
        return 0;
    const double log_scale = 0.5 * (std::log((m == 0 ? 1.0 : 2.0) * (2 * n + 1)) +
                                    std::lgamma(n - m + 1.0) - std::lgamma(n + m + 1.0));
    const double result = std::copysign(std::exp(std::log(std::abs(v)) - log_scale), v);
    require(std::isfinite(result) && result != 0,
            "Unnormalized gravity coefficient overflow or underflow");
    return result;
}
std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    while (!line.empty()) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == line.npos || line[start] == '#')
            break;
        line.remove_prefix(start);
        const auto end = line.find_first_of(" \t\r");
        result.push_back(line.substr(0, end));
        if (end == line.npos)
            break;
        line.remove_prefix(end);
    }
    return result;
}
} // namespace
void validate_gravity_reference(std::span<const application::DataReference> recorded,
                                const std::string &selection, const core::GravityInfo &info) {
    const auto selected = gravity_reference(selection, info);
    const auto found =
        std::ranges::find(recorded, selected.role, &application::DataReference::role);
    require(found != recorded.end() && *found == selected,
            "Gravity provider differs from the project's confirmed data; use a preparation plan");
}
std::string gravity_download_url(const std::string &model) {
    if (model == "egm96")
        return "https://icgem.gfz.de/getmodel/gfc/"
               "971b0a3b49a497910aad23cd85e066d4cd9af0aeafe7ce6301a696bed8570be3/EGM96.gfc";
    if (model == "egm2008")
        return "https://icgem.gfz.de/getmodel/gfc/"
               "c50128797a9cb62e936337c890e4425f03f0461d7329b09a8cc8561504465340/EGM2008.gfc";
    throw std::invalid_argument("No online source for this gravity model");
}
std::shared_ptr<const core::GravityModel> load_gravity(const std::filesystem::path &file,
                                                       const std::string &source_url,
                                                       const std::function<void()> &checkpoint) {
    std::ifstream input(file, std::ios::binary);
    require(input.is_open(), "Cannot open gravity file");
    picosha2::hash256_one_by_one hash;
    core::GravityInfo info;
    info.source_url = source_url;
    std::map<std::string, std::string> header;
    std::vector<core::GravityCoefficient> coefficients;
    std::vector<bool> seen;
    bool grv = false, started = false, ended = false, normalized = true;
    bool secular_block = false, secular_seen = false;
    std::optional<core::Epoch> reference_epoch;
    struct Rate {
        double c = 0, s = 0;
        bool cosine_seen = false, sine_seen = false;
    };
    std::map<std::pair<int, int>, Rate> rates;
    std::string tide_block;
    bool tide_block_seen = false, love_seen = false;
    bool header_tide_seen = false, block_tide_seen = false;
    const auto read_permanent = [&](const std::vector<std::string_view> &row, bool in_block) {
        bool &seen_tide = in_block ? block_tide_seen : header_tide_seen;
        require(row.size() == 2 && !seen_tide, "Invalid or duplicate permanent tide designation");
        const auto value = lower(row[1]);
        require(value == "yes" || value == "no", "Invalid permanent tide designation");
        const auto [it, inserted] = header.emplace("includespermtide", value);
        require(inserted || lower(it->second) == value, "Conflicting permanent tide designations");
        seen_tide = true;
    };
    const auto read_love = [&](const std::vector<std::string_view> &row, std::size_t offset) {
        require(!love_seen && row.size() == offset + 4, "Invalid or duplicate Love number");
        require(number<int>(row[offset]) == 2 && number<int>(row[offset + 1]) == 0,
                "STK Earth tides support only a degree-2 order-0 Love number override");
        info.solid_tide_k20 = real(row[offset + 2]);
        // STK explicitly uses only the real part, but the imaginary field must be valid.
        (void)real(row[offset + 3]);
        love_seen = true;
    };
    std::size_t line_number = 0, bytes = 0, count = 0;
    int maximum_order = 0;
    const auto begin = [&] {
        const auto get = [&](const char *gfc, const char *stk) -> const std::string & {
            const auto it = header.find(grv ? stk : gfc);
            require(it != header.end(), "Gravity header is incomplete");
            return it->second;
        };
        info.model = get("modelname", "model");
        info.mu = number<double>(get("earth_gravity_constant", "gm"));
        info.radius_m = number<double>(get("radius", "refdistance"));
        info.degree = number<int>(get("max_degree", "degree"));
        info.order = grv ? number<int>(get("max_degree", "order")) : info.degree;
        require(info.degree >= 2 && info.degree <= core::gravity_max_degree && info.order >= 0 &&
                    info.order <= info.degree,
                "Gravity file degree/order must be within 2..2190");
        // This loader is for the terrestrial HPOP environment, not lunar/planetary fields.
        require(info.mu > 3.9e14 && info.mu < 4.1e14 && info.radius_m > 6.3e6 &&
                    info.radius_m < 6.5e6,
                "Gravity file does not describe an Earth field in SI units");
        if (grv) {
            require(!header.contains("centralbody") || lower(header.at("centralbody")) == "earth",
                    "Gravity loader supports only Earth central body");
            const auto norm = lower(get("norm", "normalized"));
            require(norm == "yes" || norm == "no", "Invalid gravity normalization");
            normalized = norm == "yes";
            const auto tide = lower(get("tide_system", "includespermtide"));
            require(tide == "yes" || tide == "no", "Invalid permanent tide designation");
            info.tide_system = tide == "yes" ? "zero_tide" : "tide_free";
        } else {
            require(get("product_type", "") == "gravity_field", "Not an ICGEM gravity field");
            info.tide_system = get("tide_system", "");
            // Fully normalized is the ICGEM format's default when norm is absent (EGM96).
            if (header.contains("norm")) {
                require(header.at("norm") == "fully_normalized" ||
                            header.at("norm") == "unnormalized",
                        "Unsupported ICGEM normalization");
                normalized = header.at("norm") == "fully_normalized";
            }
        }
        seen.resize(static_cast<std::size_t>((info.degree + 1) * (info.degree + 2) / 2));
        coefficients.reserve(grv ? std::min<std::size_t>(seen.size(), 1024) : seen.size());
        started = true;
    };
    for (std::string line; std::getline(input, line);) {
        if ((++line_number % 1024) == 0 && checkpoint)
            checkpoint();
        require(line.size() <= 4096, "Gravity file line exceeds 4096 bytes");
        bytes += line.size() + 1;
        require(bytes <= 512U * 1024 * 1024, "Gravity file exceeds 512 MiB");
        hash.process(line.begin(), line.end());
        if (!input.eof()) {
            const std::string lf = "\n";
            hash.process(lf.begin(), lf.end());
        }
        // Fortran D exponents occur in ICGEM data. Change only numeric text, after hashing.
        std::string_view view(line);
        if (line_number == 1 && view.starts_with("\xef\xbb\xbf"))
            view.remove_prefix(3);
        const auto raw = fields(view);
        if (raw.empty())
            continue;
        const auto key = lower(raw.front());
        if (secular_block) {
            if (key == "end") {
                require(raw.size() == 2 && lower(raw[1]) == "secularvariations",
                        "Mismatched STK secular block terminator");
                require(reference_epoch && !rates.empty(), "Incomplete STK secular model");
                secular_block = false;
            } else if (key == "refepochtdt") {
                require(raw.size() == 2 && !reference_epoch,
                        "Invalid or duplicate secular reference epoch");
                reference_epoch = secular_epoch(raw[1]);
            } else if (key == "linearrate") {
                require(raw.size() == 5, "Invalid secular LinearRate record");
                const auto component = lower(raw[1]);
                require(component == "cos" || component == "sin", "Invalid secular component");
                const int n = number<int>(raw[2]), m = number<int>(raw[3]);
                require(n >= 2 && n <= core::gravity_max_degree && m >= 0 && m <= n,
                        "Secular coefficient exceeds degree/order limits");
                auto &rate = rates[{n, m}];
                auto &seen_component = component == "cos" ? rate.cosine_seen : rate.sine_seen;
                require(!seen_component, "Duplicate secular coefficient component");
                (component == "cos" ? rate.c : rate.s) = real(raw[4]);
                require(m != 0 || rate.s == 0, "Nonzero secular zonal sine coefficient");
                seen_component = true;
            } else {
                throw std::runtime_error("Unsupported record in SecularVariations");
            }
            continue;
        }
        if (grv && key == "begin" && raw.size() == 2 && lower(raw[1]) == "secularvariations") {
            require((!started || ended) && tide_block.empty() && !secular_seen,
                    "Duplicate or misplaced STK secular block");
            secular_block = secular_seen = true;
            continue;
        }
        if (!started) {
            if (!tide_block.empty()) {
                if (key == "end") {
                    require(raw.size() == 2 && lower(raw[1]) == tide_block,
                            "Mismatched STK tide block terminator");
                    require(tide_block != "lovenumbers" || love_seen, "Empty LoveNumbers block");
                    tide_block.clear();
                } else if (tide_block == "lovenumbers") {
                    read_love(raw, 0);
                } else if (key == "lovenumbers") {
                    read_love(raw, 1);
                } else if (key == "includespermtide") {
                    read_permanent(raw, true);
                } else {
                    throw std::runtime_error("Unsupported record in EarthIERSTideModel");
                }
                continue;
            }
            if (key.starts_with("stk.v.")) {
                require(!grv && raw.size() == 1, "Invalid or duplicate STK gravity version");
                grv = true;
                continue;
            }
            if ((!grv && key == "end_of_head") ||
                (grv && key == "begin" && raw.size() == 2 && lower(raw[1]) == "coefficients")) {
                begin();
                continue;
            }
            if (grv && key == "begin") {
                require(raw.size() == 2 && !tide_block_seen, "Invalid or duplicate STK tide block");
                tide_block = lower(raw[1]);
                require(tide_block == "earthierstidemodel" || tide_block == "lovenumbers",
                        "Unsupported STK tide/process-noise block");
                tide_block_seen = true;
                continue;
            }
            if (grv && key == "includespermtide") {
                read_permanent(raw, false);
                continue;
            }
            const bool known = grv || key == "modelname" || key == "earth_gravity_constant" ||
                               key == "radius" || key == "max_degree" || key == "norm" ||
                               key == "tide_system" || key == "product_type" || key == "errors" ||
                               key == "format";
            if (known) {
                require(raw.size() == 2 && !header.contains(key),
                        "Invalid or duplicate gravity header");
                if (grv)
                    require(key == "model" || key == "gm" || key == "refdistance" ||
                                key == "degree" || key == "order" || key == "normalized" ||
                                key == "centralbody",
                            "Unsupported STK gravity header field");
                header.emplace(key, raw[1]);
            }
            continue;
        }
        if (grv && key == "end" && raw.size() == 2 && lower(raw[1]) == "coefficients") {
            require(!ended, "Duplicate gravity coefficients block");
            ended = true;
            continue;
        }
        require(!ended, "Unsupported records after gravity coefficients");
        require(grv || key == "gfc", "Time-dependent or unknown gravity records are unsupported");
        const std::size_t offset = grv ? 0 : 1;
        require(raw.size() >= offset + 4 && raw.size() <= offset + 8,
                "Incomplete gravity coefficient record");
        const int n = number<int>(raw[offset]), m = number<int>(raw[offset + 1]);
        require(n >= 0 && n <= info.degree && m >= 0 && m <= std::min(n, info.order),
                "Gravity coefficient exceeds declared degree/order");
        const auto i = static_cast<std::size_t>(n * (n + 1) / 2 + m);
        require(!seen[i], "Duplicate gravity coefficient");
        seen[i] = true;
        maximum_order = std::max(maximum_order, m);
        double c = real(raw[offset + 2]), s = real(raw[offset + 3]);
        for (std::size_t j = offset + 4; j < raw.size(); ++j)
            require(real(raw[j]) >= 0, "Invalid gravity uncertainty");
        require((n != 0 || (c == 1 && s == 0)) && (n != 1 || (c == 0 && s == 0)) &&
                    (m != 0 || s == 0),
                "Gravity field must be geocentric with unit monopole");
        if (!normalized) {
            c = normalize(c, n, m);
            s = normalize(s, n, m);
        }
        if (n <= core::gravity_max_degree)
            coefficients.push_back({n, m, c, s});
        ++count;
    }
    require(input.eof() && !input.bad() && started && count > 0 && (!grv || ended) &&
                !secular_block && tide_block.empty(),
            "Incomplete gravity file");
    if (!grv) {
        // ICGEM fields are dense through their square part. EGM2008 legitimately
        // stops at order 2159 in degrees 2160..2190; infer the declared rectangular tail.
        int last_order = -1;
        for (int m = 0; m <= info.degree; ++m)
            if (seen[static_cast<std::size_t>(info.degree * (info.degree + 1) / 2 + m)])
                last_order = m;
        require(last_order >= 0 && last_order == maximum_order, "Truncated ICGEM gravity file");
        info.order = last_order;
        for (int n = 2; n <= info.degree; ++n)
            for (int m = 0; m <= std::min(n, last_order); ++m)
                require(seen[static_cast<std::size_t>(n * (n + 1) / 2 + m)],
                        "Missing ICGEM gravity coefficient");
    }
    if (reference_epoch) {
        info.secular_variations = core::GravitySecularVariations{*reference_epoch, {}};
        auto &normalized_rates = info.secular_variations->rates_per_year;
        normalized_rates.reserve(rates.size());
        for (const auto &[nm, rate] : rates) {
            const auto [n, m] = nm;
            require(n <= info.degree && m <= info.order,
                    "Secular coefficient exceeds the declared gravity field");
            if (normalized_rates.size() % 1024 == 0 && checkpoint)
                checkpoint();
            normalized_rates.push_back({n, m, normalized ? rate.c : normalize(rate.c, n, m),
                                        normalized ? rate.s : normalize(rate.s, n, m)});
        }
    }
    if (checkpoint)
        checkpoint();
    hash.finish();
    info.sha256 = picosha2::get_hash_hex_string(hash);
    return std::make_shared<const core::GravityModel>(std::move(info), coefficients);
}
application::DataReference gravity_reference(const std::string &selection,
                                             const core::GravityInfo &info) {
    constexpr auto version = "normalized-gravity-2190-secular-julian-year-v4";
    // Also hash physical metadata for explicitly constructed SDK resources: callers must
    // not reuse a content digest with a different epoch, rate, k20, GM or truncation.
    std::ostringstream config;
    config.imbue(std::locale::classic());
    config << version << std::hexfloat << ";mu=" << info.mu << ";radius=" << info.radius_m
           << ";degree=" << info.degree << ";order=" << info.order << ";tide=" << info.tide_system
           << ";k20=" << info.solid_tide_k20;
    if (info.secular_variations) {
        config << ";epoch-tai-ns=" << info.secular_variations->reference_epoch.tai_nanoseconds();
        for (const auto &v : info.secular_variations->rates_per_year)
            config << ';' << v.degree << ',' << v.order << ',' << v.c << ',' << v.s;
    }
    return {"gravity/" + selection, info.model, info.sha256, version,
            picosha2::hash256_hex_string(config.str())};
}
} // namespace orbitforge::runtime
