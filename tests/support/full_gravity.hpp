#pragma once
#include <algorithm>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <orbitforge/core/propagation/hpop.hpp>
#include <sstream>

namespace full_gravity_test {
using namespace orbitforge::core;
class Frames final : public IFrameProvider {
  public:
    PositionRotation gcrf_to_itrf(Epoch e, const EarthOrientationSample &) const override {
        return {e,
                OriginId::earth,
                FrameId::gcrf,
                FrameId::itrf,
                {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
                "reference-identity",
                "test"};
    }
    StateTransform gcrf_to_itrf_state(Epoch e, const EarthOrientationState &o) const override {
        return {gcrf_to_itrf(e, o.sample), {}, "reference-static"};
    }
};
class Eop final : public IEarthOrientationProvider {
    EopDataInfo info_{"reference",
                      {},
                      {},
                      {},
                      {},
                      Epoch{-1000000000000000LL},
                      Epoch{1000000000000000LL},
                      {"test", {}, {}, 0, 100000}};

  public:
    EarthOrientationState at(Epoch e) const override { return {{e, 0, 0, 0, 0, 0, "test"}, {}}; }
    const EopDataInfo &data_info() const noexcept override { return info_; }
};
inline void verify(const std::shared_ptr<const GravityModel> &model, int profile,
                   const std::string &path, int expected_accelerations, int expected_trajectories) {
    std::ifstream file(path);
    ASSERT_TRUE(file.is_open());
    int accelerations = 0, trajectories = 0;
    double max_a = 0, max_r = 0, max_v = 0;
    std::shared_ptr<const IStateProvider> provider;
    const auto start = std::chrono::steady_clock::now();
    for (std::string line; std::getline(file, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in(line);
        char tag;
        int selected, degree, order;
        in >> tag >> selected >> degree >> order;
        ASSERT_TRUE(in);
        if (selected != profile)
            continue;
        if (tag == 'A') {
            std::array<double, 3> r{}, a{};
            for (auto &v : r)
                in >> v;
            for (auto &v : a)
                in >> v;
            ASSERT_TRUE(in);
            const auto actual = model->acceleration(r, degree, order);
            for (int i = 0; i < 3; ++i) {
                EXPECT_NEAR(actual[i], a[i], 5e-12) << line;
                max_a = std::max(max_a, std::abs(actual[i] - a[i]));
            }
            ++accelerations;
        } else {
            ASSERT_EQ(tag, 'T');
            long long seconds;
            std::array<double, 3> r{}, v{};
            in >> seconds;
            for (auto &x : r)
                in >> x;
            for (auto &x : v)
                in >> x;
            ASSERT_TRUE(in);
            if (!provider) {
                HpopOptions options;
                options.gravity_model = "reference";
                options.gravity_mu = model->info().mu;
                options.gravity_degree = degree;
                options.gravity_order = order;
                options.max_step_seconds = 5;
                options.relative_tolerance = 1e-14;
                options.position_tolerance_m = 1e-8;
                options.velocity_tolerance_mps = 1e-11;
                provider = make_hpop(
                    {Epoch{0}, OriginId::earth, FrameId::gcrf, {6408136.3, 0, 0}, {0, 6200, 4700}},
                    options, {std::make_shared<Frames>(), std::make_shared<Eop>(), model});
            }
            const auto actual = provider->state_at(Epoch{seconds * 1000000000LL});
            for (int i = 0; i < 3; ++i) {
                EXPECT_NEAR(actual.position_m[i], r[i], 1e-4) << line;
                EXPECT_NEAR(actual.velocity_mps[i], v[i], 1e-7) << line;
                max_r = std::max(max_r, std::abs(actual.position_m[i] - r[i]));
                max_v = std::max(max_v, std::abs(actual.velocity_mps[i] - v[i]));
            }
            ++trajectories;
        }
    }
    EXPECT_EQ(accelerations, expected_accelerations);
    EXPECT_EQ(trajectories, expected_trajectories);
    std::cout << "Full gravity profile " << profile << ": max component errors " << max_a
              << " m/s^2, " << max_r << " m, " << max_v << " m/s; elapsed "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " s\n";
}
} // namespace full_gravity_test
