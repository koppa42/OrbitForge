#include <algorithm>
#include <cmath>
#include <numbers>
#include <orbitforge/core/propagation/ocean_tides.hpp>
#include <stdexcept>
#include <tuple>

namespace orbitforge::core {
namespace {
constexpr double arcsec = std::numbers::pi / (180 * 3600);
constexpr double year_seconds = 365.25 * 86400;
std::size_t index(int n, int m) { return static_cast<std::size_t>(n * (n + 1) / 2 + m); }
void require(bool valid, const char *message) {
    if (!valid)
        throw std::invalid_argument(message);
}
std::array<double, 6> arguments(const EarthOrientationSample &eop) {
    const auto ns = eop.epoch.tai_nanoseconds();
    const double t = (eop.epoch.seconds_since(Epoch{0}) + 32.184) / (100 * year_seconds);
    constexpr std::array<std::array<double, 5>, 5> polynomial{
        {{134.96340251 * 3600, 1717915923.2178, 31.8792, .051635, -.00024470},
         {357.52910918 * 3600, 129596581.0481, -.5532, .000136, -.00001149},
         {93.27209062 * 3600, 1739527262.8478, -12.7512, -.001037, .00000417},
         {297.85019547 * 3600, 1602961601.2090, -6.3706, .006593, -.00003169},
         {125.04455501 * 3600, -6962890.5431, 7.4722, .007702, -.00005939}}};
    std::array<double, 6> result{};
    for (std::size_t i = 0; i < polynomial.size(); ++i) {
        const auto &a = polynomial[i];
        result[i + 1] =
            std::remainder(((((a[4] * t + a[3]) * t + a[2]) * t + a[1]) * t + a[0]) * arcsec,
                           2 * std::numbers::pi);
    }
    constexpr std::int64_t day_ns = 86400000000000LL;
    const double days = static_cast<double>(ns / day_ns);
    const double fraction =
        (static_cast<double>(ns % day_ns) * 1e-9 + eop.ut1_minus_tai_seconds) / 86400;
    const double era =
        2 * std::numbers::pi * (.7790572732640 + fraction + .00273781191135448 * (days + fraction));
    // IAU 2006 GMST, IERS 2010. Solid tides intentionally retain IERS 2003 GMST.
    result[0] = era + std::numbers::pi +
                arcsec * (((((-.0000000368 * t - .000029956) * t - .00000044) * t + 1.3915817) * t +
                           4612.156534) *
                              t +
                          .014506);
    return result;
}
std::array<double, 2> pole_tide(const EarthOrientationSample &eop) {
    require(std::isfinite(eop.polar_motion_x_rad) && std::isfinite(eop.polar_motion_y_rad),
            "Ocean pole tide requires finite polar motion");
    const double t = (eop.epoch.seconds_since(Epoch{0}) + 32.184) / year_seconds;
    // Exact TAI nanoseconds of 2010-01-01 00:00 TT, not UTC or J2000 noon.
    const bool cubic = eop.epoch.tai_nanoseconds() <= 315575967816000000LL;
    const double mean_x =
        (cubic ? ((.007024 * t + .18413) * t + 1.8243) * t + 55.974 : 23.513 + 7.6141 * t) * .001;
    const double mean_y =
        (cubic ? ((-.000908 * t - .10729) * t + 1.7896) * t + 346.346 : 358.891 - .6287 * t) * .001;
    const double m1 = eop.polar_motion_x_rad / arcsec - mean_x;
    const double m2 = mean_y - eop.polar_motion_y_rad / arcsec;
    return {-2.1778e-10 * (m1 - .01724 * m2), -1.7232e-10 * (m2 - .03365 * m1)};
}
} // namespace
void validate_ocean_tide_options(const OceanTideOptions &o) {
    require(!o.model.empty() && o.model.size() <= 64 &&
                std::all_of(o.model.begin(), o.model.end(),
                            [](char ch) {
                                return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                                       ch == '-';
                            }),
            "Invalid ocean tide model selection");
    require(o.degree >= 0 && o.degree <= gravity_max_degree && o.order >= 0 && o.order <= o.degree,
            "Invalid ocean tide degree/order");
}
OceanTideModel::OceanTideModel(OceanTideInfo info, std::vector<OceanTideConstituent> constituents)
    : info_(std::move(info)) {
    require(!info_.model.empty() && info_.model.size() <= 512 && info_.degree >= 2 &&
                info_.degree <= gravity_max_degree && info_.order >= 0 &&
                info_.order <= info_.degree && std::isfinite(info_.mu) && info_.mu > 0 &&
                std::isfinite(info_.radius_m) && info_.radius_m > 0,
            "Invalid ocean tide metadata");
    require(!constituents.empty() && constituents.size() <= 4096,
            "Ocean tide model requires 1..4096 constituents");
    std::ranges::sort(constituents, {}, &OceanTideConstituent::doodson);
    int previous = -1, max_degree = 0, max_order = 0;
    std::size_t count = 0;
    for (auto &source : constituents) {
        const int d = source.doodson;
        require(d >= 0 && d <= 999999 && d != previous,
                "Invalid or duplicate ocean tide Doodson number");
        previous = d;
        const int ps = d % 10 - 5, np = d / 10 % 10 - 5, p = d / 100 % 10 - 5;
        const int h = d / 1000 % 10 - 5, s = d / 10000 % 10 - 5, tau = d / 100000 % 10;
        Wave wave{{tau, -p, -ps, -tau + s + h + p + ps, -h - ps, -tau + s + h + p - np + ps}, {}};
        std::ranges::sort(source.coefficients, [](const auto &a, const auto &b) {
            return std::tie(a.degree, a.order) < std::tie(b.degree, b.order);
        });
        int wn = 0, wm = 0, pn = -1, pm = -1;
        for (const auto &v : source.coefficients) {
            require(++count <= 4000000, "Ocean tide model exceeds four million records");
            require(v.degree >= 0 && v.degree <= info_.degree && v.order >= 0 &&
                        v.order <= std::min(v.degree, info_.order) &&
                        (v.degree != pn || v.order != pm),
                    "Invalid or duplicate ocean tide coefficient index");
            pn = v.degree;
            pm = v.order;
            for (double value : {v.c_plus, v.s_plus, v.c_minus, v.s_minus})
                require(std::isfinite(value), "Nonfinite ocean tide coefficient");
            if (v.degree < 2)
                continue;
            wn = std::max(wn, v.degree);
            wm = std::max(wm, v.order);
            const std::array amplitudes{v.c_plus + v.c_minus, v.s_plus + v.s_minus,
                                        v.s_plus - v.s_minus, v.c_minus - v.c_plus};
            for (double value : amplitudes)
                require(std::isfinite(value), "Ocean tide coefficient combination overflow");
            wave.terms.push_back({v.degree, v.order, amplitudes});
        }
        require(wn >= 2, "Ocean tide constituent has no geocentric force coefficients");
        std::size_t next = 0;
        for (int n = 2; n <= wn; ++n)
            for (int m = 0; m <= std::min(n, wm); ++m) {
                require(next < wave.terms.size() && wave.terms[next].degree == n &&
                            wave.terms[next].order == m,
                        "Missing coefficient within an ocean tide constituent");
                ++next;
            }
        max_degree = std::max(max_degree, wn);
        max_order = std::max(max_order, wm);
        waves_.push_back(std::move(wave));
    }
    require(max_degree == info_.degree && max_order == info_.order,
            "Ocean tide file extent does not match its metadata");
    info_.constituents = waves_.size();
}
std::vector<GravityCoefficient> OceanTideModel::corrections(const GravityInfo &field,
                                                            const EarthOrientationSample &eop,
                                                            int degree, int order,
                                                            bool include_pole) const {
    require(degree >= 0 && degree <= info_.degree && order >= 0 &&
                order <= std::min(degree, info_.order),
            "Ocean tide truncation exceeds its model");
    require(std::isfinite(field.mu) && field.mu > 0 && std::isfinite(field.radius_m) &&
                field.radius_m > 0,
            "Ocean tides require positive finite gravity scaling");
    if (degree < 2)
        return {};
    require(std::isfinite(eop.ut1_minus_tai_seconds), "Ocean tides require finite UT1-TAI");
    const auto angles = arguments(eop);
    std::vector<std::array<double, 2>> sums(index(degree + 1, 0));
    for (const auto &wave : waves_) {
        double phase = 0;
        for (std::size_t i = 0; i < angles.size(); ++i)
            phase += wave.arguments[i] * angles[i];
        const double c = std::cos(phase), s = std::sin(phase);
        for (const auto &term : wave.terms) {
            if (term.degree > degree)
                break;
            if (term.order > order)
                continue;
            const auto &a = term.amplitudes;
            auto &sum = sums[index(term.degree, term.order)];
            sum[0] += a[0] * c + a[1] * s;
            if (term.order > 0)
                sum[1] += a[2] * c + a[3] * s;
        }
    }
    if (include_pole && order >= 1) {
        const auto pole = pole_tide(eop);
        sums[index(2, 1)][0] += pole[0];
        sums[index(2, 1)][1] += pole[1];
    }
    std::vector<GravityCoefficient> result;
    result.reserve(sums.size() - 3);
    for (int n = 2; n <= degree; ++n) {
        const double scale = (info_.mu / field.mu) * std::pow(info_.radius_m / field.radius_m, n);
        require(std::isfinite(scale) && scale > 0,
                "Ocean tide gravity rescaling overflow/underflow");
        for (int m = 0; m <= std::min(n, order); ++m) {
            const auto &v = sums[index(n, m)];
            const double c = v[0] * scale, s = v[1] * scale;
            require(std::isfinite(c) && std::isfinite(s), "Nonfinite ocean tide correction");
            result.push_back({n, m, c, s});
        }
    }
    return result;
}
} // namespace orbitforge::core
