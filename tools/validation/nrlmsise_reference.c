/* Independent official-author C evaluator; never links OrbitForge. See the density protocol. */
#include "nrlmsise-00.h"
#include <math.h>
#include <stdio.h>

static double ap_from_kp(double kp) {
    const double values[] = {0,  2,  3,  4,  5,  6,  7,   9,   12,  15,  18,  22,  27,  32,
                             39, 48, 56, 67, 80, 94, 111, 132, 154, 179, 207, 236, 300, 400};
    const double scaled = kp * 3;
    const int i = (int)scaled;
    return i == 27 ? values[i] : values[i] + (scaled - i) * (values[i + 1] - values[i]);
}
int main(void) {
    const double pi = 3.141592653589793238462643383279502884;
    const int dates[][4] = {{1999, 7, 9, 190}, {2000, 2, 29, 60}, {2016, 12, 31, 366}};
    const double sec[] = {29000, 45000, 86400};
    const double alt[] = {0, 32.5, 50, 72.5, 90, 100, 125, 200, 400, 500, 1000};
    const double geometry[][3] = {{60, -70, 16}, {-90, 10, 0}, {0, 0, 12}, {90, -170, 23}};
    const double weather[][3] = {{70, 80, 0}, {150, 150, 3}, {250, 220, 8.5}};
    int d, z, g, w, i;
    puts("# Author NRLMSISE-00 C bc9a2fe; gtd7d, SI, daily Ap; independent of OrbitForge");
    puts("# year month day UTC_seconds altitude_m latitude_rad longitude_rad solar_hour_angle_rad "
         "sun_declination_rad f107 f107_average kp density_kg_m3 temperature_K "
         "exospheric_temperature_K");
    for (d = 0; d < 3; d++)
        for (z = 0; z < 11; z++)
            for (g = 0; g < 4; g++)
                for (w = 0; w < 3; w++) {
                    struct nrlmsise_input input = {0};
                    struct nrlmsise_flags flags = {0};
                    struct nrlmsise_output output = {0};
                    for (i = 0; i < 24; i++)
                        flags.switches[i] = 1;
                    input.year = dates[d][0];
                    input.doy = dates[d][3];
                    input.sec = sec[d];
                    input.alt = alt[z];
                    input.g_lat = geometry[g][0];
                    input.g_long = geometry[g][1];
                    input.lst = geometry[g][2];
                    input.f107 = weather[w][0];
                    input.f107A = weather[w][1];
                    input.ap = ap_from_kp(weather[w][2]);
                    gtd7d(&input, &flags, &output);
                    printf("%d %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g "
                           "%.17g %.17g\n",
                           dates[d][0], dates[d][1], dates[d][2], sec[d], alt[z] * 1000,
                           geometry[g][0] * pi / 180, geometry[g][1] * pi / 180,
                           (geometry[g][2] - 12) * pi / 12, .2, input.f107, input.f107A,
                           weather[w][2], output.d[5], output.t[1], output.t[0]);
                }
    return 0;
}
