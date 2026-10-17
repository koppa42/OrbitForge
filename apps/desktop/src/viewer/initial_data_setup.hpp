#pragma once
#include "data_inventory.hpp"
#include <functional>
#include <vector>
class QWidget;

namespace orbitforge::desktop {
class EopCatalog;
[[nodiscard]] std::vector<DataProduct> initial_data_products();
// Blocks entry to the workbench until required resources are installed. The modal
// event loop remains responsive while catalog downloads and validation run in background.
// False means cancellation; completed resources remain cached for the next launch.
bool prepare_initial_data(QWidget *parent, EopCatalog &catalog,
                          const std::vector<DataProduct> &required = initial_data_products(),
                          const std::function<void(DataProduct)> &download = {});
} // namespace orbitforge::desktop
