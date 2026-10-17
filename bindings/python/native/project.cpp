#include <orbitforge/runtime/project.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_data_lock(py::module_ &module) {
    py::class_<application::DataReference>(module, "DataReference")
        .def(py::init<std::string, std::string, std::string, std::string, std::string>(),
             py::arg("role"), py::arg("snapshot_id"), py::arg("content_sha256"),
             py::arg("interpretation_version"), py::arg("configuration_sha256"))
        .def_readonly("role", &application::DataReference::role)
        .def_readonly("snapshot_id", &application::DataReference::snapshot_id)
        .def_readonly("content_sha256", &application::DataReference::content_sha256)
        .def_readonly("interpretation_version", &application::DataReference::interpretation_version)
        .def_readonly("configuration_sha256", &application::DataReference::configuration_sha256);
    py::class_<application::DataLock>(module, "DataLock")
        .def(py::init<std::vector<application::DataReference>>(),
             py::arg("references") = std::vector<application::DataReference>{})
        .def_property_readonly("sha256", &application::DataLock::sha256)
        .def_property_readonly("references",
                               [](const application::DataLock &lock) {
                                   return std::vector<application::DataReference>(
                                       lock.references().begin(), lock.references().end());
                               })
        .def("canonical_bytes", [](const application::DataLock &lock) {
            const auto bytes = lock.canonical_bytes();
            return py::bytes(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        });
}
void bind_project(py::module_ &module) {
    py::register_exception<application::ProjectError>(module, "ProjectError", PyExc_ValueError);
    py::class_<core::GroundStationLocation>(module, "GroundStationLocation")
        .def(py::init<double, double, double>(), py::arg("longitude_rad") = 0,
             py::arg("latitude_rad") = 0, py::arg("height_m") = 0)
        .def_readwrite("longitude_rad", &core::GroundStationLocation::longitude_rad)
        .def_readwrite("latitude_rad", &core::GroundStationLocation::latitude_rad)
        .def_readwrite("height_m", &core::GroundStationLocation::height_m);
    py::class_<application::ProjectGroundStation>(module, "ProjectGroundStation")
        .def(py::init<>())
        .def_readwrite("object_id", &application::ProjectGroundStation::object_id)
        .def_readwrite("display_name", &application::ProjectGroundStation::display_name)
        .def_readwrite("location", &application::ProjectGroundStation::location)
        .def_readwrite("minimum_elevation_rad",
                       &application::ProjectGroundStation::minimum_elevation_rad)
        .def_property_readonly("name", &application::ProjectGroundStation::name);
    py::enum_<application::SensorPointing>(module, "SensorPointing")
        .value("NADIR", application::SensorPointing::nadir)
        .value("TARGETED", application::SensorPointing::targeted);
    py::class_<application::ProjectSensor>(module, "ProjectSensor")
        .def(py::init<>())
        .def_readwrite("object_id", &application::ProjectSensor::object_id)
        .def_readwrite("display_name", &application::ProjectSensor::display_name)
        .def_readwrite("parent_satellite_id", &application::ProjectSensor::parent_satellite_id)
        .def_readwrite("half_angle_rad", &application::ProjectSensor::half_angle_rad)
        .def_readwrite("pointing", &application::ProjectSensor::pointing)
        .def_readwrite("target_station_id", &application::ProjectSensor::target_station_id)
        .def_property_readonly("name", &application::ProjectSensor::name);
    py::enum_<application::OrbitPropagator>(module, "OrbitPropagator")
        .value("TWO_BODY", application::OrbitPropagator::two_body)
        .value("J2_SECULAR", application::OrbitPropagator::j2_secular)
        .value("J4_SECULAR", application::OrbitPropagator::j4_secular)
        .value("SGP4", application::OrbitPropagator::sgp4)
        .value("HPOP", application::OrbitPropagator::hpop)
        .value("MISSION", application::OrbitPropagator::mission);
    py::enum_<core::MissionSegmentKind>(module, "MissionSegmentKind")
        .value("COAST", core::MissionSegmentKind::coast)
        .value("IMPULSE", core::MissionSegmentKind::impulse);
    py::enum_<core::MissionCoastModel>(module, "MissionCoastModel")
        .value("TWO_BODY", core::MissionCoastModel::two_body)
        .value("HPOP", core::MissionCoastModel::hpop);
    py::enum_<core::ManeuverFrame>(module, "ManeuverFrame")
        .value("GCRF", core::ManeuverFrame::gcrf)
        .value("VNC", core::ManeuverFrame::vnc)
        .value("LVLH", core::ManeuverFrame::lvlh);
    py::class_<core::MissionSegment>(module, "MissionSegment")
        .def(py::init<>())
        .def_readwrite("kind", &core::MissionSegment::kind)
        .def_readwrite("name", &core::MissionSegment::name)
        .def_readwrite("duration_nanoseconds", &core::MissionSegment::duration_nanoseconds)
        .def_readwrite("delta_v_mps", &core::MissionSegment::delta_v_mps)
        .def_readwrite("frame", &core::MissionSegment::frame);
    py::class_<core::MissionPlan>(module, "MissionPlan")
        .def(py::init<>())
        .def_readwrite("coast_model", &core::MissionPlan::coast_model)
        .def_readwrite("segments", &core::MissionPlan::segments)
        .def(
            "end_epoch",
            [](const core::MissionPlan &plan, core::Epoch initial) {
                return core::validate_mission(initial, plan);
            },
            py::arg("initial"));
    py::enum_<core::HpopStepControl>(module, "HpopStepControl")
        .value("RELATIVE_ERROR", core::HpopStepControl::relative_error)
        .value("FIXED", core::HpopStepControl::fixed);
    py::enum_<core::HpopCorrection>(module, "HpopCorrection")
        .value("FULL", core::HpopCorrection::full)
        .value("PSEUDO", core::HpopCorrection::pseudo);
    py::enum_<core::HpopIntegrator>(module, "HpopIntegrator")
        .value("GAUSS_JACKSON", core::HpopIntegrator::gauss_jackson)
        .value("BULIRSCH_STOER", core::HpopIntegrator::bulirsch_stoer)
        .value("RKV89", core::HpopIntegrator::rkv89)
        .value("RKF78", core::HpopIntegrator::rkf78)
        .value("RK4", core::HpopIntegrator::rk4);
    py::enum_<core::SrpSunPositionMethod>(module, "SrpSunPositionMethod")
        .value("TRUE", core::SrpSunPositionMethod::geometric)
        .value("APPARENT_TO_TRUE_CB", core::SrpSunPositionMethod::apparent_to_true_cb)
        .value("APPARENT", core::SrpSunPositionMethod::apparent);
    py::enum_<core::SrpShadowModel>(module, "SrpShadowModel")
        .value("NONE", core::SrpShadowModel::none)
        .value("CYLINDRICAL", core::SrpShadowModel::cylindrical)
        .value("DUAL_CONE", core::SrpShadowModel::dual_cone);
    py::enum_<core::AtmosphereModel>(module, "AtmosphereModel")
        .value("JACCHIA_ROBERTS", core::AtmosphereModel::jacchia_roberts)
        .value("NRLMSISE00", core::AtmosphereModel::nrlmsise00);
    py::enum_<core::LowAltitudeDensityModel>(module, "LowAltitudeDensityModel")
        .value("NONE", core::LowAltitudeDensityModel::none)
        .value("NRLMSISE00", core::LowAltitudeDensityModel::nrlmsise00);
    py::enum_<core::GeomagneticUpdate>(module, "GeomagneticUpdate")
        .value("DAILY", core::GeomagneticUpdate::daily)
        .value("THREE_HOURLY", core::GeomagneticUpdate::three_hourly)
        .value("THREE_HOURLY_INTERPOLATED", core::GeomagneticUpdate::three_hourly_interpolated)
        .value("THREE_HOURLY_CUBIC_SPLINE", core::GeomagneticUpdate::three_hourly_cubic_spline);
    py::enum_<core::GeomagneticSource>(module, "GeomagneticSource")
        .value("AP", core::GeomagneticSource::ap)
        .value("KP", core::GeomagneticSource::kp);
    py::class_<core::SpaceWeatherSample>(module, "SpaceWeatherSample")
        .def_readonly("f107", &core::SpaceWeatherSample::f107)
        .def_readonly("f107_average", &core::SpaceWeatherSample::f107_average)
        .def_readonly("kp", &core::SpaceWeatherSample::kp)
        .def_readonly("ap", &core::SpaceWeatherSample::ap)
        .def_readonly("ap_history", &core::SpaceWeatherSample::ap_history);
    py::class_<core::DragOptions>(module, "DragOptions")
        .def(py::init<>())
        .def_readwrite("enabled", &core::DragOptions::enabled)
        .def_readwrite("cd", &core::DragOptions::cd)
        .def_readwrite("area_mass_ratio_m2_per_kg", &core::DragOptions::area_mass_ratio_m2_per_kg)
        .def_readwrite("atmosphere", &core::DragOptions::atmosphere)
        .def_readwrite("low_altitude_model", &core::DragOptions::low_altitude_model)
        .def_readwrite("blending_range_m", &core::DragOptions::blending_range_m)
        .def_readwrite("f107", &core::DragOptions::f107)
        .def_readwrite("f107_average", &core::DragOptions::f107_average)
        .def_readwrite("kp", &core::DragOptions::kp)
        .def_readwrite("use_apparent_sun_position", &core::DragOptions::use_apparent_sun_position)
        .def_readwrite("use_approximate_altitude", &core::DragOptions::use_approximate_altitude)
        .def_readwrite("weather_model", &core::DragOptions::weather_model)
        .def_readwrite("geomagnetic_update", &core::DragOptions::geomagnetic_update)
        .def_readwrite("geomagnetic_source", &core::DragOptions::geomagnetic_source)
        .def("validate", &core::validate_drag_options);
    py::class_<core::AtmosphereInput>(module, "AtmosphereInput")
        .def(py::init<>())
        .def_readwrite("utc", &core::AtmosphereInput::utc)
        .def_property(
            "geodetic",
            [](const core::AtmosphereInput &input) {
                return std::array{input.geodetic.longitude_rad, input.geodetic.latitude_rad,
                                  input.geodetic.height_m};
            },
            [](core::AtmosphereInput &input, const std::array<double, 3> &position) {
                input.geodetic = {position[0], position[1], position[2]};
            })
        .def_readwrite("solar_hour_angle_rad", &core::AtmosphereInput::solar_hour_angle_rad)
        .def_readwrite("sun_declination_rad", &core::AtmosphereInput::sun_declination_rad)
        .def_readwrite("f107", &core::AtmosphereInput::f107)
        .def_readwrite("f107_average", &core::AtmosphereInput::f107_average)
        .def_readwrite("kp", &core::AtmosphereInput::kp)
        .def_readwrite("daily_ap", &core::AtmosphereInput::daily_ap)
        .def_readwrite("ap_history", &core::AtmosphereInput::ap_history);
    py::class_<core::AtmosphereEvaluation>(module, "AtmosphereEvaluation")
        .def_readonly("density_kg_m3", &core::AtmosphereEvaluation::density_kg_m3)
        .def_readonly("temperature_k", &core::AtmosphereEvaluation::temperature_k)
        .def_readonly("exospheric_temperature_k",
                      &core::AtmosphereEvaluation::exospheric_temperature_k);
    py::class_<core::DragEvaluation>(module, "DragEvaluation")
        .def_readonly("acceleration_itrf_mps2", &core::DragEvaluation::acceleration_itrf_mps2)
        .def_readonly("density_kg_m3", &core::DragEvaluation::density_kg_m3)
        .def_readonly("density_altitude_m", &core::DragEvaluation::density_altitude_m)
        .def_property_readonly("geodetic", [](const core::DragEvaluation &result) {
            return std::array{result.geodetic.longitude_rad, result.geodetic.latitude_rad,
                              result.geodetic.height_m};
        });
    module.def("evaluate_atmosphere", &core::evaluate_atmosphere, py::arg("model"),
               py::arg("input"));
    module.def("evaluate_drag", &core::evaluate_drag, py::arg("position_itrf_m"),
               py::arg("velocity_itrf_mps"), py::arg("sun_itrf_m"), py::arg("utc"),
               py::arg("options"), py::arg("weather") = std::nullopt,
               py::arg("low_weather") = std::nullopt);
    module.def("ap_to_kp", &core::ap_to_kp, py::arg("ap"));
    module.def("kp_to_ap", &core::kp_to_ap, py::arg("kp"));
    py::class_<core::SrpEclipsingBody>(module, "SrpEclipsingBody")
        .def(py::init<>())
        .def_readwrite("body", &core::SrpEclipsingBody::body)
        .def_readwrite("radius_m", &core::SrpEclipsingBody::radius_m);
    py::class_<core::SrpBodyPosition>(module, "SrpBodyPosition")
        .def(py::init<>())
        .def_readwrite("body", &core::SrpBodyPosition::body)
        .def_readwrite("position_m", &core::SrpBodyPosition::position_m);
    module.def("default_srp_eclipsing_body", &core::default_srp_eclipsing_body, py::arg("body"));
    py::class_<core::SrpOptions>(module, "SrpOptions")
        .def(py::init<>())
        .def_readwrite("enabled", &core::SrpOptions::enabled)
        .def_readwrite("cr", &core::SrpOptions::cr)
        .def_readwrite("area_mass_ratio_m2_per_kg", &core::SrpOptions::area_mass_ratio_m2_per_kg)
        .def_readwrite("shadow_model", &core::SrpOptions::shadow_model)
        .def_readwrite("sun_position_method", &core::SrpOptions::sun_position_method)
        .def_readwrite("earth_shadow", &core::SrpOptions::earth_shadow)
        .def_readwrite("moon_shadow", &core::SrpOptions::moon_shadow)
        .def_readwrite("boundary_mitigation", &core::SrpOptions::boundary_mitigation)
        .def_readwrite("additional_eclipsing_bodies",
                       &core::SrpOptions::additional_eclipsing_bodies)
        .def_readwrite("atmospheric_altitude_m", &core::SrpOptions::atmospheric_altitude_m)
        .def_readwrite("pressure_at_reference_pa", &core::SrpOptions::pressure_at_reference_pa)
        .def_readwrite("reference_distance_m", &core::SrpOptions::reference_distance_m)
        .def_readwrite("sun_radius_m", &core::SrpOptions::sun_radius_m)
        .def_readwrite("earth_radius_m", &core::SrpOptions::earth_radius_m)
        .def_readwrite("moon_radius_m", &core::SrpOptions::moon_radius_m)
        .def("validate", &core::validate_srp_options);
    py::class_<core::SrpEvaluation>(module, "SrpEvaluation")
        .def_readonly("acceleration_mps2", &core::SrpEvaluation::acceleration_mps2)
        .def_readonly("lighting_ratio", &core::SrpEvaluation::lighting_ratio);
    module.def(
        "evaluate_srp",
        [](const std::array<double, 3> &satellite, const std::array<double, 3> &sun,
           const core::SrpOptions &options, const std::optional<std::array<double, 3>> &moon,
           const std::vector<core::SrpBodyPosition> &additional) {
            return core::evaluate_srp(satellite, sun, options, moon, additional);
        },
        py::arg("satellite_m"), py::arg("sun_m"), py::arg("options"),
        py::arg("moon_m") = std::nullopt,
        py::arg("additional_positions") = std::vector<core::SrpBodyPosition>{});
    py::enum_<core::ThirdBody>(module, "ThirdBody")
        .value("SUN", core::ThirdBody::sun)
        .value("MOON", core::ThirdBody::moon)
        .value("JUPITER", core::ThirdBody::jupiter)
        .value("VENUS", core::ThirdBody::venus)
        .value("SATURN", core::ThirdBody::saturn)
        .value("MARS", core::ThirdBody::mars)
        .value("MERCURY", core::ThirdBody::mercury)
        .value("URANUS", core::ThirdBody::uranus)
        .value("NEPTUNE", core::ThirdBody::neptune)
        .value("PLUTO", core::ThirdBody::pluto);
    py::enum_<core::ThirdBodyGravitySource>(module, "ThirdBodyGravitySource")
        .value("CB_FILE", core::ThirdBodyGravitySource::cb_file)
        .value("JPL_DE", core::ThirdBodyGravitySource::jpl_de)
        .value("USER_SPECIFIED", core::ThirdBodyGravitySource::user_specified);
    py::class_<core::CbGravity>(module, "CbGravity")
        .def_readonly("mu", &core::CbGravity::mu)
        .def_readonly("name", &core::CbGravity::name)
        .def_readonly("content_sha256", &core::CbGravity::content_sha256)
        .def_readonly("body", &core::CbGravity::body)
        .def("__eq__", [](const core::CbGravity &a, const core::CbGravity &b) { return a == b; });
    py::class_<core::ThirdBodyGravity>(module, "ThirdBodyGravity")
        .def(py::init<>())
        .def_readwrite("enabled", &core::ThirdBodyGravity::enabled)
        .def_readwrite("source", &core::ThirdBodyGravity::source)
        .def_readwrite("user_mu", &core::ThirdBodyGravity::user_mu)
        .def_readwrite("cb", &core::ThirdBodyGravity::cb);
    py::class_<core::RelativityEvaluation>(module, "RelativityEvaluation")
        .def_readonly("schwarzschild_mps2", &core::RelativityEvaluation::schwarzschild_mps2)
        .def_readonly("lense_thirring_mps2", &core::RelativityEvaluation::lense_thirring_mps2)
        .def_readonly("de_sitter_mps2", &core::RelativityEvaluation::de_sitter_mps2)
        .def_readonly("acceleration_mps2", &core::RelativityEvaluation::acceleration_mps2);
    module.def("evaluate_relativity", &core::evaluate_relativity, py::arg("position_m"),
               py::arg("velocity_mps"), py::arg("earth_mu"), py::arg("sun_position_m"),
               py::arg("sun_velocity_mps"), py::arg("sun_mu"), py::arg("earth_pole"));
    py::enum_<core::SolidTideMode>(module, "SolidTideMode")
        .value("NONE", core::SolidTideMode::none)
        .value("PERMANENT", core::SolidTideMode::permanent)
        .value("FULL", core::SolidTideMode::full);
    py::class_<core::SolidTideOptions>(module, "SolidTideOptions")
        .def(py::init<>())
        .def_readwrite("mode", &core::SolidTideOptions::mode)
        .def_readwrite("truncate_to_gravity", &core::SolidTideOptions::truncate_to_gravity)
        .def_readwrite("include_time_dependent_terms",
                       &core::SolidTideOptions::include_time_dependent_terms)
        .def("validate", &core::validate_solid_tide_options);
    py::class_<core::OceanTideOptions>(module, "OceanTideOptions")
        .def(py::init<>())
        .def_readwrite("enabled", &core::OceanTideOptions::enabled)
        .def_readwrite("model", &core::OceanTideOptions::model)
        .def_readwrite("degree", &core::OceanTideOptions::degree)
        .def_readwrite("order", &core::OceanTideOptions::order)
        .def("validate", &core::validate_ocean_tide_options);
    py::class_<core::HpopOptions>(module, "HpopOptions")
        .def(py::init<>())
        .def_readwrite("gravity_model", &core::HpopOptions::gravity_model)
        .def_readwrite("gravity_mu", &core::HpopOptions::gravity_mu)
        .def_readwrite("include_secular_variations", &core::HpopOptions::include_secular_variations)
        .def_readwrite("third_bodies", &core::HpopOptions::third_bodies)
        .def_readwrite("satellite_mass_kg", &core::HpopOptions::satellite_mass_kg)
        .def_property_readonly("drag_area_m2", &core::HpopOptions::drag_area_m2)
        .def_property_readonly("srp_area_m2", &core::HpopOptions::srp_area_m2)
        .def_readwrite("srp", &core::HpopOptions::srp)
        .def_readwrite("drag", &core::HpopOptions::drag)
        .def_readwrite("solid_tides", &core::HpopOptions::solid_tides)
        .def_readwrite("ocean_tides", &core::HpopOptions::ocean_tides)
        .def_readwrite("include_relativistic_accelerations",
                       &core::HpopOptions::include_relativistic_accelerations)
        .def_readwrite("ephemeris_model", &core::HpopOptions::ephemeris_model)
        .def_readwrite("gravity_degree", &core::HpopOptions::gravity_degree)
        .def_readwrite("gravity_order", &core::HpopOptions::gravity_order)
        .def_readwrite("integrator", &core::HpopOptions::integrator)
        .def_readwrite("vop", &core::HpopOptions::vop)
        .def("uses_vop", &core::HpopOptions::uses_vop)
        .def_readwrite("correction", &core::HpopOptions::correction)
        .def_readwrite("step_control", &core::HpopOptions::step_control)
        .def("uses_fixed_step", &core::HpopOptions::uses_fixed_step)
        .def_readwrite("relative_tolerance", &core::HpopOptions::relative_tolerance)
        .def_readwrite("position_tolerance_m", &core::HpopOptions::position_tolerance_m)
        .def_readwrite("velocity_tolerance_mps", &core::HpopOptions::velocity_tolerance_mps)
        .def_readwrite("min_step_seconds", &core::HpopOptions::min_step_seconds)
        .def_readwrite("max_step_seconds", &core::HpopOptions::max_step_seconds)
        .def_readwrite("fixed_step_seconds", &core::HpopOptions::fixed_step_seconds)
        .def_readwrite("time_regularization", &core::HpopOptions::time_regularization)
        .def_readwrite("regularization_exponent", &core::HpopOptions::regularization_exponent)
        .def_readwrite("steps_per_orbit", &core::HpopOptions::steps_per_orbit)
        .def_readwrite("interpolation", &core::HpopOptions::interpolation)
        .def_readwrite("report_ephemeris_on_fixed_step",
                       &core::HpopOptions::report_ephemeris_on_fixed_step)
        .def_readwrite("minimum_altitude_m", &core::HpopOptions::minimum_altitude_m)
        .def("validate", &core::validate_hpop_options);
    py::class_<application::OrbitInput>(module, "OrbitInput")
        .def(py::init<>())
        .def_readwrite("initial", &application::OrbitInput::initial)
        .def_readwrite("propagator", &application::OrbitInput::propagator)
        .def_readwrite("input_frame", &application::OrbitInput::input_frame)
        .def_readwrite("tle", &application::OrbitInput::tle)
        .def_readwrite("hpop", &application::OrbitInput::hpop)
        .def_readwrite("mission", &application::OrbitInput::mission)
        .def_readwrite("object_id", &application::OrbitInput::object_id)
        .def_readwrite("first", &application::OrbitInput::first)
        .def_readwrite("last", &application::OrbitInput::last)
        .def_readwrite("step_seconds", &application::OrbitInput::step_seconds);
    py::enum_<application::LineStyle>(module, "LineStyle")
        .value("SOLID", application::LineStyle::solid)
        .value("DASHED", application::LineStyle::dashed)
        .value("DOTTED", application::LineStyle::dotted)
        .value("DASH_DOT", application::LineStyle::dash_dot);
    py::class_<application::OrbitDisplay>(module, "OrbitDisplay")
        .def(py::init<>())
        .def_readwrite("extent", &application::OrbitDisplay::extent)
        .def_readwrite("past_minutes", &application::OrbitDisplay::past_minutes)
        .def_readwrite("future_minutes", &application::OrbitDisplay::future_minutes)
        .def_readwrite("track_2d", &application::OrbitDisplay::track_2d)
        .def_readwrite("labels_2d", &application::OrbitDisplay::labels_2d)
        .def_readwrite("track_3d", &application::OrbitDisplay::track_3d)
        .def_readwrite("line_color", &application::OrbitDisplay::line_color)
        .def_readwrite("line_width", &application::OrbitDisplay::line_width)
        .def_readwrite("line_style", &application::OrbitDisplay::line_style);
    py::class_<application::ProjectSatellite>(module, "ProjectSatellite")
        .def(py::init<>())
        .def_readwrite("orbit", &application::ProjectSatellite::orbit)
        .def_readwrite("display_name", &application::ProjectSatellite::display_name)
        .def_property_readonly("name", &application::ProjectSatellite::name)
        .def_readwrite("display", &application::ProjectSatellite::display)
        .def_readwrite("model_kind", &application::ProjectSatellite::model_kind)
        .def_readwrite("model_diameter_m", &application::ProjectSatellite::model_diameter_m)
        .def_readwrite("model_path", &application::ProjectSatellite::model_path);
    py::enum_<application::AnalysisStatus>(module, "AnalysisStatus")
        .value("UNRECORDED", application::AnalysisStatus::unrecorded)
        .value("CURRENT", application::AnalysisStatus::current)
        .value("STALE", application::AnalysisStatus::stale);
    py::class_<application::AnalysisRecord>(module, "AnalysisRecord")
        .def_readonly("input_sha256", &application::AnalysisRecord::input_sha256)
        .def_readonly("data_sha256", &application::AnalysisRecord::data_sha256);
    py::class_<application::Project>(module, "Project")
        .def(py::init<std::string, core::Epoch, core::Epoch>(), py::arg("name"), py::arg("first"),
             py::arg("last"))
        .def_readwrite("name", &application::Project::name)
        .def_readwrite("first", &application::Project::first)
        .def_readwrite("last", &application::Project::last)
        .def_readwrite("current", &application::Project::current)
        .def_readwrite("satellites", &application::Project::satellites)
        .def_readwrite("ground_stations", &application::Project::ground_stations)
        .def_readwrite("sensors", &application::Project::sensors)
        .def_readwrite("reports", &application::Project::reports)
        .def_readwrite("earth_orientation", &application::Project::earth_orientation)
        .def_readwrite("time_data", &application::Project::time_data)
        .def_readwrite("force_data", &application::Project::force_data)
        .def_readwrite("analysis_record", &application::Project::analysis_record)
        .def_property_readonly("analysis_status", &application::Project::analysis_status)
        .def_property_readonly("data_lock", &application::Project::data_lock)
        .def("with_settings", &application::Project::with_settings, py::arg("name"),
             py::arg("first"), py::arg("last"))
        .def("renamed_satellite", &application::Project::renamed_satellite, py::arg("object_id"),
             py::arg("name"))
        .def("without_satellite", &application::Project::without_satellite, py::arg("object_id"))
        .def("with_satellite_copy", &application::Project::with_satellite_copy,
             py::arg("object_id"), py::arg("new_id"), py::arg("name"))
        .def("renamed_ground_station", &application::Project::renamed_ground_station,
             py::arg("object_id"), py::arg("name"))
        .def("without_ground_station", &application::Project::without_ground_station,
             py::arg("object_id"))
        .def("with_ground_station_copy", &application::Project::with_ground_station_copy,
             py::arg("object_id"), py::arg("new_id"), py::arg("name"))
        .def(
            "add_ground_station",
            [](application::Project &project, application::ProjectGroundStation station) {
                auto candidate = project;
                candidate.ground_stations.push_back(std::move(station));
                application::validate_project(candidate);
                project = std::move(candidate);
            },
            py::arg("station"))
        .def(
            "add_sensor",
            [](application::Project &project, application::ProjectSensor sensor) {
                auto candidate = project;
                candidate.sensors.push_back(std::move(sensor));
                application::validate_project(candidate);
                project = std::move(candidate);
            },
            py::arg("sensor"))
        .def(
            "add_satellite",
            [](application::Project &project, application::OrbitInput input,
               application::OrbitDisplay display) {
                input.first = project.first;
                input.last = project.last;
                application::ProjectSatellite satellite;
                satellite.orbit = std::move(input);
                satellite.display = display;
                auto candidate = project;
                candidate.satellites.push_back(std::move(satellite));
                application::validate_project(candidate);
                project = std::move(candidate);
            },
            py::arg("input"), py::arg("display") = application::OrbitDisplay{})
        .def(
            "to_scenario",
            [](const application::Project &project, const core::ITimeProvider *time) {
                auto snapshot = project;
                py::gil_scoped_release release;
                return runtime::make_project_scenario(snapshot, time);
            },
            py::arg("time") = nullptr)
        .def(
            "save",
            [](const application::Project &project, const std::filesystem::path &path,
               bool overwrite) {
                auto snapshot = project;
                py::gil_scoped_release release;
                application::save_project(path, snapshot, overwrite);
            },
            py::arg("path"), py::arg("overwrite") = false);
    module.def("load_project", &application::load_project, py::arg("path"),
               py::call_guard<py::gil_scoped_release>());
}
