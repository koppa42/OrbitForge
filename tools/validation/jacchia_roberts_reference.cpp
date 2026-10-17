// Compiles with the pinned, unmodified NASA GMAT scalar model and API-only stubs.
// No OrbitForge headers/libraries or physical calculations in the stubs.
#include "AtmosphereModel.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#define private public
#include "JacchiaRobertsAtmosphere.cpp"
#include "JacchiaRobertsAtmosphere.hpp"
#undef private

int main() {
    constexpr double pi = 3.141592653589793238462643383279502884;
    const int dates[][3] = {{1999, 7, 9}, {2000, 2, 29}, {2016, 12, 31}};
    // Gregorian UTC JD-2430000 offsets, independently obtained from civil dates.
    const double mjd[] = {21368.5, 21603.5, 27753.5}, sec[] = {29000, 45000, 86400};
    const double alt[] = {90,     95,  99.99, 100, 110, 125,  126,  150,
                          199.99, 200, 400,   500, 501, 1000, 1500, 2500};
    const double geometry[][3] = {{60, .2, 1.2}, {-80, -.3, -1.8}, {0, .4, .8}, {80, -.2, 2.6}};
    const double weather[][3] = {{70, 80, 0}, {150, 150, 3}, {250, 220, 8.5}};
    puts("# NASA GMAT c9b78bd scalar JR; original numerical methods, kg/m3; independent of "
         "OrbitForge");
    puts("# year month day UTC_seconds altitude_m latitude_rad longitude_rad solar_hour_angle_rad "
         "sun_declination_rad f107 f107_average kp density_kg_m3");
    for (int d = 0; d < 3; d++)
        for (double height : alt)
            for (const auto &g : geometry)
                for (const auto &w : weather) {
                    JacchiaRobertsAtmosphere model;
                    model.nominalF107 = w[0];
                    model.nominalF107a = w[1];
                    model.nominalKp = w[2];
                    model.geoLat = g[0];
                    model.cbPolarRadius = 6378.137 * (1 - 1 / 298.257223563);
                    model.cbPolarSquared = model.cbPolarRadius * model.cbPolarRadius;
                    const double lat = g[0] * pi / 180;
                    double r[] = {cos(lat) * cos(g[2]), cos(lat) * sin(g[2]), sin(lat)};
                    double sun[] = {cos(g[1]), 0, sin(g[1])};
                    const double density =
                        1000 * model.JacchiaRoberts(height, r, sun, mjd[d] + sec[d] / 86400);
                    printf("%d %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g\n",
                           dates[d][0], dates[d][1], dates[d][2], sec[d], height * 1000, lat, .5,
                           g[2], g[1], w[0], w[1], w[2], density);
                }
}
