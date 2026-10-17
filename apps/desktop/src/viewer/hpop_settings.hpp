#pragma once
#include <functional>
#include <orbitforge/core/propagation/hpop.hpp>
#include <orbitforge/runtime/data_catalog.hpp>
class QWidget;
namespace orbitforge::desktop {
class EopCatalog;
// Desktop defaults for a newly selected HPOP model; saved and SDK settings are unchanged.
inline core::HpopOptions new_hpop_options() {
    core::HpopOptions options;
    options.gravity_model = "egm2008";
    options.gravity_degree = options.gravity_order = 21;
    options.solid_tides.mode = core::SolidTideMode::permanent;
    return options;
}
// Commits only when the user accepts valid settings. Cancel leaves the draft intact.
bool edit_hpop_settings(
    core::HpopOptions &options, QWidget *parent,
    const std::vector<runtime::CatalogGravity> &models = {},
    const std::vector<runtime::CatalogEphemeris> &ephemerides = {},
    const std::function<std::vector<runtime::CatalogEphemeris>()> &manage_ephemerides = {},
    EopCatalog *catalog = nullptr);
} // namespace orbitforge::desktop
