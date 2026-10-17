#include <cmath>
#include <iostream>
#include <orbitforge/application/project_document.hpp>
#include <orbitforge/application/scenario.hpp>
#include <orbitforge/core/propagation/relativity.hpp>
#include <orbitforge/runtime/access.hpp>
#include <orbitforge/runtime/analysis.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/ephemeris.hpp>
#include <orbitforge/runtime/hpop.hpp>
#include <orbitforge/runtime/j4.hpp>
#include <orbitforge/runtime/project.hpp>
#include <orbitforge/runtime/report.hpp>
#include <orbitforge/runtime/sgp4.hpp>
#include <orbitforge/runtime/tabulated_orbit.hpp>
#include <orbitforge/runtime/time_frames.hpp>
#include <orbitforge/runtime/two_body.hpp>
int main(int argc, char **argv) try {
    {
        using namespace orbitforge::core;
        SrpOptions options;
        options.enabled = true;
        options.shadow_model = SrpShadowModel::none;
        const double distance = options.reference_distance_m - 7e6;
        const double pressure =
            options.pressure_at_reference_pa * std::pow(options.reference_distance_m / distance, 2);
        const auto partials =
            srp_differential({7e6, 0, 0}, {options.reference_distance_m, 0, 0}, options);
        if (std::abs(partials.srp_parameter_jacobian_pa[0] + pressure) > 1e-20 ||
            std::abs(partials.force.position_jacobian_per_s2[0][0] +
                     2 * pressure * options.cr * options.area_mass_ratio_m2_per_kg / distance) >
                1e-30 ||
            partials.force.velocity_jacobian_per_s[0][0] != 0 ||
            partials.sun_position_jacobian_per_s2[0][0] !=
                -partials.force.position_jacobian_per_s2[0][0])
            throw std::runtime_error("Installed SRP differential API failed");
    }
    {
        using namespace orbitforge::core;
        SrpOptions options;
        options.enabled = true;
        options.earth_shadow = options.moon_shadow = false;
        options.additional_eclipsing_bodies = {default_srp_eclipsing_body(ThirdBody::venus)};
        const std::array positions{SrpBodyPosition{ThirdBody::venus, {1e8, 0, 0}}};
        if (evaluate_srp({0, 0, 0}, {149597870700., 0, 0}, options, {}, positions).lighting_ratio !=
            0)
            return 47;
    }
    {
        using namespace orbitforge::core;
        HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.vop = true;
        if (!options.uses_vop())
            return 53;
        const CartesianState initial{Epoch{0},
                                     OriginId::earth,
                                     FrameId::gcrf,
                                     {7e6, 0, 0},
                                     {0, std::sqrt(egm96_mu / 7e6), 0}};
        const auto nodes = propagate_hpop_ephemeris(initial, Epoch{600'000'000'000LL}, options, {},
                                                    30'000'000'000LL);
        if (nodes.size() != 1 || nodes[0].size() != 21)
            return 44;
        const auto session = orbitforge::runtime::make_hpop_ephemeris_session(
            initial, Epoch{600'000'000'000LL}, options, {}, {OrbitInterpolationMethod::hermite, 7},
            30'000'000'000LL);
        const auto state = session.sample(std::array{Epoch{17'123'456'789LL}}).states().front();
        if (!std::isfinite(state.position_m[0]) ||
            session.provenance().algorithm.find("hpop-vop-universal-osculating") ==
                std::string::npos)
            return 45;
        const auto vop = orbitforge::runtime::make_tabulated_orbit_session(
            nodes, {OrbitInterpolationMethod::vop, 1, egm96_mu});
        const double dt = 17.123456789, phase = std::sqrt(egm96_mu / (7e6 * 7e6 * 7e6)) * dt;
        const auto vop_state = vop.sample(std::array{Epoch{17'123'456'789LL}}).states().front();
        if (std::hypot(vop_state.position_m[0] - 7e6 * std::cos(phase),
                       vop_state.position_m[1] - 7e6 * std::sin(phase),
                       vop_state.position_m[2]) > .001 ||
            vop.provenance().algorithm.find("vop-mu=") == std::string::npos)
            return 52;
        options.interpolation = {OrbitInterpolationMethod::lagrange, 7};
        const auto bounded = orbitforge::runtime::make_hpop_interpolated_session(
            initial, Epoch{-1'000'000'000}, Epoch{1'000'000'000}, options);
        const auto short_state = bounded.sample(std::array{Epoch{123'456'789}}).states().front();
        if (!std::isfinite(short_state.position_m[0]) ||
            bounded.provenance().algorithm.find("bounded-accepted-support-v1") == std::string::npos)
            return 46;
    }
    {
        using namespace orbitforge::core;
        const OrbitEphemerisArc nodes{
            {Epoch{0}, OriginId::earth, FrameId::gcrf, {0, 0, 0}, {0, 0, 0}},
            {Epoch{2'000'000'000}, OriginId::earth, FrameId::gcrf, {8, 0, 0}, {12, 0, 0}}};
        const auto session = orbitforge::runtime::make_tabulated_orbit_session(
            {nodes}, {OrbitInterpolationMethod::hermite, 3});
        const auto state = session.sample(std::array{Epoch{1'000'000'000}}).states().front();
        if (std::abs(state.position_m[0] - 1) > 1e-12 ||
            std::abs(state.velocity_mps[0] - 3) > 1e-12)
            return 43;
    }
    {
        using namespace orbitforge::core;
        const GravityModel field(
            {"Full field", {}, {}, "tide_free", 3.986004415e14, 6378136.3, 2190, 2159},
            {{2, 0, -4.84165371736e-4, 0}, {2190, 2159, 2e-8, -3e-8}});
        const auto full = field.acceleration({6378146.3, 0, 0}, 2190, 2159);
        const auto truncated = field.acceleration({6378146.3, 0, 0}, 2189, 2159);
        if (gravity_max_degree != 2190 || !std::isfinite(full[2]) ||
            std::abs(full[2] - truncated[2]) < 1e-8)
            return 34;
        const auto third = third_body_differential({}, {0, 1e8, 0}, 1e12);
        const auto rel = relativity_differential({7e6, 0, 0}, {}, 3.986004415e14, {1.496e11, 0, 0},
                                                 {0, 30000, 0}, 1.3271244004127942e20, {0, 0, 1});
        if (std::abs(third.position_jacobian_per_s2[1][1] - 2e-12) > 1e-27 ||
            rel.lense_thirring.acceleration_mps2 != std::array<double, 3>{} ||
            rel.lense_thirring.velocity_jacobian_per_s[0][1] == 0 ||
            rel.de_sitter.velocity_jacobian_per_s[0][1] == 0)
            return 57;
        const auto differential = field.differential({6378146.3, 0, 0}, 2190, 2159);
        const auto &jacobian = differential.position_jacobian_per_s2;
        if (differential.acceleration_mps2 != full || !std::isfinite(jacobian[0][2]) ||
            jacobian[0][2] != jacobian[2][0] ||
            std::abs(jacobian[0][0] + jacobian[1][1] + jacobian[2][2]) > 1e-18)
            return 56;
        try {
            (void)field.acceleration({6378146.3, 0, 0}, 2190, 2160);
            return 35;
        } catch (const std::invalid_argument &) {
        }
    }
    {
        using namespace orbitforge::core;
        auto info = builtin_egm96()->info();
        info.solid_tide_k20 = .3;
        const GravityModel custom(info, {});
        const auto delta = solid_tide_corrections(custom.info(), {SolidTideMode::permanent}, 2, 0);
        if (delta.size() != 1 || delta[0].c != iers2003_permanent_tide_c20(.3) ||
            std::abs(delta[0].c - (-4.17423864e-9)) > 1e-24)
            return 38;
    }
    {
        orbitforge::core::HpopOptions wide_steps;
        wide_steps.min_step_seconds = wide_steps.max_step_seconds = wide_steps.fixed_step_seconds =
            orbitforge::core::hpop_max_step_seconds;
        orbitforge::core::validate_hpop_options(wide_steps);
        if (wide_steps.max_step_seconds != 86400)
            return 42;
    }
    orbitforge::core::HpopOptions mass_options;
    mass_options.satellite_mass_kg = 1250;
    mass_options.srp.area_mass_ratio_m2_per_kg = .03;
    if (mass_options.drag_area_m2() != 25 || mass_options.srp_area_m2() != 37.5)
        return 36;
    mass_options.satellite_mass_kg = 0;
    try {
        orbitforge::core::validate_hpop_options(mass_options);
        return 37;
    } catch (const std::invalid_argument &) {
    }
    orbitforge::core::DragOptions drag;
    drag.enabled = true;
    drag.atmosphere = orbitforge::core::AtmosphereModel::nrlmsise00;
    orbitforge::core::AtmosphereInput atmosphere;
    atmosphere.geodetic.height_m = 400000;
    const auto density = orbitforge::core::evaluate_atmosphere(drag.atmosphere, atmosphere);
    const auto force = orbitforge::core::evaluate_drag({6778137, 0, 0}, {0, 7500, 0},
                                                       {149597870700, 0, 0}, atmosphere.utc, drag);
    if (!std::isfinite(density.density_kg_m3) || density.density_kg_m3 <= 0 ||
        force.acceleration_itrf_mps2[1] >= 0 || orbitforge::core::kp_to_ap(3) != 15)
        return 29;
    const auto drag_partials = orbitforge::core::drag_differential(
        {6778137, 0, 0}, {0, 7500, 0}, {149597870700, 0, 0}, atmosphere.utc, drag);
    if (drag_partials.force.acceleration_mps2 != force.acceleration_itrf_mps2 ||
        drag_partials.drag_parameter_jacobian_pa[1] >= 0 ||
        drag_partials.force.velocity_jacobian_per_s[1][1] >= 0 ||
        !std::isfinite(drag_partials.density_gradient_kg_m4[0]))
        return 34;
    atmosphere.daily_ap = 15;
    if (orbitforge::core::evaluate_atmosphere(drag.atmosphere, atmosphere).density_kg_m3 !=
        density.density_kg_m3)
        return 31;
    atmosphere.daily_ap.reset();
    atmosphere.ap_history = std::array{40., 400., 100., 50., 20., 7.125, 3.75};
    const auto historical = orbitforge::core::evaluate_atmosphere(drag.atmosphere, atmosphere);
    if (!std::isfinite(historical.density_kg_m3) || historical.density_kg_m3 <= 0 ||
        historical.density_kg_m3 == density.density_kg_m3)
        return 32;
    orbitforge::core::SpaceWeatherDay weather_day;
    weather_day.f107_observed = 150;
    const orbitforge::core::SpaceWeather weather{{}, {weather_day}};
    if (weather.days().size() != 1 || *weather.days()[0].f107_observed != 150 ||
        orbitforge::core::ap_to_kp(15) != 3)
        return 33;
    namespace app = orbitforge::application;
    app::ProjectDocument document{app::Project{"SDK objects", orbitforge::core::Epoch{0},
                                               orbitforge::core::Epoch{60'000'000'000}}};
    auto edit = document.edit();
    auto objects = edit.objects();
    const auto satellite_ref = objects.create(app::objects::Satellite{"Observer", {}, {}});
    const auto sensor_ref = objects.create(app::objects::Sensor{"Camera", satellite_ref, 0.2, {}});
    edit.commit();
    const auto fixed = document.snapshot();
    document.objects().update(sensor_ref, app::objects::SensorUpdate{"Narrow", 0.1, {}});
    if (fixed.project->sensors[0].name() != "Camera" || document.snapshot().revision != 2 ||
        document.objects().children(satellite_ref).size() != 1)
        return 25;
    const auto observation =
        orbitforge::core::observe_ground_target({}, {orbitforge::core::Epoch{0},
                                                     orbitforge::core::OriginId::earth,
                                                     orbitforge::core::FrameId::itrf,
                                                     {6379137, 0, 0},
                                                     {2, 0, 0}});
    if (!observation.line_of_sight || observation.range_m != 1000 ||
        observation.range_rate_mps != 2)
        return 24;
    const orbitforge::application::Project project("SDK project", orbitforge::core::Epoch{123},
                                                   orbitforge::core::Epoch{10'000'000'123});
    const auto restored =
        orbitforge::application::decode_project(orbitforge::application::encode_project(project));
    auto report_project = project;
    report_project.ground_stations.push_back({"station", "SDK station", {0, 0, 0}});
    orbitforge::application::ReportDefinition report;
    report.id = "sdk-report";
    report.query.scope = orbitforge::application::ReportScope::all_compatible;
    report.name = "SDK report";
    report.query.frame = orbitforge::core::FrameId::itrf;
    report.format.time_scale = orbitforge::application::ReportTimeScale::tai;
    const auto table = orbitforge::runtime::generate_report({report_project, {}, {}, {}}, report);
    if (table.table.rows().size() != 2 || std::get<double>(table.table.rows()[0][4]) != 6378137)
        return 18;
    if (restored.first != project.first || restored.last != project.last ||
        !orbitforge::runtime::make_project_scenario(restored).objects().empty() ||
        restored.data_lock() != project.data_lock())
        return 14;
    const orbitforge::application::Scenario empty("SDK mission", orbitforge::core::Epoch{0},
                                                  orbitforge::core::Epoch{10'000'000'000});
    const auto populated = empty.with_object(orbitforge::runtime::make_circular_demo_session());
    const auto removed = populated.without_object(populated.objects()[0].provenance().object_id);
    if (!removed.objects().empty() || removed.revision() != populated.revision() + 1)
        return 15;
    auto named = project;
    orbitforge::application::ProjectSatellite satellite;
    satellite.orbit.first = project.first;
    satellite.orbit.last = project.last;
    named.satellites.push_back(satellite);
    named = named.renamed_satellite("satellite-1", "Observer")
                .with_satellite_copy("satellite-1", "satellite-2", "Observer copy")
                .without_satellite("satellite-1");
    if (named.satellites.size() != 1 || named.satellites[0].name() != "Observer copy")
        return 16;
    if (!empty.objects().empty() || populated.objects().size() != 1 ||
        populated.sample(std::array{orbitforge::core::Epoch{0}}).size() != 1)
        return 12;
    const auto result = orbitforge::runtime::make_circular_demo_session().sample(
        orbitforge::runtime::demo_epochs());
    if (result.states().size() != 6 || result.states().front().position_m[0] != 7e6) {
        return 1;
    }
    if (orbitforge::runtime::time_frames_available() !=
        static_cast<bool>(ORBITFORGE_EXPECT_TIME_FRAMES)) {
        return 2;
    }
    if (orbitforge::runtime::time_frames_available()) {
        if (argc < 2) {
            return 4;
        }
        const auto time = orbitforge::runtime::make_builtin_time_provider();
        const orbitforge::core::Tle tle{
            "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753",
            "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667"};
        const auto tle_epoch = time->from_utc(orbitforge::core::parse_tle(tle).epoch_utc);
        const auto state = orbitforge::runtime::make_sgp4_session(tle, *time, "sdk-sgp4")
                               .sample(std::array{tle_epoch})
                               .states()
                               .front();
        if (state.frame != orbitforge::core::FrameId::teme ||
            std::abs(state.position_m[0] - 7022465.29266) > .001)
            return 20;
        const auto epoch = time->from_utc({2016, 12, 31, 23, 59, 60, 123456789});
        const auto utc = time->to_utc(epoch);
        const auto frames = orbitforge::runtime::make_frame_provider();
        const auto eop = orbitforge::runtime::load_eop_snapshot(argv[1]);
        if (orbitforge::runtime::two_body_available()) {
            using namespace orbitforge;
            using namespace std::chrono_literals;
            app::ProjectDocument access_project{app::Project{
                "SDK access", epoch, core::Epoch{epoch.tai_nanoseconds() + 10'000'000'003}}};
            app::OrbitInput input;
            input.initial.epoch = epoch;
            const auto satellite =
                access_project.objects().create(app::objects::Satellite{"Orbiter", input, {}});
            const auto data = runtime::DataSnapshot::load(argv[1]);
            const auto r = runtime::prepare(access_project, data)
                               .state(satellite, epoch, core::FrameId::itrf)
                               .position_m;
            const auto station = access_project.objects().create(app::objects::GroundStation{
                "Station", {std::atan2(r[1], r[0]), std::atan2(r[2], std::hypot(r[0], r[1])), 0}});
            const auto sensor = access_project.objects().create(
                app::objects::Sensor{"Tracking", satellite, .1, {station}});
            const auto analysis = runtime::prepare(access_project, data);
            const auto access = analysis.access(station, sensor, {.search_step = 1s});
            if (access.windows().size() != 1 ||
                access.windows()[0].duration() != 10'000'000'003ns ||
                access.provenance().target_id != sensor.id())
                return 29;
            runtime::export_access_csv(access, "sdk-access.csv", true);
            const auto aer = analysis.aer(station, sensor, 1s);
            app::ReportDefinition definition;
            definition.id = "sdk-aer";
            definition.name = "AER";
            definition.query.kind = app::ReportKind::aer;
            definition.query.object_ids = {station.id(), sensor.id()};
            definition.query.step_nanoseconds = 1'000'000'000;
            const auto report = analysis.report(definition);
            if (aer.samples().size() != 12 || report.table.rows().size() != 12 ||
                aer.samples().back().epoch != access_project.snapshot().project->last ||
                report.input_revision != analysis.snapshot().revision ||
                !aer.samples().front().off_boresight_rad ||
                std::get<double>(report.table.rows()[0][5]) != aer.samples()[0].observation.range_m)
                return 30;
            runtime::export_aer_csv(aer, "sdk-aer.csv", true);
            runtime::export_report_csv(report, "sdk-report.csv", true);
            app::ReportTemplate format;
            format.time_scale = app::ReportTimeScale::tai;
            format.columns = {{"aer.range", "km", "Range"}, {"time.epoch", "TAI", ""}};
            const auto formatted = analysis.report(definition.query, format);
            const auto restored = formatted.with_template(definition.format);
            if (formatted.table.columns().size() != 2 || formatted.column_unit(0) != "km" ||
                restored.table.rows() != report.table.rows() ||
                formatted.input_revision != report.input_revision)
                return 31;
        }
        if (orbitforge::runtime::data_catalog_available()) {
            orbitforge::runtime::DataCatalog catalog("sdk-data-catalog");
            const auto entry = catalog.install_eop(argv[1]);
            const auto lock = orbitforge::application::eop_data_lock(eop->data_info());
            if (entry.lock_sha256 != lock.sha256() ||
                catalog.resolve_eop(lock)->data_info().id != eop->data_info().id)
                return 17;
            catalog.activate_eop(entry.lock_sha256);
            catalog.set_preferred_eop(orbitforge::runtime::EopProduct::c04);
            catalog.set_extrapolation_days(orbitforge::runtime::EopProduct::c04, 3);
            const auto selected = catalog.snapshot(entry.first, entry.last);
            app::ProjectDocument analysis_project{
                app::Project{"SDK catalog", entry.first, entry.last}};
            if (selected.data_lock() != lock ||
                selected.eop()->data_info().extrapolation_days != 0 ||
                orbitforge::runtime::prepare(analysis_project, catalog).data().data_lock() != lock)
                return 27;
            const auto prepared = orbitforge::runtime::prepare(analysis_project, catalog);
            orbitforge::runtime::save_project(analysis_project, "sdk-analysis.orbitforge", prepared,
                                              true);
            const orbitforge::runtime::ProjectOpenPlan opening{"sdk-analysis.orbitforge", catalog};
            const auto reopened = opening.open();
            if (reopened.dirty() || reopened.snapshot().project->data_lock() != lock ||
                reopened.snapshot().project->analysis_status() != app::AnalysisStatus::current)
                return 28;
        }
        const auto transform = frames->gcrf_to_itrf_state(epoch, eop->at(epoch));
        if (utc.second != 60 || utc.nanosecond != 123456789 ||
            transform.rotation.to != orbitforge::core::FrameId::itrf) {
            return 3;
        }
        const orbitforge::core::CartesianState ground{epoch,
                                                      orbitforge::core::OriginId::earth,
                                                      orbitforge::core::FrameId::itrf,
                                                      {6378137, 0, 0},
                                                      {0, 0, 0}};
        const auto inertial = transform.inverse(ground);
        const auto back = transform.apply(inertial);
        if (std::abs(std::hypot(inertial.velocity_mps[0], inertial.velocity_mps[1],
                                inertial.velocity_mps[2]) -
                     465.1011) > 0.001) {
            return 5;
        }
        for (std::size_t k = 0; k < 3; ++k) {
            if (std::abs(back.position_m[k] - ground.position_m[k]) > 5e-8 ||
                std::abs(back.velocity_mps[k]) > 5e-10) {
                return 6;
            }
        }
    }
    if (orbitforge::runtime::ephemeris_available() != static_cast<bool>(ORBITFORGE_EXPECT_SPICE)) {
        return 7;
    }
    if (orbitforge::runtime::two_body_available() != static_cast<bool>(ORBITFORGE_EXPECT_SPICE))
        return 10;
    if (orbitforge::runtime::two_body_available()) {
        using namespace orbitforge;
        const auto analysis = runtime::prepare(document);
        const auto trajectory =
            analysis.states(satellite_ref, std::chrono::seconds{40}, core::FrameId::gcrf);
        document.objects().remove(satellite_ref);
        if (trajectory.states().size() != 3 ||
            trajectory.states().back().epoch != document.snapshot().project->last ||
            analysis.state(satellite_ref, core::Epoch{0}, core::FrameId::gcrf).position_m[0] != 7e6)
            return 26;
        const auto j4 =
            runtime::make_j4_session({core::Epoch{0}, 7e6, .01, .5, .1, .2, .3}, "sdk-j4")
                .sample(std::array{core::Epoch{0}, core::Epoch{60'000'000'000}});
        if (j4.states().size() != 2 || !std::isfinite(j4.states()[1].position_m[0]))
            return 21;
        const core::KeplerianElements elements{core::Epoch{1}, 7e6, 0.1, 0.3, 0.2, 0.4, 0.5};
        const auto converted =
            runtime::to_cartesian(elements, core::GravitationalParameter{3.986004418e14});
        const auto recovered =
            runtime::to_keplerian(converted, core::GravitationalParameter{3.986004418e14});
        if (recovered.epoch != elements.epoch || std::abs(recovered.semi_major_axis_m - 7e6) > 1e-6)
            return 13;
        const core::CartesianState initial{
            core::Epoch{0}, core::OriginId::earth, core::FrameId::gcrf, {3, 0, 0}, {0, 2, 0}};
        const auto states = runtime::make_two_body_session(initial, core::GravitationalParameter{6},
                                                           "sdk-two-body", 3)
                                .sample(std::array{core::Epoch{4'000'000'000}});
        if (std::abs(states.states().front().position_m[1] - 6) > 1e-8 ||
            states.provenance().input_revision != 3)
            return 11;
    }
    if (orbitforge::runtime::ephemeris_available()) {
        if (argc != 4) {
            return 8;
        }
        if (orbitforge::runtime::data_catalog_available()) {
            using namespace orbitforge;
            const auto samples = std::filesystem::path(argv[2]).parent_path().parent_path();
            runtime::DataCatalog catalog(std::filesystem::path(argv[3]) / "centres-catalog");
            (void)catalog.install_ephemeris_snapshot(samples /
                                                     "de440s_planets_2016_leap/manifest.json");
            const auto installed =
                catalog.install_planetary_centres(samples / "mar099s_2016_leap/mar099s_subset.bsp");
            const auto ephemeris = catalog.load_ephemeris("de440s");
            const std::array bodies{core::ThirdBody::mars};
            ephemeris->validate_eclipsing_body_interval(installed.info.first, installed.info.last,
                                                        bodies);
            const auto point =
                ephemeris->eclipsing_body_position(core::ThirdBody::mars, installed.info.first);
            if (!std::isfinite(point[0]))
                return 48;
        }
        auto provider = orbitforge::runtime::load_spice_snapshot(argv[2], argv[3]);
        const auto epoch = orbitforge::core::Epoch{
            provider->data_info().first_epoch.tai_nanoseconds() + 600000000000};
        provider->validate_spacecraft_apparent_sun_interval(epoch, epoch);
        const auto apparent = provider->apparent_sun_from_spacecraft(epoch, {}, {});
        const auto centre_sun = provider->apparent_sun_from_earth(epoch);
        for (std::size_t i = 0; i < 3; ++i)
            if (std::abs(apparent[i] - centre_sun[i]) > 1e-3)
                return 34;
        const auto moon = provider->geometric_state(301, 399, epoch);
        const auto range = std::hypot(moon.position_m[0], moon.position_m[1], moon.position_m[2]);
        if (moon.frame != orbitforge::core::FrameId::spice_j2000 ||
            moon.velocity_time_scale != orbitforge::core::TimeScale::tdb || range < 3.5e8 ||
            range > 4.1e8 ||
            std::abs(provider->from_tdb(provider->to_tdb(epoch)).seconds_since(epoch)) > 1e-6) {
            return 9;
        }
        auto ephemeris = orbitforge::runtime::make_third_body_ephemeris(std::move(provider));
        orbitforge::core::HpopOptions options;
        options.gravity_degree = options.gravity_order = 0;
        options.third_body(orbitforge::core::ThirdBody::sun).enabled =
            options.third_body(orbitforge::core::ThirdBody::moon).enabled = true;
        options.srp.enabled = true;
        options.srp.cr = 1.2;
        options.srp.sun_position_method = orbitforge::core::SrpSunPositionMethod::apparent;
        const auto full_light = orbitforge::core::evaluate_srp(
            {7e6, 0, 0}, {149597870700, 0, 0}, options.srp, std::array{0., 384400000., 0.});
        if (full_light.lighting_ratio != 1 || full_light.acceleration_mps2[0] >= 0)
            return 28;
        const orbitforge::core::CartesianState initial{epoch,
                                                       orbitforge::core::OriginId::earth,
                                                       orbitforge::core::FrameId::gcrf,
                                                       {7e6, 0, 0},
                                                       {0, 6200, 5300}};
        const auto numerical =
            orbitforge::core::make_hpop(initial, options, {nullptr, nullptr, nullptr, ephemeris});
        const auto state =
            numerical->state_at(orbitforge::core::Epoch{epoch.tai_nanoseconds() + 60'000'000'000});
        if (!std::isfinite(state.position_m[0]) || state.position_m == initial.position_m ||
            ephemeris->data_info().sun_mu != orbitforge::runtime::de440_sun_mu)
            return 17;
    }
    {
        const orbitforge::core::OceanTideModel ocean({"sdk-ocean", {}, {}, 2, 0},
                                                     {{55555, {{2, 0, 1e-10, 0, 2e-10, 0}}}});
        const auto delta =
            ocean.corrections(orbitforge::core::builtin_egm96()->info(),
                              {orbitforge::core::Epoch{0}, 0, 0, 0, 0, 0, "test"}, 2, 0);
        if (delta.size() != 1 || std::abs(delta[0].c - 3e-10) > 1e-25)
            return 36;
        orbitforge::core::validate_ocean_tide_options({false, "missing", 100, 12});
    }
    std::cout << "Installed SDK consumer passed\n";
} catch (const std::exception &error) {
    std::cerr << "Installed SDK consumer failed: " << error.what() << '\n';
    return 30;
}
