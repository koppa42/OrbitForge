#include "interpolation_identity.hpp"
#include <orbitforge/application/project.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/gravity.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/ocean_tides.hpp>
#include <orbitforge/runtime/space_weather.hpp>
#include <sstream>

namespace orbitforge::runtime {
namespace {
std::string hpop_algorithm(const core::HpopOptions &options,
                           const core::HpopEnvironment &environment) {
    auto algorithm =
        std::string(options.uses_vop() ? "hpop-vop-universal-osculating-gcrf-tai-v1;"
                                       : "hpop-cowell-gcrf-tai-v1;") +
        (options.integrator == core::HpopIntegrator::rkf78 ? "rkf78-7th-fehlberg-r287-v1;"
         : options.integrator == core::HpopIntegrator::rkv89
             ? "rkv89-efficient-8th-rklib-a1bf2d26-v1;"
         : options.integrator == core::HpopIntegrator::bulirsch_stoer
             ? (options.uses_fixed_step() ? "bulirsch-stoer-midpoint-richardson-fixed18-v2;"
                                          : "bulirsch-stoer-midpoint-richardson-4to18-cost-v1;")
         : options.integrator == core::HpopIntegrator::gauss_jackson
             ? (options.correction == core::HpopCorrection::full
                    ? "gauss-jackson12-summed-bs14to18-startup-full-v3;"
                    : "gauss-jackson12-summed-bs14to18-startup-pseudo-v3;")
             : "rk4-fixed-grid-v2;") +
        (options.gravity_model.empty() ? std::string(core::egm96_model_id())
                                       : options.gravity_model) +
        ";degree=" + std::to_string(options.gravity_degree) +
        ";order=" + std::to_string(options.gravity_order);
    if (options.time_regularization) {
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat << ";time=sundman-initial-ellipse-bounded-v1;exponent="
               << options.regularization_exponent << ";steps-per-orbit=" << options.steps_per_orbit
               << (options.uses_fixed_step() ? ";steps=sundman-fixed-phase-v1"
                                             : ";steps=sundman-relative-physical-bounds-v1");
        algorithm += config.str();
    } else {
        algorithm += options.uses_fixed_step() ? ";steps=fixed-grid-v1"
                                               : ";steps=relative-mixed-max-span-v2";
    }
    if (options.uses_secular_gravity())
        algorithm += ";secular=file-linear-julian-year-v1";
    if (options.uses_eop_force() && environment.eop)
        algorithm += ";data=" + application::eop_data_lock(environment.eop->data_info()).sha256();
    if (!options.gravity_model.empty() && environment.gravity)
        algorithm += ";gravity=" +
                     application::DataLock(
                         {gravity_reference(options.gravity_model, environment.gravity->info())})
                         .sha256();
    if (options.uses_earth_fixed_gravity()) {
        const auto tides =
            core::effective_solid_tide_options(options.solid_tides, options.gravity_degree);
        const auto field =
            options.gravity_model.empty() ? core::builtin_egm96() : environment.gravity;
        std::ostringstream love;
        love.imbue(std::locale::classic());
        love << std::hexfloat << ";solid-tides=iers2003-anelastic-v3;k20="
             << (field ? field->info().solid_tide_k20 : 0.30190);
        algorithm += love.str() + ";mode=" + std::to_string(static_cast<int>(tides.mode)) +
                     ";tide-truncate=" + std::to_string(tides.truncate_to_gravity) +
                     (tides.include_time_dependent_terms
                          ? ";frequency=iers2003-71;pole=iers2003-printed-annual-v1"
                          : ";frequency=off;pole=off");
        if (options.uses_full_solid_tides() && environment.ephemeris) {
            std::ostringstream constants;
            constants.imbue(std::locale::classic());
            constants << std::hexfloat
                      << ";tide-sun-mu=" << environment.ephemeris->data_info().sun_mu
                      << ";tide-moon-mu=" << environment.ephemeris->data_info().moon_mu;
            algorithm += constants.str();
        }
    }
    if (options.ocean_tides.active() && environment.ocean_tides) {
        const auto &o = options.ocean_tides;
        algorithm +=
            ";ocean-tides=iers2010-potential-original-mean-pole-v1;pole=automatic"
            ";ocean-degree=" +
            std::to_string(o.degree) + ";ocean-order=" + std::to_string(o.order) + ";ocean-data=" +
            application::DataLock({ocean_tide_reference(o.model, environment.ocean_tides->info())})
                .sha256();
    }
    if (options.include_relativistic_accelerations && environment.ephemeris) {
        std::ostringstream constants;
        constants.imbue(std::locale::classic());
        constants << std::hexfloat << ";relativity=iers2003-three-terms-v1;beta=1;gamma=1"
                  << ";c=299792458;earth-J-per-mass=980000000;pole=itrf-z;velocity=tai"
                  << ";relativity-earth-mu=" << options.gravity_mu
                  << ";relativity-sun-mu=" << environment.ephemeris->data_info().sun_mu;
        algorithm += constants.str();
    }
    if (options.uses_third_bodies() && environment.ephemeris) {
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat;
        for (auto body : options.enabled_third_bodies()) {
            const auto &settings = options.third_body(body);
            config << ";" << core::third_body_name(body)
                   << "-target=" << core::third_body_naif_id(body)
                   << ";source=" << static_cast<int>(settings.source) << ";mu="
                   << core::third_body_mu(settings, body, environment.ephemeris->data_info());
            if (settings.source == core::ThirdBodyGravitySource::cb_file)
                config << ";cb=" << settings.cb->content_sha256;
        }
        algorithm += config.str();
    }
    if (options.srp.enabled) {
        const auto o = core::effective_srp_options(options.srp);
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat << ";srp=spherical-uniform-angular-union-v3"
               << ";sun-method=" << static_cast<int>(o.sun_position_method) << ";sun-correction="
               << (o.sun_position_method == core::SrpSunPositionMethod::geometric ? "NONE"
                   : o.sun_position_method == core::SrpSunPositionMethod::apparent_to_true_cb
                       ? "earth-LT+S-v1"
                       : "spacecraft-LT+S-tai-rate-v1")
               << ";occultors=geometric"
               << ";shadow=" << static_cast<int>(o.shadow_model)
               << ";earth-shadow=" << o.earth_shadow << ";moon-shadow=" << o.moon_shadow
               << ";cr=" << o.cr << ";area-mass=" << o.area_mass_ratio_m2_per_kg
               << ";atmosphere-height=" << o.atmospheric_altitude_m
               << ";pressure=" << o.pressure_at_reference_pa
               << ";reference-distance=" << o.reference_distance_m
               << ";sun-radius=" << o.sun_radius_m << ";earth-radius=" << o.earth_radius_m
               << ";moon-radius=" << o.moon_radius_m;
        if (o.boundary_mitigation)
            config << ";srp-boundary=contacts-one-sided-bs-v1;check=60s;root=1ns-or-16ulp";
        for (const auto &body : o.additional_eclipsing_bodies)
            config << ";occultor-centre=" << core::srp_eclipsing_body_naif_id(body.body)
                   << ";radius=" << body.radius_m;
        algorithm += config.str();
    }
    if (options.drag.enabled) {
        const auto d = core::effective_drag_options(options.drag);
        std::ostringstream config;
        config.imbue(std::locale::classic());
        config << std::hexfloat << ";drag=spherical-corotating-itrf-wgs84-v2"
               << ";density-altitude="
               << (d.use_approximate_altitude ? "radial-wgs84-v1" : "geodetic-wgs84-v1")
               << ";atmosphere=" << static_cast<int>(d.atmosphere)
               << ";density-kernel=jr-gmat-c9b78bd;nrlmsise00-orekit-13.1.2-gtd7d"
               << ";cd=" << d.cd << ";drag-area-mass=" << d.area_mass_ratio_m2_per_kg
               << ";low-altitude=" << static_cast<int>(d.low_altitude_model)
               << ";blend=" << d.blending_range_m
               << ";sun=" << (d.use_apparent_sun_position ? "earth-LT+S" : "NONE");
        if (d.weather_model.empty())
            config << ";weather=manual-daily;f107=" << d.f107 << ";f107-average=" << d.f107_average
                   << ";kp=" << d.kp;
        algorithm += config.str();
        if (!d.weather_model.empty() && environment.weather) {
            if (d.geomagnetic_update == core::GeomagneticUpdate::three_hourly_interpolated)
                algorithm += ";geomag-interp=natural-primitive-quadratic-utc-runs-v1;"
                             "bounds=cell-mean-scaling";
            if (d.geomagnetic_update == core::GeomagneticUpdate::three_hourly_cubic_spline)
                algorithm += ";geomag-spline=natural-utc-midpoint-runs-v1;overshoot=error";
            algorithm +=
                ";weather=file;boundary=utc-one-sided-v1;update=" +
                std::to_string(static_cast<int>(d.geomagnetic_update)) + ";source=" +
                std::to_string(static_cast<int>(environment.weather->info().cssi_predictions
                                                    ? core::GeomagneticSource::ap
                                                    : d.geomagnetic_source)) +
                ";weather-data=" +
                application::DataLock(
                    {space_weather_reference(d.weather_model, environment.weather->info())})
                    .sha256();
        }
    }
    if (options.uses_ephemeris() && environment.ephemeris) {
        algorithm +=
            ";ephemeris=" +
            application::DataLock(
                {ephemeris_reference(options.ephemeris_model, environment.ephemeris->data_info())})
                .sha256();
    }
    return algorithm;
}
} // namespace
application::Session make_hpop_session(core::CartesianState initial, core::HpopOptions options,
                                       core::HpopEnvironment environment, std::string object_id,
                                       std::uint64_t revision) {
    auto algorithm = hpop_algorithm(options, environment);
    return {core::make_hpop(initial, options, std::move(environment)),
            {std::move(object_id), revision, std::move(algorithm)}};
}
application::Session make_hpop_ephemeris_session(core::CartesianState initial, core::Epoch end,
                                                 core::HpopOptions options,
                                                 core::HpopEnvironment environment,
                                                 core::OrbitInterpolationOptions interpolation,
                                                 std::int64_t reporting_step_nanoseconds,
                                                 std::string object_id, std::uint64_t revision,
                                                 const std::function<void()> &checkpoint) {
    core::validate_orbit_interpolation(interpolation);
    options.interpolation = interpolation;
    auto algorithm = hpop_algorithm(options, environment) +
                     ";ephemeris=continuous-accepted-gated-v1;from=" +
                     std::to_string(initial.epoch.tai_nanoseconds()) +
                     ";to=" + std::to_string(end.tai_nanoseconds()) +
                     ";report-ns=" + std::to_string(reporting_step_nanoseconds) +
                     ";interpolation=bracketed-nearest-v1;method=" +
                     detail::interpolation_name(interpolation.method) +
                     ";interpolation-order=" + std::to_string(interpolation.order) +
                     detail::interpolation_mu_identity(interpolation);
    auto arcs = core::propagate_hpop_ephemeris(initial, end, options, std::move(environment),
                                               reporting_step_nanoseconds, checkpoint);
    return {core::make_tabulated_orbit(std::move(arcs), interpolation, checkpoint),
            {std::move(object_id), revision, std::move(algorithm)}};
}
application::Session make_hpop_interpolated_session(core::CartesianState initial, core::Epoch first,
                                                    core::Epoch last, core::HpopOptions options,
                                                    core::HpopEnvironment environment,
                                                    std::int64_t reporting_step_nanoseconds,
                                                    std::string object_id, std::uint64_t revision) {
    auto algorithm = hpop_algorithm(options, environment) +
                     ";ephemeris=bounded-accepted-support-v1;initial=" +
                     std::to_string(initial.epoch.tai_nanoseconds()) +
                     ";first=" + std::to_string(first.tai_nanoseconds()) +
                     ";last=" + std::to_string(last.tai_nanoseconds()) +
                     ";report-ns=" + std::to_string(reporting_step_nanoseconds) +
                     ";interpolation=bracketed-nearest-v1;method=" +
                     detail::interpolation_name(options.interpolation.method) +
                     ";interpolation-order=" + std::to_string(options.interpolation.order) +
                     detail::interpolation_mu_identity(options.interpolation);
    return {core::make_hpop_interpolated(initial, first, last, options, std::move(environment),
                                         reporting_step_nanoseconds),
            {std::move(object_id), revision, std::move(algorithm)}};
}
} // namespace orbitforge::runtime
