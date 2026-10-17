#include <orbitforge/runtime/ephemeris.hpp>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

namespace py = pybind11;
using namespace orbitforge;
void bind_ephemeris(py::module_ &module) {
    py::register_exception<core::EphemerisDataError>(module, "EphemerisDataError",
                                                     PyExc_ValueError);
    py::register_exception<core::EphemerisCoverageError>(module, "EphemerisCoverageError",
                                                         PyExc_ValueError);
    py::register_exception<core::EphemerisQueryError>(module, "EphemerisQueryError",
                                                      PyExc_RuntimeError);
    py::class_<core::TdbTime>(module, "TdbTime")
        .def(py::init<double>(), py::arg("seconds_past_j2000"))
        .def_readonly("seconds_past_j2000", &core::TdbTime::seconds_past_j2000);
    py::class_<core::EphemerisState>(module, "EphemerisState")
        .def_readonly("epoch", &core::EphemerisState::epoch)
        .def_readonly("target_naif_id", &core::EphemerisState::target_naif_id)
        .def_readonly("observer_naif_id", &core::EphemerisState::observer_naif_id)
        .def_readonly("frame", &core::EphemerisState::frame)
        .def_readonly("position_m", &core::EphemerisState::position_m)
        .def_readonly("velocity_mps", &core::EphemerisState::velocity_mps)
        .def_readonly("velocity_time_scale", &core::EphemerisState::velocity_time_scale)
        .def_readonly("tdb", &core::EphemerisState::tdb)
        .def_readonly("source_id", &core::EphemerisState::source_id);
    py::class_<core::KernelDataInfo>(module, "KernelDataInfo")
        .def_readonly("file", &core::KernelDataInfo::file)
        .def_readonly("kind", &core::KernelDataInfo::kind)
        .def_readonly("sha256", &core::KernelDataInfo::sha256)
        .def_readonly("source_url", &core::KernelDataInfo::source_url)
        .def_readonly("source_sha256", &core::KernelDataInfo::source_sha256)
        .def_readonly("producer", &core::KernelDataInfo::producer);
    py::class_<core::EphemerisDataInfo>(module, "EphemerisDataInfo")
        .def_readonly("id", &core::EphemerisDataInfo::id)
        .def_readonly("manifest_sha256", &core::EphemerisDataInfo::manifest_sha256)
        .def_readonly("model", &core::EphemerisDataInfo::model)
        .def_readonly("toolkit_version", &core::EphemerisDataInfo::toolkit_version)
        .def_readonly("first_epoch", &core::EphemerisDataInfo::first_epoch)
        .def_readonly("last_epoch", &core::EphemerisDataInfo::last_epoch)
        .def_readonly("kernels", &core::EphemerisDataInfo::kernels);
    py::class_<core::IEphemerisProvider>(module, "EphemerisProvider")
        .def("to_tdb", &core::IEphemerisProvider::to_tdb, py::arg("epoch"),
             py::call_guard<py::gil_scoped_release>())
        .def("from_tdb", &core::IEphemerisProvider::from_tdb, py::arg("time"),
             py::call_guard<py::gil_scoped_release>())
        .def("tdb_seconds_per_tai_second", &core::IEphemerisProvider::tdb_seconds_per_tai_second,
             py::arg("epoch"), py::call_guard<py::gil_scoped_release>())
        .def("geometric_state", &core::IEphemerisProvider::geometric_state,
             py::arg("target_naif_id"), py::arg("observer_naif_id"), py::arg("epoch"),
             py::call_guard<py::gil_scoped_release>())
        .def("apparent_sun_from_earth", &core::IEphemerisProvider::apparent_sun_from_earth,
             py::arg("epoch"), py::call_guard<py::gil_scoped_release>())
        .def("apparent_sun_from_spacecraft",
             &core::IEphemerisProvider::apparent_sun_from_spacecraft, py::arg("epoch"),
             py::arg("position_earth_m"), py::arg("velocity_earth_tai_mps"),
             py::call_guard<py::gil_scoped_release>())
        .def("validate_spacecraft_apparent_sun_interval",
             &core::IEphemerisProvider::validate_spacecraft_apparent_sun_interval, py::arg("first"),
             py::arg("last"), py::call_guard<py::gil_scoped_release>())
        .def("validate_apparent_sun_interval",
             &core::IEphemerisProvider::validate_apparent_sun_interval, py::arg("first"),
             py::arg("last"), py::call_guard<py::gil_scoped_release>())
        .def(
            "geometric_states",
            [](const core::IEphemerisProvider &provider, std::int32_t target, std::int32_t observer,
               const std::vector<core::Epoch> &epochs) {
                return provider.geometric_states(target, observer, epochs);
            },
            py::arg("target_naif_id"), py::arg("observer_naif_id"), py::arg("epochs"),
            py::call_guard<py::gil_scoped_release>())
        .def_property_readonly("data_info", &core::IEphemerisProvider::data_info,
                               py::return_value_policy::copy);
    module.def("ephemeris_available", &runtime::ephemeris_available);
    module.def("load_spice_snapshot", &runtime::load_spice_snapshot, py::arg("manifest"),
               py::arg("workspace"), py::call_guard<py::gil_scoped_release>());
}
