#pragma once
#include <orbitforge/core/propagation/atmospheric_drag.hpp>

namespace orbitforge::core::detail {
AtmosphereEvaluation jacchia_roberts(const AtmosphereInput &input, double utc_gmat_mjd);
AtmosphereEvaluation nrlmsise00(const AtmosphereInput &input, int doy, double seconds_in_day);
} // namespace orbitforge::core::detail
