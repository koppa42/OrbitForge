#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <orbitforge/core/propagation/solid_tides.hpp>
#include <stdexcept>

namespace orbitforge::core {
namespace {
constexpr std::array k2_real{0.30190, 0.29830, 0.30102};
constexpr std::array k2_imag{0.0, -0.00144, -0.00130};
constexpr std::array k2_plus{-0.00089, -0.00080, -0.00057};
constexpr std::array k3_real{0.093, 0.093, 0.093, 0.094};
constexpr double arcsec = std::numbers::pi / (180 * 3600);
constexpr double julian_year_seconds = 365.25 * 86400;
struct FrequencyTerm {
    int order;
    std::array<int, 5> delaunay;   // Printed table multipliers; phase = m * gamma - dot(args).
    double in_phase, out_of_phase; // 1e-12, already includes the forcing amplitude.
};
#include "solid_tide_data.inc"

double polynomial(const std::array<double, 5> &a, double t) {
    return ((((a[4] * t + a[3]) * t + a[2]) * t + a[1]) * t + a[0]);
}
std::array<double, 2> mean_pole(Epoch epoch, const ITimeProvider &time) {
    const auto utc =
        time.to_utc(epoch); // Enforce the confirmed UTC coverage, including future dates.
    if (utc.year < 1900)
        throw std::out_of_range("IERS 2003 mean pole starts at 1900-01-01 UTC");
    if (utc.year >= 2000) {
        // Preserve the final tabulated value at the exact endpoint, as in IERS/Orekit.
        if (utc.year == 2000 && utc.month == 1 && utc.day == 1 && utc.hour == 0 &&
            utc.minute == 0 && utc.second == 0 && utc.nanosecond == 0)
            return annual_mean_pole.back();
        const double years = (epoch.seconds_since(Epoch{0}) + 32.184) / julian_year_seconds;
        return {.054 + .00083 * years, .357 + .00395 * years};
    }
    const auto first = time.from_utc({utc.year, 1, 1, 0, 0, 0, 0});
    const auto last = time.from_utc({utc.year + 1, 1, 1, 0, 0, 0, 0});
    const double fraction = epoch.seconds_since(first) / last.seconds_since(first);
    const auto &a = annual_mean_pole.at(static_cast<std::size_t>(utc.year - 1900));
    const auto &b = annual_mean_pole.at(static_cast<std::size_t>(utc.year - 1899));
    return {a[0] + fraction * (b[0] - a[0]), a[1] + fraction * (b[1] - a[1])};
}
std::array<std::complex<double>, 3> time_dependent_tides(const EarthOrientationSample &eop,
                                                         const ITimeProvider &time) {
    if (!std::isfinite(eop.ut1_minus_tai_seconds) || !std::isfinite(eop.polar_motion_x_rad) ||
        !std::isfinite(eop.polar_motion_y_rad))
        throw std::invalid_argument(
            "Time-dependent solid tides require finite UT1 and polar motion");
    const double t = (eop.epoch.seconds_since(Epoch{0}) + 32.184) / (100 * julian_year_seconds);
    // IERS 2003 luni-solar fundamental arguments, TT centuries from J2000 TT.
    constexpr std::array<std::array<double, 5>, 5> polynomials{
        {{134.96340251 * 3600, 1717915923.2178, 31.8792, .051635, -.00024470},
         {357.52910918 * 3600, 129596581.0481, -.5532, .000136, -.00001149},
         {93.27209062 * 3600, 1739527262.8478, -12.7512, -.001037, .00000417},
         {297.85019547 * 3600, 1602961601.2090, -6.3706, .006593, -.00003169},
         {125.04455501 * 3600, -6962890.5431, 7.4722, .007702, -.00005939}}};
    std::array<double, 5> args{};
    for (std::size_t i = 0; i < args.size(); ++i)
        args[i] = std::remainder(polynomial(polynomials[i], t) * arcsec, 2 * std::numbers::pi);
    // Split TAI into whole days and a sub-day remainder before adding UT1-TAI.
    constexpr std::int64_t day_ns = 86400000000000LL;
    const double days = static_cast<double>(eop.epoch.tai_nanoseconds() / day_ns);
    const double fraction = (static_cast<double>(eop.epoch.tai_nanoseconds() % day_ns) * 1e-9 +
                             eop.ut1_minus_tai_seconds) /
                            86400;
    const double era =
        2 * std::numbers::pi * (.7790572732640 + fraction + .00273781191135448 * (days + fraction));
    // IERS 2003 table 5.4 polynomial; not the IAU 2006 GMST polynomial.
    const double gamma =
        era + arcsec * polynomial({.014506, 4612.15739966, 1.39667721, -.00009344, .00001882}, t) +
        std::numbers::pi;
    std::array<std::complex<double>, 3> delta{};
    for (const auto &term : frequency_terms) {
        double phase = term.order * gamma;
        for (std::size_t i = 0; i < args.size(); ++i)
            phase -= term.delaunay[i] * args[i];
        const double s = std::sin(phase), c = std::cos(phase);
        const double ip = term.in_phase * 1e-12, op = term.out_of_phase * 1e-12;
        if (term.order == 0)
            delta[0] += ip * c - op * s;
        else if (term.order == 1)
            delta[1] += std::complex<double>{ip * s + op * c, ip * c - op * s};
        else
            delta[2] += std::complex<double>{ip * c, -ip * s};
    }
    const auto pole = mean_pole(eop.epoch, time);
    const double m1 = eop.polar_motion_x_rad / arcsec - pole[0];
    const double m2 = pole[1] - eop.polar_motion_y_rad / arcsec;
    // Printed IERS 2003 section 6.2 signs, retained explicitly for reproducibility.
    delta[1] += std::complex<double>{-1.333e-9 * (m1 - .0115 * m2), -1.333e-9 * (m2 + .0115 * m1)};
    return delta;
}

} // namespace
double iers2003_permanent_tide_c20(double k20) {
    if (!std::isfinite(k20))
        throw std::invalid_argument("Solid tide k20 must be finite");
    return 4.4228e-8 * -0.31460 * k20;
}
void validate_solid_tide_options(const SolidTideOptions &options) {
    if (options.mode != SolidTideMode::none && options.mode != SolidTideMode::permanent &&
        options.mode != SolidTideMode::full)
        throw std::invalid_argument("Unsupported solid tide mode");
}
SolidTideOptions effective_solid_tide_options(SolidTideOptions options, int gravity_degree) {
    validate_solid_tide_options(options);
    if (gravity_degree == 0 && options.truncate_to_gravity)
        return {};
    if (options.mode == SolidTideMode::none ||
        (options.mode == SolidTideMode::permanent && gravity_degree >= 2))
        options.truncate_to_gravity = true;
    if (options.mode != SolidTideMode::full)
        options.include_time_dependent_terms = false;
    return options;
}
std::vector<GravityCoefficient>
solid_tide_corrections(const GravityInfo &field, const SolidTideOptions &options, int degree,
                       int order, std::span<const TideRaisingBody> bodies,
                       const EarthOrientationSample *eop, const ITimeProvider *time) {
    validate_solid_tide_options(options);
    if (!std::isfinite(field.mu) || field.mu <= 0 || !std::isfinite(field.radius_m) ||
        field.radius_m <= 0 || !std::isfinite(field.solid_tide_k20) ||
        (field.tide_system != "tide_free" && field.tide_system != "zero_tide"))
        throw std::invalid_argument("Solid tides require a tide-free or zero-tide Earth field");
    if (degree < 0 || degree == 1 || degree > std::min(field.degree, gravity_max_degree) ||
        order < 0 || order > std::min(degree, field.order))
        throw std::invalid_argument("Invalid gravity truncation for solid tides");
    if (options.truncate_to_gravity && degree == 0)
        return {};
    std::array<std::array<std::complex<double>, 4>, 5> delta{};
    if (options.mode == SolidTideMode::permanent)
        delta[2][0] = iers2003_permanent_tide_c20(field.solid_tide_k20);
    else if (options.mode == SolidTideMode::full) {
        if (bodies.size() != 2)
            throw std::invalid_argument("Full solid tides require geometric Sun and Moon states");
        for (const auto &body : bodies) {
            const auto &r = body.position_itrf_m;
            const double distance = std::hypot(r[0], r[1], r[2]);
            if (!std::isfinite(distance) || distance <= field.radius_m || !std::isfinite(body.mu) ||
                body.mu <= 0)
                throw std::invalid_argument("Invalid solid-tide raising body geometry or GM");
            const double z = r[2] / distance;
            const std::complex<double> xy{r[0] / distance, r[1] / distance};
            // Explicit fully normalized Cartesian surface harmonics, no longitude
            // division. Well-defined even when a tide-raising body is at a pole.
            const std::array p2{std::complex<double>{std::sqrt(5.0) * (3 * z * z - 1) / 2},
                                std::sqrt(15.0) * z * xy, std::sqrt(15.0) / 2 * xy * xy};
            const std::array p3{std::complex<double>{std::sqrt(7.0) / 2 * z * (5 * z * z - 3)},
                                std::sqrt(21.0 / 8) * (5 * z * z - 1) * xy,
                                std::sqrt(105.0) / 2 * z * xy * xy,
                                std::sqrt(35.0 / 8) * xy * xy * xy};
            const double ratio = field.radius_m / distance;
            const double factor2 = body.mu / field.mu * ratio * ratio * ratio / 5;
            const double factor3 = factor2 * ratio * (5.0 / 7);
            for (std::size_t m = 0; m < p2.size(); ++m) {
                // C + i S = conjugate(k) * Pnm exp(i m lambda) * scale.
                const double real = m == 0 ? field.solid_tide_k20 : k2_real[m];
                delta[2][m] += std::complex<double>{real, -k2_imag[m]} * factor2 * p2[m];
                delta[4][m] += k2_plus[m] * factor2 * p2[m];
            }
            for (std::size_t m = 0; m < p3.size(); ++m)
                delta[3][m] += k3_real[m] * factor3 * p3[m];
        }
    }
    if (options.mode == SolidTideMode::full && options.include_time_dependent_terms) {
        if (!eop || !time)
            throw std::invalid_argument("Time-dependent solid tides require EOP and UTC providers");
        const auto secondary = time_dependent_tides(*eop, *time);
        for (std::size_t m = 0; m < secondary.size(); ++m)
            delta[2][m] += secondary[m];
    }
    if (field.tide_system == "zero_tide" && degree >= 2)
        delta[2][0] -= iers2003_permanent_tide_c20(field.solid_tide_k20);
    std::vector<GravityCoefficient> result;
    result.reserve(10);
    for (int n = 2; n <= 4; ++n)
        for (int m = 0; m <= std::min(n, n == 4 ? 2 : 3); ++m) {
            if (options.truncate_to_gravity && (n > degree || m > order))
                continue;
            const auto value = delta[static_cast<std::size_t>(n)][static_cast<std::size_t>(m)];
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
                throw std::invalid_argument("Nonfinite solid tide correction");
            if (value != std::complex<double>{})
                result.push_back({n, m, value.real(), value.imag()});
        }
    return result;
}
} // namespace orbitforge::core
