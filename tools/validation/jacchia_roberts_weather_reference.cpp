// Streaming validation adapter for pinned, unmodified NASA GMAT c9b78bd1.
// API-only stubs; no OrbitForge headers, interpolation or density implementation.
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
int main() {
    double jd, altitude, latitude, hour_angle, declination, f107, average, kp;
    std::cout << std::setprecision(17);
    while (std::cin >> jd >> altitude >> latitude >> hour_angle >> declination >> f107 >> average >>
           kp) {
        JacchiaRobertsAtmosphere model;
        model.nominalF107 = f107;
        model.nominalF107a = average;
        model.nominalKp = kp;
        model.geoLat = latitude * 180 / std::numbers::pi;
        model.cbPolarRadius = 6378.137 * (1 - 1 / 298.257223563);
        model.cbPolarSquared = model.cbPolarRadius * model.cbPolarRadius;
        double r[]{std::cos(latitude) * std::cos(hour_angle),
                   std::cos(latitude) * std::sin(hour_angle), std::sin(latitude)};
        double sun[]{std::cos(declination), 0, std::sin(declination)};
        std::cout << 1000 * model.JacchiaRoberts(altitude / 1000, r, sun, jd) << '\n';
    }
    return std::cin.eof() ? 0 : 1;
}
