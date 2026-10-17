/* Independent author C gtd7d: native daily Ap and historical Ap (switch 9=-1).
 * Does not include or link OrbitForge. Sources are pinned in density_sources.json. */
#include "nrlmsise-00.h"
#include <stdio.h>

int main(void) {
    const double pi = 3.141592653589793238462643383279502884;
    const int dates[][4] = {{1999, 7, 9, 190}, {2000, 2, 29, 60}, {2016, 12, 31, 366}};
    const double sec[] = {29000, 45000, 86400};
    const double alt[] = {0, 32.5, 50, 72.5, 90, 100, 125, 200, 400, 500, 1000};
    const double geometry[][3] = {{60, -70, 16}, {-90, 10, 0}, {0, 0, 12}, {90, -170, 23}};
    const double history[][7] = {{0, 0, 0, 0, 0, 0, 0},
                                 {15.25, 0, 0, 0, 0, 0, 0},
                                 {400, 0, 0, 0, 0, 0, 0},
                                 {0, 0, 0, 0, 0, 0, 0},
                                 {15.25, 15.25, 15.25, 15.25, 15.25, 15.25, 15.25},
                                 {400, 400, 400, 400, 400, 400, 400},
                                 {40, 400, 100, 50, 20, 7.125, 3.75},
                                 {40, 0, 2, 6, 18, 88.75, 222.125}};
    int d, z, g, w, i;
    puts("# Author NRLMSISE-00 C bc9a2fe; gtd7d SI; daily/history Ap; independent of OrbitForge");
    puts("# year month day UTC_seconds altitude_m latitude_rad longitude_rad solar_hour_angle_rad "
         "f107 f107_average history_mode ap0 ap1 ap2 ap3 ap4 ap5 ap6 density_kg_m3 "
         "temperature_K exospheric_temperature_K");
    for (d = 0; d < 3; ++d)
        for (z = 0; z < 11; ++z)
            for (g = 0; g < 4; ++g)
                for (w = 0; w < 8; ++w) {
                    struct nrlmsise_input input = {0};
                    struct nrlmsise_flags flags = {0};
                    struct nrlmsise_output output = {0};
                    struct ap_array ap = {{0}};
                    const int history_mode = w >= 3;
                    for (i = 0; i < 24; ++i)
                        flags.switches[i] = 1;
                    flags.switches[9] = history_mode ? -1 : 1;
                    for (i = 0; i < 7; ++i)
                        ap.a[i] = history[w][i];
                    input.year = dates[d][0];
                    input.doy = dates[d][3];
                    input.sec = sec[d];
                    input.alt = alt[z];
                    input.g_lat = geometry[g][0];
                    input.g_long = geometry[g][1];
                    input.lst = geometry[g][2];
                    input.f107 = 80 + 70 * d;
                    input.f107A = 90 + 60 * d;
                    input.ap = ap.a[0];
                    input.ap_a = &ap;
                    gtd7d(&input, &flags, &output);
                    printf("%d %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %d", dates[d][0],
                           dates[d][1], dates[d][2], sec[d], alt[z] * 1000,
                           geometry[g][0] * pi / 180, geometry[g][1] * pi / 180,
                           (geometry[g][2] - 12) * pi / 12, input.f107, input.f107A, history_mode);
                    for (i = 0; i < 7; ++i)
                        printf(" %.17g", ap.a[i]);
                    printf(" %.17g %.17g %.17g\n", output.d[5], output.t[1], output.t[0]);
                }
    return 0;
}
