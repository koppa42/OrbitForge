// Streaming adapter to pinned, unmodified GMAT JR and the author's NRLMSISE-00.
// Only API stubs: no OrbitForge headers, libraries, coordinate or derivative code.
#include "AtmosphereModel.hpp"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#define private public
#include "JacchiaRobertsAtmosphere.cpp"
#include "JacchiaRobertsAtmosphere.hpp"
#undef private
extern "C" {
#include "nrlmsise-00.h"
}
int main() {
    int model, history;
    double altitude, latitude, longitude, hour, declination, f107, average, kp, ap;
    std::cout << std::setprecision(17);
    while (std::cin >> model >> altitude >> latitude >> longitude >> hour >> declination >> f107 >>
           average >> kp >> ap >> history) {
        if (model == 0) {
            JacchiaRobertsAtmosphere jr;
            jr.nominalF107 = f107;
            jr.nominalF107a = average;
            jr.nominalKp = kp;
            jr.geoLat = latitude * 180 / std::numbers::pi;
            jr.cbPolarRadius = 6378.137 * (1 - 1 / 298.257223563);
            jr.cbPolarSquared = jr.cbPolarRadius * jr.cbPolarRadius;
            double r[]{std::cos(latitude) * std::cos(hour), std::cos(latitude) * std::sin(hour),
                       std::sin(latitude)};
            double sun[]{std::cos(declination), 0, std::sin(declination)};
            // UTC 2000-02-29 13:23:12.5, JD - 2430000.
            std::cout << 1000 *
                             jr.JacchiaRoberts(altitude / 1000, r, sun, 21603.5 + 48192.5 / 86400)
                      << '\n';
        } else {
            nrlmsise_input in{};
            nrlmsise_flags flags{};
            nrlmsise_output out{};
            ap_array past{{7, 14, 91, 110, 41, 38, 23}};
            for (auto &s : flags.switches)
                s = 1;
            flags.switches[9] = history ? -1 : 1;
            in.year = 2000;
            in.doy = 60;
            in.sec = 48192.5;
            in.alt = altitude / 1000;
            in.g_lat = latitude * 180 / std::numbers::pi;
            in.g_long = longitude * 180 / std::numbers::pi;
            in.lst = 12 + hour * 12 / std::numbers::pi;
            in.f107 = f107;
            in.f107A = average;
            in.ap = ap;
            in.ap_a = &past;
            gtd7d(&in, &flags, &out);
            std::cout << out.d[5] << '\n';
        }
    }
    return std::cin.eof() ? 0 : 1;
}
