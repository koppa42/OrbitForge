//$Id$
//------------------------------------------------------------------------------
//                              JacchiaRobertsAtmosphere
//------------------------------------------------------------------------------
// GMAT: General Mission Analysis Tool.
//
// Copyright (c) 2002-2026 United States Government as represented by the
// Administrator of the National Aeronautics and Space Administration.
// All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// You may not use this file except in compliance with the License.
// You may obtain a copy of the License at:
// http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
// express or implied.   See the License for the specific language
// governing permissions and limitations under the License.
//
// Developed jointly by NASA/GSFC and Thinking Systems, Inc. under contract
// number NNG04CC06P
//
// Author: Waka A. Waktola
// Created: 2004/05/11
//
/**
 * The Jacchia-Roberts atmosphere.  The implementation here is derived from the
 * Windows Swingby code.
 */
//------------------------------------------------------------------------------

// OrbitForge adaptation: explicit inputs/local scratch; finite angular limits; bounded roots.

#include "../atmosphere_models.hpp"
#include <cmath>
#include <numbers>
#include <orbitforge/core/propagation/state_provider.hpp>
namespace orbitforge::core::detail {
namespace {
using Real = double;
using Integer = int;
struct GEOPARMS {
    double tkp, xtemp;
};
class JacchiaRoberts {
  public:
    double root1 = 0, root2 = 0, x_root = 0, y_root = 0, t_infinity = 0, tx = 0, sum = 0;
    double high_temperature_exponent = 0;
    double cbPolarRadius = 6378.137 * (1 - 1 / 298.257223563),
           cbPolarSquared = cbPolarRadius * cbPolarRadius;
    static constexpr double RHO_ZERO = 3.46e-9;
    static constexpr double TZERO = 183.0;
    static constexpr double G_ZERO = 9.80665;
    static constexpr double GAS_CON = 8.31432;
    static constexpr double AVOGADRO = 6.022045e23;
    static constexpr double CON_C[5] = {-89284375.0, 3542400.0, -52687.5, 340.5, -0.8};
    static constexpr double CON_L[5] = {0.1031445e5, 0.2341230e1, 0.1579202e-2, -0.1252487e-5,
                                        0.2462708e-9};
    static constexpr double MZERO = 28.82678;
    static constexpr double M_CON[7] = {-435093.363387, 28275.5646391, -765.33466108,  11.043387545,
                                        -0.08958790995, 0.00038737586, -0.000000697444};
    static constexpr double S_CON[6] = {3144902516.672729, -123774885.4832917,
                                        1816141.096520398, -11403.31079489267,
                                        24.36498612105595, 0.008957502869707995};
    static constexpr double S_BETA[6] = {
        -52864482.17910969, -16632.50847336828, -1.308252378125, 0.0, 0.0, 0.0};
    static constexpr double OMEGA = -0.94585589;
    static constexpr double ZETA_CON[7] = {0.1985549e-10,  -0.1833490e-14, 0.1711735e-17,
                                           -0.1021474e-20, 0.3727894e-24,  -0.7734110e-28,
                                           0.7026942e-32};
    static constexpr double MOL_MASS[6] = {28.0134, 39.948, 4.0026, 31.9988, 15.9994, 1.00797};
    static constexpr double NUM_DENS[5] = {0.78110, 0.93432e-2, 0.61471e-5, 0.161778, 0.95544e-1};
    static constexpr double CON_DEN[5][7] = {
        {0.1093155e2, 0.1186783e-2, -0.1677341e-5, 0.1420228e-8, -0.7139785e-12, 0.1969715e-15,
         -0.2296182e-19},
        {0.8049405e1, 0.2382822e-2, -0.3391366e-5, 0.2909714e-8, -0.1481702e-11, 0.4127600e-15,
         -0.4837461e-19},
        {0.7646886e1, -0.4383486e-3, 0.4694319e-6, -0.2894886e-9, 0.9451989e-13, -0.1270838e-16,
         0.0},
        {0.9924237e1, 0.1600311e-2, -0.2274761e-5, 0.1938454e-8, -0.9782183e-12, 0.2698450e-15,
         -0.3131808e-19},
        {0.1097083e2, 0.6118742e-4, -0.1165003e-6, 0.9239354e-10, -0.3490739e-13, 0.5116298e-17,
         0.0}};
    Real exotherm(Real *, Real *, GEOPARMS *, Real, Real, Real);
    Real rho_100(Real, Real);
    Real rho_125(Real, Real);
    Real rho_cor(Real, Real, Real, GEOPARMS *);
    Real rho_high(Real, Real, Real, Real, Real);
    void roots(Real[], Integer, Real[][2], Integer);
    void deflate_polynomial(Real[], Integer, Real, Real[]);
};
Real JacchiaRoberts::exotherm(Real space_craft[2], Real sun[3], GEOPARMS *geo, Real height,
                              Real sun_dec, Real geo_lat) {

    Real exotemp, expkp, hour_angle;
    Real theta, eta, tau, th22, t1;
    Real c_star[5], aux[4][2];
    Integer i;
    const Integer na = 5;

    // atan2 has a finite noon/midnight and pole limit; the upstream signed-acos
    // expression divides by its horizontal cross product at these positions.
    hour_angle = atan2(sun[0] * space_craft[1] - sun[1] * space_craft[0],
                       sun[0] * space_craft[0] + sun[1] * space_craft[1]);
    theta = 0.5 * fabs(geo_lat + sun_dec);
    eta = 0.5 * fabs(geo_lat - sun_dec);
    tau = hour_angle - 0.64577182325 + 0.10471975512 * sin(hour_angle + 0.75049157836);
    if (tau < -std::numbers::pi)
        tau += 2 * std::numbers::pi;
    else if (tau > std::numbers::pi)
        tau -= 2 * std::numbers::pi;
    th22 = pow(sin(theta), 2.2);
    t1 = (Real)geo->xtemp *
         (1.0 + 0.3 * (th22 + pow(cos(0.5 * tau), 3.0) * (pow(cos(eta), 2.2) - th22)));
    expkp = exp(geo->tkp);

    if (height < 200.0) {
        t_infinity = t1 + 14.0 * geo->tkp + 0.02 * expkp;
    } else {
        t_infinity = t1 + 28.0 * geo->tkp + 0.03 * expkp;
    }

    tx = 371.6678 + 0.0518806 * t_infinity - 294.3505 * exp(-0.00216222 * t_infinity);

    if (height < 125.0) {

        for (sum = CON_C[4], i = 3; i >= 0; i--) {
            sum = CON_C[i] + sum * height;
        }

        exotemp = tx + (tx - TZERO) * sum / 1.500625e6;
    }

    else if (height > 125.0) {

        for (sum = CON_L[4], i = 3; i >= 0; i--) {
            sum = CON_L[i] + sum * t_infinity;
        }

        high_temperature_exponent = -(tx - TZERO) / (t_infinity - tx) * (height - 125.0) / 35.0 *
                                    sum / (cbPolarRadius + height);
        exotemp = t_infinity - (t_infinity - tx) * exp(high_temperature_exponent);
    } else
        exotemp = tx;

    if (height <= 125.0) {

        for (c_star[0] = CON_C[0] + 1500625.0 * tx / (tx - TZERO), i = 1; i <= 4; i++) {
            c_star[i] = CON_C[i];
        }

        aux[0][0] = 125.0;
        aux[0][1] = 0.0;
        roots(c_star, na, aux, 1);
        root1 = aux[0][0];

        deflate_polynomial(c_star, na, root1, c_star);

        aux[0][0] = 200.0;
        aux[0][1] = 0.0;
        roots(c_star, na - 1, aux, 1);
        root2 = aux[0][0];

        deflate_polynomial(c_star, na - 1, root2, c_star);

        aux[0][0] = 10.0;
        aux[0][1] = 125.0;
        roots(c_star, na - 2, aux, 1);
        x_root = aux[0][0];
        y_root = fabs(aux[0][1]);
    }

    return exotemp;
}

Real JacchiaRoberts::rho_100(Real height, Real temperature) {
    Real m_poly, s_poly, f2, p1, p2, p3, p4, p5, p6, b[6];
    Real x_star, u[2], w[2], v, factor_k, roots_2, log_f1;
    Integer i;

    for (m_poly = M_CON[6], i = 5; i >= 0; i--) {
        m_poly = m_poly * height + M_CON[i];
    }

    for (i = 0; i <= 5; i++) {
        b[i] = S_CON[i] + S_BETA[i] * tx / (tx - TZERO);
    }

    roots_2 = x_root * x_root + y_root * y_root;
    x_star = -2.0 * root1 * root2 * cbPolarRadius *
             (cbPolarSquared + 2.0 * cbPolarRadius * x_root + roots_2);
    v = (cbPolarRadius + root1) * (cbPolarRadius + root2) *
        (cbPolarSquared + 2.0 * cbPolarRadius * x_root + roots_2);
    u[0] = (root1 - root2) * (root1 + cbPolarRadius) * (root1 + cbPolarRadius) *
           (root1 * root1 - 2.0 * root1 * x_root + roots_2);
    u[1] = (root1 - root2) * (root2 + cbPolarRadius) * (root2 + cbPolarRadius) *
           (root2 * root2 - 2.0 * root2 * x_root + roots_2);
    w[0] =
        root1 * root2 * cbPolarRadius * (cbPolarRadius + root1) * (cbPolarRadius + roots_2 / root1);
    w[1] =
        root1 * root2 * cbPolarRadius * (cbPolarRadius + root2) * (cbPolarRadius + roots_2 / root2);

    for (s_poly = b[5], i = 4; i >= 0; i--) {
        s_poly = s_poly * root1 + b[i];
    }

    p2 = s_poly / u[0];

    for (s_poly = b[5], i = 4; i >= 0; i--) {
        s_poly = s_poly * root2 + b[i];
    }

    p3 = -s_poly / u[1];

    for (s_poly = b[5], i = 4; i >= 0; i--) {
        s_poly = -s_poly * cbPolarRadius + b[i];
    }

    p5 = s_poly / v;

    p4 = (b[0] -
          root1 * root2 * cbPolarSquared *
              (b[4] + b[5] * (2.0 * x_root + root1 + root2 - cbPolarRadius)) +
          w[0] * p2 + w[1] * p3 - root1 * root2 * b[5] * cbPolarRadius * roots_2 +
          root1 * root2 * (cbPolarSquared - roots_2) * p5) /
         x_star;

    p1 = b[5] - 2 * p4 - p3 - p2;

    p6 = b[4] + b[5] * (2.0 * x_root + root1 + root2 - cbPolarRadius) - p5 -
         2.0 * (x_root + cbPolarRadius) * p4 - (root2 + cbPolarRadius) * p3 -
         (root1 + cbPolarRadius) * p2;

    log_f1 = p1 * log((height + cbPolarRadius) / (90.0 + cbPolarRadius)) +
             p2 * log((height - root1) / (90.0 - root1)) +
             p3 * log((height - root2) / (90.0 - root2)) +
             p4 * log((height * height - 2.0 * x_root * height + roots_2) /
                      (8100.0 - 180.0 * x_root + roots_2));

    f2 = (height - 90.0) * (M_CON[6] + p5 / ((height + cbPolarRadius) * (90.0 + cbPolarRadius))) +
         p6 *
             atan(y_root * (height - 90.0) /
                  (y_root * y_root + (height - x_root) * (90.0 - x_root))) /
             y_root;

    factor_k = -G_ZERO / (GAS_CON * (tx - TZERO));

    return RHO_ZERO * TZERO * m_poly * exp(factor_k * (log_f1 + f2)) / (MZERO * temperature);
}

Real JacchiaRoberts::rho_125(Real height, Real temperature) {
    Real f4, q1, q2, q3, q4, q5, q6;
    Real rho_prime, x_star, u[2], w[2], v, factor_k, t_100;
    Real rho_sum, rhoi, roots_2, log_f3;
    Integer i;

    for (rho_prime = ZETA_CON[6], i = 5; i >= 0; i--) {
        rho_prime = rho_prime * t_infinity + ZETA_CON[i];
    }

    t_100 = tx + OMEGA * (tx - TZERO);

    roots_2 = x_root * x_root + y_root * y_root;
    x_star = -2.0 * root1 * root2 * cbPolarRadius *
             (cbPolarSquared + 2.0 * cbPolarRadius * x_root + roots_2);
    v = (cbPolarRadius + root1) * (cbPolarRadius + root2) *
        (cbPolarSquared + 2.0 * cbPolarRadius * x_root + roots_2);
    u[0] = (root1 - root2) * (root1 + cbPolarRadius) * (root1 + cbPolarRadius) *
           (root1 * root1 - 2.0 * root1 * x_root + roots_2);
    u[1] = (root1 - root2) * (root2 + cbPolarRadius) * (root2 + cbPolarRadius) *
           (root2 * root2 - 2.0 * root2 * x_root + roots_2);
    w[0] =
        root1 * root2 * cbPolarRadius * (cbPolarRadius + root1) * (cbPolarRadius + roots_2 / root1);
    w[1] =
        root1 * root2 * cbPolarRadius * (cbPolarRadius + root2) * (cbPolarRadius + roots_2 / root2);

    q2 = 1.0 / u[0];

    q3 = -1.0 / u[1];

    q5 = 1.0 / v;

    q4 = (1.0 + w[0] * q2 + w[1] * q3 + root1 * root2 * (cbPolarSquared - roots_2) * q5) / x_star;

    q1 = -2 * q4 - q3 - q2;

    q6 = -q5 - 2.0 * (x_root + cbPolarRadius) * q4 - (root2 + cbPolarRadius) * q3 -
         (root1 + cbPolarRadius) * q2;

    log_f3 = q1 * log((height + cbPolarRadius) / (100.0 + cbPolarRadius)) +
             q2 * log((height - root1) / (100.0 - root1)) +
             q3 * log((height - root2) / (100.0 - root2)) +
             q4 * log((height * height - 2.0 * x_root * height + roots_2) /
                      (1.0e4 - 200.0 * x_root + roots_2));

    f4 = (height - 100.0) * q5 / ((height + cbPolarRadius) * (100.0 + cbPolarRadius)) +
         q6 *
             atan(y_root * (height - 100.0) /
                  (y_root * y_root + (height - x_root) * (100.0 - x_root))) /
             y_root;

    factor_k = -1500625.0 * G_ZERO * cbPolarSquared / (GAS_CON * CON_C[4] * (tx - TZERO));

    for (rho_sum = 0.0, i = 0; i <= 4; i++) {
        rhoi = MOL_MASS[i] * NUM_DENS[i] * exp(MOL_MASS[i] * factor_k * (f4 + log_f3));

        if (i == 2) {
            rhoi *= pow(t_100 / temperature, -0.38);
        }
        rho_sum += rhoi;
    }

    return rho_sum * rho_prime * t_100 / temperature;
}

Real JacchiaRoberts::rho_cor(Real height, Real a1_time, Real geo_lat, GEOPARMS *geo) {
    Real geo_cor, semian_cor, slat_cor, f, g, day_58, tausa, alpha;
    Real sin_lat, eta_lat;

    if (height < 200.0) {
        geo_cor = 0.012 * geo->tkp + 0.000012 * exp(geo->tkp);
    } else {
        geo_cor = 0.0;
    }

    f = (5.876e-7 * pow(height, 2.331) + 0.06328) * exp(-0.002868 * height);
    day_58 = (a1_time - 6204.5) / 365.2422;
    tausa = day_58 +
            0.09544 * (pow(0.5 * (1.0 + sin(2 * std::numbers::pi * day_58 + 6.035)), 1.65) - 0.5);
    alpha = sin(4.0 * std::numbers::pi * tausa + 4.259);
    g = 0.02835 + (0.3817 + 0.17829 * sin(2 * std::numbers::pi * tausa + 4.137)) * alpha;
    semian_cor = f * g;

    sin_lat = sin(geo_lat);
    eta_lat = sin(2.0 * std::numbers::pi * day_58 + 1.72) * sin_lat * fabs(sin_lat);
    slat_cor = 0.014 * (height - 90.0) * eta_lat * exp(-0.0013 * (height - 90.0) * (height - 90.0));

    return pow(10.0, geo_cor + semian_cor + slat_cor);
}

Real JacchiaRoberts::rho_high(Real height, Real temperature, Real t_500, Real sun_dec,
                              Real geo_lat) {
    Real f, log_di, gamma, exp1, rho_out, r;
    Real di = 0;
    Integer i, j;

    for (rho_out = 0.0, i = 0; i <= 5; i++) {

        if (i <= 4) {
            for (log_di = CON_DEN[i][6], j = 5; j >= 0; j--) {
                log_di = log_di * t_infinity + CON_DEN[i][j];
            }
            di = pow(10.0, log_di) / AVOGADRO;
        }

        Real polar125 = cbPolarRadius + 125.0;
        gamma = 35.0 * MOL_MASS[i] * G_ZERO * cbPolarSquared * (t_infinity - tx) /
                (GAS_CON * sum * t_infinity * (tx - TZERO) * polar125);

        exp1 = 1.0 + gamma;

        f = 1.0;

        if (i == 2) {
            exp1 -= 0.38;

            // Continuous equinox limit, avoiding sun_dec/fabs(sun_dec) at zero.
            f = sun_dec == 0 ? 0
                             : 4.9914 * fabs(sun_dec) *
                                   (pow(sin(0.25 * std::numbers::pi -
                                            0.5 * geo_lat * std::copysign(1.0, sun_dec)),
                                        3) -
                                    0.35355) /
                                   std::numbers::pi;
            f = pow(10.0, f);
        }

        if (height > 500.0 && i == 5) {
            const double exponent500 = -(tx - TZERO) / (t_infinity - tx) * (500.0 - 125.0) / 35.0 *
                                       sum / (cbPolarRadius + 500.0);
            r = MOL_MASS[5] * pow(10.0, 73.13 - (39.4 - 5.5 * log10(t_500)) * log10(t_500)) *
                pow(t_500 / temperature, exp1) *
                exp(gamma * (high_temperature_exponent - exponent500)) / AVOGADRO;
            rho_out += r;

        } else if (i <= 4) {

            // T approaches T_infinity at high altitude. Evaluate their ratio
            // from the original temperature exponent, never subtract rounded T's.
            r = f * MOL_MASS[i] * di * pow(tx / temperature, exp1) *
                exp(gamma * high_temperature_exponent);
            rho_out += r;
        }
    }
    return rho_out;
}

void JacchiaRoberts::roots(Real a[], Integer na, Real croots[][2], Integer irl) {
    Integer i, ir, n1, n2, j;
    Real z[2], zs[2], cb[2], cc[2], dif, denom, temp;

    unsigned iterations = 0;
    ir = 0;
    n1 = na - 1;
    n2 = n1 - 1;
    dif = 0.0l;

    while (ir < irl) {
        z[0] = croots[ir][0];
        z[1] = croots[ir][1];

        do {
            if (++iterations > 200)
                throw PropagationError("JR polynomial roots did not converge");
            cb[0] = a[n1];
            cb[1] = 0;
            cc[0] = a[n1];
            cc[1] = 0;

            for (i = 0; i <= n2; ++i) {
                j = n2 - i;
                temp = (z[0] * cb[0] - z[1] * cb[1]) + a[j];
                cb[1] = z[0] * cb[1] + z[1] * cb[0];
                cb[0] = temp;
                if (j != 0) {
                    temp = (z[0] * cc[0] - z[1] * cc[1]) + cb[0];
                    cc[1] = (z[0] * cc[1] + z[1] * cc[0]) + cb[1];
                    cc[0] = temp;
                }
            }
            zs[0] = z[0];
            zs[1] = z[1];

            denom = cc[0] * cc[0] + cc[1] * cc[1];
            if (!std::isfinite(denom) || denom == 0)
                throw PropagationError("JR singular root iteration");
            z[0] -= ((cb[0] * cc[0] + cb[1] * cc[1]) / denom);
            z[1] += ((cb[0] * cc[1] - cb[1] * cc[0]) / denom);

            dif = fabs((zs[0] - z[0]) / zs[0]);
            if (zs[1] != 0.0E0) {
                dif += fabs((zs[1] - z[1]) / zs[1]);
            }
        } while (dif > 1.0E-14);

        croots[ir][0] = z[0];
        croots[ir][1] = z[1];
        ++ir;
    }
}

void JacchiaRoberts::deflate_polynomial(Real c[], Integer n, Real root, Real c_new[]) {
    int i;
    Real accumulator, save;

    accumulator = c[n - 1];

    for (i = n - 2; i >= 0; i--) {
        save = c[i];
        c_new[i] = accumulator;
        accumulator = save + accumulator * root;
    }
}

} // namespace
AtmosphereEvaluation jacchia_roberts(const AtmosphereInput &input, double utc_gmat_mjd) {
    JacchiaRoberts model;
    const double lat = input.geodetic.latitude_rad, dec = input.sun_declination_rad;
    double satellite[3] = {std::cos(lat) * std::cos(input.solar_hour_angle_rad),
                           std::cos(lat) * std::sin(input.solar_hour_angle_rad), std::sin(lat)};
    double sun[3] = {std::cos(dec), 0, std::sin(dec)};
    GEOPARMS geo{input.kp,
                 379 + 3.24 * input.f107_average + 1.3 * (input.f107 - input.f107_average)};
    const double height = input.geodetic.height_m / 1000;
    double density = 0, temperature = 183;
    if (height > 125) {
        const double t500 = model.exotherm(satellite, sun, &geo, 500, dec, lat);
        temperature = model.exotherm(satellite, sun, &geo, height, dec, lat);
        density = model.rho_high(height, temperature, t500, dec, lat);
    } else {
        temperature = model.exotherm(satellite, sun, &geo, height, dec, lat);
        density = height <= 90   ? model.RHO_ZERO
                  : height < 100 ? model.rho_100(height, temperature)
                                 : model.rho_125(height, temperature);
    }
    density *= 1000 * model.rho_cor(height, utc_gmat_mjd, lat, &geo);
    return {density, temperature, model.t_infinity};
}
} // namespace orbitforge::core::detail
