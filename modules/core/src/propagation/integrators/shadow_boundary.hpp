#pragma once
#include "../srp_shadow.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <orbitforge/core/propagation/state_provider.hpp>

namespace orbitforge::core::detail {
inline double shadow_time_resolution(double begin, double end) {
    return std::max(1e-9, 16 * std::numeric_limits<double>::epsilon() *
                              std::max(std::abs(begin), std::abs(end)));
}
// Search accepted step geometry in propagation order. Samples and root probes
// use the caller's integrated trajectory, never an interpolated output orbit.
// 60 s is a maximum checking interval, not a replacement integration step.
// As with finite-check event detectors, arbitrary faster oscillating user
// ephemerides cannot be certified. Interior extrema also catch short grazing
// eclipses with equal signs at the sample endpoints.
template <class Sample>
std::optional<double> first_shadow_boundary(double begin, double end, const Sample &sample) {
    const double direction = end > begin ? 1 : -1;
    const double span = std::abs(end - begin);
    const double tolerance = shadow_time_resolution(begin, end);
    if (span <= 8 * tolerance)
        return {};
    const double first = 4 * tolerance;
    std::map<double, ShadowContacts> cache;
    const auto value = [&](double offset) -> const ShadowContacts & {
        const auto found = cache.find(offset);
        if (found != cache.end())
            return found->second;
        auto contacts = sample(offset == span ? end : begin + direction * offset);
        if (contacts.size > contacts.values.size() ||
            !std::all_of(contacts.values.begin(), contacts.values.begin() + contacts.size,
                         [](double v) { return std::isfinite(v); }))
            throw PropagationError("SRP contact search produced invalid geometry");
        return cache.emplace(offset, contacts).first->second;
    };
    const auto count = value(first).size;
    if (count == 0)
        return {};
    std::optional<double> earliest;
    const auto get = [&](double at, std::size_t body) {
        const auto &contacts = value(at);
        if (contacts.size != count)
            throw PropagationError("SRP contact assignments changed during integration");
        return contacts.values[body];
    };
    const auto opposite = [](double a, double b) { return (a < 0) != (b < 0); };
    const auto root = [&](double left, double right, std::size_t body) {
        double g_left = get(left, body), g_right = get(right, body);
        if (g_left == 0 && g_right == 0)
            return;
        if (g_left != 0 && g_right != 0 && !opposite(g_left, g_right))
            return;
        if (g_left == 0)
            right = left;
        else if (g_right != 0) {
            for (unsigned i = 0; right - left > tolerance; ++i) {
                if (i == 80)
                    throw PropagationError("SRP shadow contact failed to converge");
                const double mid = left + .5 * (right - left), g_mid = get(mid, body);
                if (g_mid == 0) {
                    right = mid;
                    break;
                }
                if (opposite(g_left, g_mid))
                    right = mid;
                else {
                    left = mid;
                    g_left = g_mid;
                }
            }
        }
        // The outgoing side of the bracket avoids retriggering the same contact.
        if (right > first && (!earliest || right < *earliest))
            earliest = right;
    };
    const auto steps = static_cast<std::size_t>(std::max(2., std::ceil(span / 60.)));
    const auto grid = [&](std::size_t i) {
        return i == 0 ? first : (i == steps ? span : span * static_cast<double>(i) / steps);
    };
    for (std::size_t i = 1; i <= steps; ++i) {
        const double left = grid(i - 1), right = grid(i);
        for (std::size_t body = 0; body < count; ++body)
            root(left, right, body);
        if (i < 2)
            continue;
        const double a = grid(i - 2), b = left, c = right;
        for (std::size_t body = 0; body < count; ++body) {
            const double fa = get(a, body), fb = get(b, body), fc = get(c, body);
            const double sense = fb < 0 ? -1 : 1;
            if (opposite(fa, fb) || opposite(fb, fc) ||
                !(sense * fb < sense * fa && sense * fb < sense * fc))
                continue;
            // Bracketed, derivative-free extremum: no angular-rate division near
            // a grazing contact. Search both brief eclipses and brief sunlight.
            double lo = a, hi = c;
            constexpr double fraction = .3819660112501051518;
            double x = lo + fraction * (hi - lo), y = hi - fraction * (hi - lo);
            double fx = sense * get(x, body), fy = sense * get(y, body);
            for (unsigned iteration = 0; hi - lo > tolerance; ++iteration) {
                if (iteration == 100)
                    throw PropagationError("SRP grazing search failed to converge");
                if (fx < fy) {
                    hi = y;
                    y = x;
                    fy = fx;
                    x = lo + fraction * (hi - lo);
                    fx = sense * get(x, body);
                } else {
                    lo = x;
                    x = y;
                    fx = fy;
                    y = hi - fraction * (hi - lo);
                    fy = sense * get(y, body);
                }
            }
            const double extremum = fx < fy ? x : y;
            if (sense * get(extremum, body) < 0) {
                root(a, extremum, body);
                root(extremum, c, body);
            }
        }
        // A minimum in the next triple can only introduce a root after b.
        if (earliest && *earliest < b)
            break;
    }
    return earliest ? std::optional<double>{begin + direction * *earliest} : std::nullopt;
}
} // namespace orbitforge::core::detail
