#include <orbitforge/application/scenario.hpp>
#include <orbitforge/runtime/demo.hpp>
#include <orbitforge/runtime/tabulated_orbit.hpp>
#include <orbitforge/runtime/two_body.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;
using namespace orbitforge;

void bind_time_frames(py::module_ &module);
void bind_ephemeris(py::module_ &module);
void bind_j2(py::module_ &module);
void bind_project(py::module_ &module);
void bind_project_document(py::module_ &module);
void bind_data_lock(py::module_ &module);
void bind_data_catalog(py::module_ &module);
void bind_reports(py::module_ &module);
void bind_analysis(py::module_ &module);

PYBIND11_MODULE(_native, module) {
    module.doc() = "OrbitForge P0 native API. Time is explicit TAI, positions are metres.";
    module.attr("__version__") = ORBITFORGE_VERSION;
    py::class_<core::Epoch>(module, "Epoch")
        .def(py::init<std::int64_t>(), py::arg("tai_nanoseconds_since_2000_noon"))
        .def_property_readonly("tai_nanoseconds", &core::Epoch::tai_nanoseconds)
        .def("seconds_since", &core::Epoch::seconds_since, py::arg("other"));
    py::enum_<core::FrameId>(module, "FrameId")
        .value("GCRF", core::FrameId::gcrf)
        .value("ITRF", core::FrameId::itrf)
        .value("SPICE_J2000", core::FrameId::spice_j2000)
        .value("TEME", core::FrameId::teme);
    py::enum_<core::OriginId>(module, "OriginId").value("EARTH", core::OriginId::earth);
    py::class_<core::CartesianState>(module, "CartesianState")
        .def(py::init<core::Epoch, core::OriginId, core::FrameId, std::array<double, 3>,
                      std::array<double, 3>>(),
             py::arg("epoch"), py::arg("origin"), py::arg("frame"), py::arg("position_m"),
             py::arg("velocity_mps"))
        .def_readonly("epoch", &core::CartesianState::epoch)
        .def_readonly("frame", &core::CartesianState::frame)
        .def_readonly("origin", &core::CartesianState::origin)
        .def_readonly("position_m", &core::CartesianState::position_m)
        .def_readonly("velocity_mps", &core::CartesianState::velocity_mps);
    py::class_<results::Provenance>(module, "Provenance")
        .def_readonly("object_id", &results::Provenance::object_id)
        .def_readonly("input_revision", &results::Provenance::input_revision)
        .def_readonly("algorithm", &results::Provenance::algorithm);
    py::class_<results::StateSeries>(module, "StateSeries")
        .def_property_readonly("provenance", &results::StateSeries::provenance)
        .def_property_readonly("states", &results::StateSeries::states);
    py::class_<application::Session>(module, "Session")
        .def_property_readonly("provenance", &application::Session::provenance)
        .def(
            "sample",
            [](const application::Session &session, const std::vector<core::Epoch> &epochs) {
                return session.sample(epochs);
            },
            py::arg("epochs"), py::call_guard<py::gil_scoped_release>());
    bind_data_lock(module);
    py::class_<application::Scenario>(module, "Scenario")
        .def(py::init<std::string, core::Epoch, core::Epoch>(), py::arg("name"), py::arg("first"),
             py::arg("last"))
        .def_property_readonly("name", &application::Scenario::name)
        .def_property_readonly("first", &application::Scenario::first)
        .def_property_readonly("last", &application::Scenario::last)
        .def_property_readonly("revision", &application::Scenario::revision)
        .def_property_readonly("data_lock", &application::Scenario::data_lock,
                               py::return_value_policy::copy)
        .def("with_data_lock", &application::Scenario::with_data_lock, py::arg("data_lock"))
        .def("with_settings", &application::Scenario::with_settings, py::arg("name"),
             py::arg("first"), py::arg("last"))
        .def_property_readonly(
            "object_count",
            [](const application::Scenario &scenario) { return scenario.objects().size(); })
        .def("with_object", &application::Scenario::with_object, py::arg("session"),
             py::call_guard<py::gil_scoped_release>())
        .def("without_object", &application::Scenario::without_object, py::arg("object_id"),
             py::call_guard<py::gil_scoped_release>())
        .def(
            "sample",
            [](const application::Scenario &scenario, const std::vector<core::Epoch> &epochs) {
                return scenario.sample(epochs);
            },
            py::arg("epochs"), py::call_guard<py::gil_scoped_release>());
    module.def("make_circular_demo_session", &runtime::make_circular_demo_session);
    module.def("demo_epochs", &runtime::demo_epochs);
    py::register_exception<core::PropagationError>(module, "PropagationError", PyExc_RuntimeError);
    module.def("two_body_available", &runtime::two_body_available);
    py::class_<core::KeplerianElements>(module, "KeplerianElements")
        .def(py::init<core::Epoch, double, double, double, double, double, double>(),
             py::arg("epoch"), py::arg("semi_major_axis_m"), py::arg("eccentricity"),
             py::arg("inclination_rad"), py::arg("raan_rad"), py::arg("argument_of_periapsis_rad"),
             py::arg("true_anomaly_rad"))
        .def_readonly("epoch", &core::KeplerianElements::epoch)
        .def_readonly("semi_major_axis_m", &core::KeplerianElements::semi_major_axis_m)
        .def_readonly("eccentricity", &core::KeplerianElements::eccentricity)
        .def_readonly("inclination_rad", &core::KeplerianElements::inclination_rad)
        .def_readonly("raan_rad", &core::KeplerianElements::raan_rad)
        .def_readonly("argument_of_periapsis_rad",
                      &core::KeplerianElements::argument_of_periapsis_rad)
        .def_readonly("true_anomaly_rad", &core::KeplerianElements::true_anomaly_rad);
    module.def(
        "to_cartesian",
        [](core::KeplerianElements elements, double mu) {
            return runtime::to_cartesian(elements, core::GravitationalParameter{mu});
        },
        py::arg("elements"), py::arg("gravitational_parameter_m3_s2"),
        py::call_guard<py::gil_scoped_release>());
    module.def(
        "to_keplerian",
        [](core::CartesianState state, double mu) {
            return runtime::to_keplerian(state, core::GravitationalParameter{mu});
        },
        py::arg("state"), py::arg("gravitational_parameter_m3_s2"),
        py::call_guard<py::gil_scoped_release>());
    module.def(
        "make_two_body_session",
        [](core::CartesianState initial, double gravitational_parameter_m3_s2,
           std::string object_id, std::uint64_t input_revision) {
            return runtime::make_two_body_session(
                std::move(initial), core::GravitationalParameter{gravitational_parameter_m3_s2},
                std::move(object_id), input_revision);
        },
        py::arg("initial"), py::arg("gravitational_parameter_m3_s2"), py::arg("object_id"),
        py::arg("input_revision") = 1, py::call_guard<py::gil_scoped_release>());
    py::enum_<core::OrbitInterpolationMethod>(module, "OrbitInterpolationMethod")
        .value("LAGRANGE", core::OrbitInterpolationMethod::lagrange)
        .value("HERMITE", core::OrbitInterpolationMethod::hermite)
        .value("VOP", core::OrbitInterpolationMethod::vop);
    py::class_<core::OrbitInterpolationOptions>(module, "OrbitInterpolationOptions")
        .def(py::init<>())
        .def_readwrite("method", &core::OrbitInterpolationOptions::method)
        .def_readwrite("order", &core::OrbitInterpolationOptions::order)
        .def_readwrite("vop_mu", &core::OrbitInterpolationOptions::vop_mu);
    module.def(
        "make_tabulated_orbit_session",
        [](std::vector<core::OrbitEphemerisArc> arcs, core::OrbitInterpolationOptions options,
           std::string object_id, std::uint64_t revision) {
            return runtime::make_tabulated_orbit_session(std::move(arcs), options,
                                                         std::move(object_id), revision);
        },
        py::arg("arcs"), py::arg("options") = core::OrbitInterpolationOptions{},
        py::arg("object_id") = "satellite-1", py::arg("input_revision") = 1,
        py::call_guard<py::gil_scoped_release>());
    bind_time_frames(module);
    bind_ephemeris(module);
    bind_j2(module);
    bind_reports(module);
    bind_project(module);
    bind_project_document(module);
    bind_data_catalog(module);
    bind_analysis(module);
}
