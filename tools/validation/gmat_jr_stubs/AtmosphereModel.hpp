#pragma once
#include <cmath>
#include <stdexcept>
#include <string>
#define GMAT_API
using Real = double;
using Integer = int;
struct GEOPARMS {
    double tkp, xtemp;
};
struct GmatBase {
    virtual ~GmatBase() = default;
};
struct CelestialBody {
    double GetPolarRadius() const { return 6378.137 * (1 - 1 / 298.257223563); }
};
struct SolarFluxReader {
    struct FluxData {
        double obsCtrF107a = 150, obsF107 = 150, kp[1]{3};
    };
    void SetHistoricDataSource(int) {}
    void SetPredictedDataSource(int) {}
    bool LoadFluxData(const std::string &, const std::string &) { return false; }
    void GetEpochs(double &, double &, double &, double &) {}
    void SetSchattenFlags(int, int) {}
    FluxData GetInputs(double) { return {}; }
    void PrepareKpData(FluxData &, double) {}
};
struct AtmosphereModel : GmatBase {
    AtmosphereModel(const char *, const std::string &) {}
    void SetCentralBody(CelestialBody *value) { mCentralBody = value; }
    void BuildAngularVelocity(double) {}
    double CalculateGeodetics(double *, double, bool) {
        throw std::logic_error("Reference harness must pass geodetic latitude and height directly");
    }
    double nominalF107 = 150, nominalF107a = 150, nominalKp = 3, constantF107 = 150,
           constantF107a = 150, constantKp = 3;
    double historicStart = 0, historicEnd = 0, predictStart = 0, predictEnd = 0, geoLat = 0,
           wUpdateEpoch = 0;
    int historicalDataSource = 0, predictedDataSource = 0, schattenTimingModel = 0,
        schattenErrorModel = 0;
    std::string CSSIFileName, schattenFileName;
    bool fluxReaderLoaded = false;
    SolarFluxReader reader;
    SolarFluxReader *fluxReader = &reader;
    double sunVector[3]{1, 0, .2};
    CelestialBody *mCentralBody = nullptr;
};
