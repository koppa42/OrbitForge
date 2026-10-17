"""Immutable orbit inputs. Mean elements remain distinct from osculating ones."""
from dataclasses import dataclass, field
from typing import Literal
from . import _native as n
from . import units as u
from . import missions
from ._values import require


@dataclass(frozen=True, slots=True)
class Cartesian:
    epoch: n.Epoch
    position_m: tuple[float, float, float]
    velocity_mps: tuple[float, float, float]
    origin: n.OriginId = n.OriginId.EARTH
    frame: n.FrameId = n.FrameId.GCRF

    def __post_init__(self) -> None:
        object.__setattr__(self, "position_m", tuple(self.position_m))
        object.__setattr__(self, "velocity_mps", tuple(self.velocity_mps))

    def _native(self) -> n.CartesianState:
        return n.CartesianState(self.epoch, self.origin, self.frame, self.position_m, self.velocity_mps)

    @classmethod
    def _read(cls, value: n.CartesianState) -> "Cartesian":
        x, y, z = value.position_m
        vx, vy, vz = value.velocity_mps
        return cls(value.epoch, (x, y, z), (vx, vy, vz), value.origin, value.frame)


@dataclass(frozen=True, slots=True)
class Keplerian:
    epoch: n.Epoch
    semi_major_axis: u.Length
    eccentricity: float
    inclination: u.Angle
    raan: u.Angle = u.deg(0)
    argument_of_periapsis: u.Angle = u.deg(0)
    true_anomaly: u.Angle = u.deg(0)

    def _cartesian(self, mu: float | None = None) -> Cartesian:
        elements = n.KeplerianElements(
            self.epoch, require(self.semi_major_axis, u.Length, "semi_major_axis").metres,
            self.eccentricity, require(self.inclination, u.Angle, "inclination").radians,
            require(self.raan, u.Angle, "raan").radians,
            require(self.argument_of_periapsis, u.Angle, "argument_of_periapsis").radians,
            require(self.true_anomaly, u.Angle, "true_anomaly").radians,
        )
        return Cartesian._read(n.to_cartesian(
            elements, n.central_body().gravitational_parameter_m3_s2 if mu is None else mu))


THIRD_BODY_NAMES = ("sun", "moon", "jupiter", "venus", "saturn", "mars", "mercury", "uranus", "neptune", "pluto")


@dataclass(frozen=True, slots=True)
class ThirdBodySettings:
    """GM source and retained drafts for one body. mu is SI m^3/s^2.

    cb is a captured value from load_cb_gravity(); all positions use the selected DE440s.
    Mars and outer planets use system barycentres and require SystemGm in Cb files.
    """
    body: str
    source: Literal["jpl_de", "cb_file", "user_specified"] = "jpl_de"
    mu: float | None = None
    cb: n.CbGravity | None = None

    def _native(self) -> n.ThirdBodyGravity:
        if self.body not in THIRD_BODY_NAMES:
            raise ValueError("Unknown third body")
        sources = {"jpl_de": n.ThirdBodyGravitySource.JPL_DE,
                   "cb_file": n.ThirdBodyGravitySource.CB_FILE,
                   "user_specified": n.ThirdBodyGravitySource.USER_SPECIFIED}
        if self.source not in sources:
            raise ValueError("Unknown third-body gravity source")
        value = n.ThirdBodyGravity()
        value.source, value.user_mu, value.cb = sources[self.source], self.mu, self.cb
        return value


def load_cb_gravity(path, body: str, *, mu_scale: float = 1.0) -> n.CbGravity:
    """Capture Gm/SystemGm from body.cb. SI by default; km^3/s^2 uses mu_scale=1e9."""
    if body not in THIRD_BODY_NAMES:
        raise ValueError("Unknown third body")
    return n.load_cb_gravity(path, getattr(n.ThirdBody, body.upper()), mu_scale)


@dataclass(frozen=True, slots=True)
class SrpEclipsingBody:
    """Additional spherical planet occultor; physical centre, explicit SI radius.

    None uses the largest IAU 2015 axis from NAIF pck00011 as a starting sphere.
    Planet-centre ephemeris coverage is required; system barycentres are not used.
    """
    body: Literal["mercury", "venus", "mars", "jupiter", "saturn", "uranus", "neptune", "pluto"]
    radius_m: float | None = None

    def __post_init__(self) -> None:
        if self.body not in THIRD_BODY_NAMES or self.body in ("sun", "moon"):
            raise ValueError("Additional SRP occultors must be planets")
        native = n.default_srp_eclipsing_body(getattr(n.ThirdBody, self.body.upper()))
        if self.radius_m is None:
            object.__setattr__(self, "radius_m", native.radius_m)
        native.radius_m = self.radius_m
        options = n.SrpOptions()
        options.additional_eclipsing_bodies = [native]
        options.validate()

    def _native(self) -> n.SrpEclipsingBody:
        result = n.SrpEclipsingBody()
        result.body = getattr(n.ThirdBody, self.body.upper())
        result.radius_m = self.radius_m
        return result

    @classmethod
    def _read(cls, value: n.SrpEclipsingBody) -> "SrpEclipsingBody":
        return cls(value.body.name.lower(), value.radius_m)


@dataclass(frozen=True, slots=True)
class SrpSettings:
    """Spherical SRP using DE440s Sun positions and assigned spherical occultors.

    All quantities are SI. Dual cone integrates the uniform apparent solar disk;
    occultations use the union of assigned spheres and geometric body centres.
    Sun methods are true, geocentric apparent (default), and spacecraft apparent
    (received LT+S).
    Boundary mitigation splits shadow contacts and corrects penumbra integration.
    No ellipsoidal limb or reproduction of STK internals is implied. Disabled drafts are
    retained but do not affect results.
    """
    enabled: bool = False
    cr: float = 1.0
    area_mass_ratio_m2_per_kg: float = 0.02
    shadow_model: Literal["none", "cylindrical", "dual_cone"] = "dual_cone"
    sun_position_method: Literal["true", "apparent_to_true_cb", "apparent"] = "apparent_to_true_cb"
    earth_shadow: bool = True
    moon_shadow: bool = True
    boundary_mitigation: bool = False
    atmospheric_altitude_m: float = 0.0
    pressure_at_reference_pa: float = 4.56e-6
    reference_distance_m: float = 149597870700.0
    sun_radius_m: float = 695700000.0
    earth_radius_m: float = 6378137.0
    moon_radius_m: float = 1737400.0
    additional_eclipsing_bodies: tuple[SrpEclipsingBody, ...] = ()

    def __post_init__(self) -> None:
        for name in ("enabled", "earth_shadow", "moon_shadow", "boundary_mitigation"):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} requires bool")
        object.__setattr__(self, "additional_eclipsing_bodies", tuple(self.additional_eclipsing_bodies))
        if any(not isinstance(body, SrpEclipsingBody) for body in self.additional_eclipsing_bodies):
            raise TypeError("additional_eclipsing_bodies requires SrpEclipsingBody values")
        self._native()

    def _native(self) -> n.SrpOptions:
        shadows = {"none": n.SrpShadowModel.NONE,
                   "cylindrical": n.SrpShadowModel.CYLINDRICAL,
                   "dual_cone": n.SrpShadowModel.DUAL_CONE}
        if self.shadow_model not in shadows:
            raise ValueError("Unknown SRP shadow model")
        suns = {"true": n.SrpSunPositionMethod.TRUE,
                "apparent_to_true_cb": n.SrpSunPositionMethod.APPARENT_TO_TRUE_CB,
                "apparent": n.SrpSunPositionMethod.APPARENT}
        if self.sun_position_method not in suns:
            raise ValueError("Unknown SRP Sun position method")
        value = n.SrpOptions()
        for name in self.__dataclass_fields__:
            if name not in ("shadow_model", "sun_position_method", "additional_eclipsing_bodies"):
                setattr(value, name, getattr(self, name))
        value.additional_eclipsing_bodies = [body._native() for body in self.additional_eclipsing_bodies]
        value.shadow_model = shadows[self.shadow_model]
        value.sun_position_method = suns[self.sun_position_method]
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.SrpOptions) -> "SrpSettings":
        shadows = {n.SrpShadowModel.NONE: "none", n.SrpShadowModel.CYLINDRICAL: "cylindrical",
                   n.SrpShadowModel.DUAL_CONE: "dual_cone"}
        suns = {n.SrpSunPositionMethod.TRUE: "true",
                n.SrpSunPositionMethod.APPARENT_TO_TRUE_CB: "apparent_to_true_cb",
                n.SrpSunPositionMethod.APPARENT: "apparent"}
        fields = {name: getattr(value, name) for name in cls.__dataclass_fields__
                  if name not in ("shadow_model", "sun_position_method", "additional_eclipsing_bodies")}
        fields["additional_eclipsing_bodies"] = tuple(SrpEclipsingBody._read(body) for body in value.additional_eclipsing_bodies)
        return cls(**fields, shadow_model=shadows[value.shadow_model],
                   sun_position_method=suns[value.sun_position_method])


@dataclass(frozen=True, slots=True)
class DragSettings:
    """Spherical drag relative to co-rotating ITRF atmosphere; SI except F10.7 in sfu.

    JR supports 90..2500 km; NRLMSISE-00 gtd7d supports 0..1000 km.
    Manual flux/Kp values are constants; Kp is converted to Ap for NRLMSISE.
    Optional NRLMSISE low atmosphere blends over [90 km, 90 km + blending_range].
    Density height is exact WGS84 geodetic unless use_approximate_altitude selects
    the oblate-spheroid radial height. Geodetic latitude stays exact.
    Apparent Sun means received geocentric LT+S.
    File weather uses an explicit catalog selection and checked model lags/history.
    No atmospheric winds are modeled.
    """
    enabled: bool = False
    cd: float = 2.2
    area_mass_ratio_m2_per_kg: float = 0.02
    atmosphere: Literal["jacchia_roberts", "nrlmsise00"] = "jacchia_roberts"
    low_altitude_model: Literal["none", "nrlmsise00"] = "none"
    blending_range_m: float = 30000.0
    f107: float = 150.0
    f107_average: float = 150.0
    kp: float = 3.0
    use_apparent_sun_position: bool = False
    use_approximate_altitude: bool = False
    weather_model: str = ""
    geomagnetic_update: Literal["daily", "three_hourly", "three_hourly_interpolated", "three_hourly_cubic_spline"] = "daily"
    geomagnetic_source: Literal["ap", "kp"] = "ap"

    def __post_init__(self) -> None:
        for name in ("enabled", "use_apparent_sun_position", "use_approximate_altitude"):
            if not isinstance(getattr(self, name), bool):
                raise TypeError(f"{name} requires bool")
        self._native()

    def _native(self) -> n.DragOptions:
        models = {"jacchia_roberts": n.AtmosphereModel.JACCHIA_ROBERTS,
                  "nrlmsise00": n.AtmosphereModel.NRLMSISE00}
        lower = {"none": n.LowAltitudeDensityModel.NONE,
                 "nrlmsise00": n.LowAltitudeDensityModel.NRLMSISE00}
        if self.atmosphere not in models or self.low_altitude_model not in lower:
            raise ValueError("Unknown atmospheric density model")
        updates = {"daily": n.GeomagneticUpdate.DAILY, "three_hourly": n.GeomagneticUpdate.THREE_HOURLY,
                   "three_hourly_interpolated": n.GeomagneticUpdate.THREE_HOURLY_INTERPOLATED,
                   "three_hourly_cubic_spline": n.GeomagneticUpdate.THREE_HOURLY_CUBIC_SPLINE}
        sources = {"ap": n.GeomagneticSource.AP, "kp": n.GeomagneticSource.KP}
        if self.geomagnetic_update not in updates or self.geomagnetic_source not in sources:
            raise ValueError("Unknown geomagnetic update/source")
        if not isinstance(self.weather_model, str):
            raise TypeError("weather_model requires str")
        value = n.DragOptions()
        for name in self.__dataclass_fields__:
            if name not in ("atmosphere", "low_altitude_model", "geomagnetic_update", "geomagnetic_source"):
                setattr(value, name, getattr(self, name))
        value.atmosphere = models[self.atmosphere]
        value.low_altitude_model = lower[self.low_altitude_model]
        value.geomagnetic_update = updates[self.geomagnetic_update]
        value.geomagnetic_source = sources[self.geomagnetic_source]
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.DragOptions) -> "DragSettings":
        fields = {name: getattr(value, name) for name in cls.__dataclass_fields__
                  if name not in ("atmosphere", "low_altitude_model", "geomagnetic_update", "geomagnetic_source")}
        fields["atmosphere"] = ("jacchia_roberts" if value.atmosphere == n.AtmosphereModel.JACCHIA_ROBERTS
                                else "nrlmsise00")
        fields["low_altitude_model"] = ("none" if value.low_altitude_model == n.LowAltitudeDensityModel.NONE
                                       else "nrlmsise00")
        fields["geomagnetic_update"] = {
            n.GeomagneticUpdate.DAILY: "daily",
            n.GeomagneticUpdate.THREE_HOURLY: "three_hourly",
            n.GeomagneticUpdate.THREE_HOURLY_INTERPOLATED: "three_hourly_interpolated",
            n.GeomagneticUpdate.THREE_HOURLY_CUBIC_SPLINE: "three_hourly_cubic_spline",
        }[value.geomagnetic_update]
        fields["geomagnetic_source"] = "ap" if value.geomagnetic_source == n.GeomagneticSource.AP else "kp"
        return cls(**fields)


@dataclass(frozen=True, slots=True)
class SolidTideSettings:
    """IERS 2003 primary Sun/Moon deformation and separable permanent tide.

    'full' includes degrees 2/3 and induced degree 4. The optional time-dependent
    terms add 71 frequency corrections and printed IERS 2003 pole tide signs.
    They require the confirmed EOP/UTC providers. No amplitude filtering yet. 'none' removes the
    permanent component of zero-tide fields; 'permanent' includes it once.
    With truncation off, tides can extend beyond the static degree/order.
    """
    mode: Literal["none", "permanent", "full"] = "none"
    truncate_to_gravity: bool = True
    include_time_dependent_terms: bool = False

    def __post_init__(self) -> None:
        self._native()

    def _native(self) -> n.SolidTideOptions:
        modes = {"none": n.SolidTideMode.NONE, "permanent": n.SolidTideMode.PERMANENT,
                 "full": n.SolidTideMode.FULL}
        if self.mode not in modes:
            raise ValueError("solid tide mode must be 'none', 'permanent', or 'full'")
        if not isinstance(self.truncate_to_gravity, bool):
            raise TypeError("truncate_to_gravity must be bool")
        if not isinstance(self.include_time_dependent_terms, bool):
            raise TypeError("include_time_dependent_terms must be bool")
        value = n.SolidTideOptions()
        value.mode = modes[self.mode]
        value.truncate_to_gravity = self.truncate_to_gravity
        value.include_time_dependent_terms = self.include_time_dependent_terms
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.SolidTideOptions) -> "SolidTideSettings":
        modes = {n.SolidTideMode.NONE: "none", n.SolidTideMode.PERMANENT: "permanent",
                 n.SolidTideMode.FULL: "full"}
        return cls(modes[value.mode], value.truncate_to_gravity, value.include_time_dependent_terms)


@dataclass(frozen=True, slots=True)
class OceanTideSettings:
    """IERS 2010 ocean potential with independent degree/order and automatic pole tide.

    The catalog/DataSnapshot model is required only when enabled and degree >= 2.
    Defaults to FES2014bv1, 4x4. Includes only explicit file constituents; no minor-wave
    inference or amplitude filtering. Uses TT/UT1 and original IERS 2010 mean pole.
    """
    enabled: bool = False
    model: str = "fes2014bv1"
    degree: int = 4
    order: int = 4

    def __post_init__(self) -> None:
        self._native()

    def _native(self) -> n.OceanTideOptions:
        if type(self.enabled) is not bool:
            raise TypeError("ocean tide enabled must be bool")
        if type(self.degree) is not int or type(self.order) is not int:
            raise TypeError("ocean tide degree/order must be integers")
        value = n.OceanTideOptions()
        value.enabled, value.model = self.enabled, self.model
        value.degree, value.order = self.degree, self.order
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.OceanTideOptions) -> "OceanTideSettings":
        return cls(value.enabled, value.model, value.degree, value.order)


@dataclass(frozen=True, slots=True)
class HpopSettings:
    """File or built-in gravity with Cowell or universal-variable VOP equations.
    vop is active only for RKF78/Bulirsch-Stoer and uses gravity_mu; unsupported
    methods retain the inactive draft. Interpolation is independently configured.
    VOP requires a numerically non-radial osculating orbit and supports all conics.

    Fixed steps and adaptive Min/Max Step are 1e-6..86400 TAI seconds. The adaptive
    ceiling is not a fixed step or an accuracy guarantee; local dynamics seed the first
    trial, then error control chooses subsequent steps. Output/weather tails may be shorter.
    time_regularization integrates dS=sqrt(mu)*r**(-n)*dt, with finite 0<n<=10 and a
    positive int steps_per_orbit. It requires a nonradial bound initial ellipse and
    resets its scale after each mission impulse. n=1 corresponds to eccentric anomaly;
    n=2 to true anomaly. The initial 1.5/120 settings follow STK's documented example.
    Fixed mode uses uniform regularized steps; adaptive mode retains physical Min/Max
    Step bounds. fixed_step_seconds is inactive while regularization is enabled.
    Disabled regularization parameters are saved as valid drafts, excluded from identity.
    include_secular_variations applies the selected file's explicit epoch and coefficient
    rates per Julian year. Static files without these data cannot enable it; the flag
    remains an inactive draft for gravity degree 0. No secular defaults are inferred.
    Optional third-body gravity and spherical SRP use explicitly selected DE440s ephemerides.
    Spherical drag requires UTC/EOP and Sun ephemerides. Solid tides use IERS 2003. Noncentral gravity requires
    explicitly selected EOP covering the entire integration path, including initial epoch.
    gravity_model selects a catalog/DataSnapshot ID (empty: built-in EGM96, up to 20).
    File models support degrees through 2190 and their actual order limit. gravity_mu must match the selected file's
    GM in m^3/s^2 and is also used when converting initial Keplerian elements.
    include_relativistic_accelerations enables all three IERS 2003 terms and
    requires explicit EOP and geometric Sun position/velocity even for degree 0.
    """
    gravity_degree: int = 8
    gravity_order: int = 8
    integrator: Literal["rkf78", "rk4", "rkv89", "bulirsch_stoer", "gauss_jackson"] = "rkf78"
    relative_tolerance: float = 1e-12
    position_tolerance_m: float = 1e-3
    velocity_tolerance_mps: float = 1e-6
    min_step_seconds: float = 0.001
    max_step_seconds: float = 60
    fixed_step_seconds: float = 10
    minimum_altitude_m: float = 0
    gravity_model: str = ""
    gravity_mu: float = 3.986004415e14
    third_bodies: tuple[str, ...] = ()
    ephemeris: str = "de440s"
    body_gravity: tuple[ThirdBodySettings, ...] = ()
    srp: SrpSettings = field(default_factory=SrpSettings)
    drag: DragSettings = field(default_factory=DragSettings)
    solid_tides: SolidTideSettings = field(default_factory=SolidTideSettings)
    include_relativistic_accelerations: bool = False
    satellite_mass_kg: float = 1000.0
    ocean_tides: OceanTideSettings = field(default_factory=OceanTideSettings)
    include_secular_variations: bool = False
    correction: Literal["full", "pseudo"] = "full"
    step_control: Literal["relative_error", "fixed"] = "relative_error"
    interpolation: Literal["lagrange", "hermite", "vop"] = "hermite"
    interpolation_order: int = 7
    interpolation_mu: float = 3.986004415e14
    report_ephemeris_on_fixed_step: bool = False
    time_regularization: bool = False
    regularization_exponent: float = 1.5
    steps_per_orbit: int = 120
    vop: bool = False

    def __post_init__(self) -> None:
        if type(self.vop) is not bool:
            raise TypeError("vop must be bool")
        if type(self.time_regularization) is not bool:
            raise TypeError("time_regularization must be bool")
        if type(self.steps_per_orbit) is not int:
            raise TypeError("steps_per_orbit must be int")
        if type(self.report_ephemeris_on_fixed_step) is not bool:
            raise TypeError("report_ephemeris_on_fixed_step must be bool")
        if type(self.interpolation_order) is not int:
            raise TypeError("interpolation_order must be int")
        if type(self.include_secular_variations) is not bool:
            raise TypeError("include_secular_variations must be bool")
        if type(self.include_relativistic_accelerations) is not bool:
            raise TypeError("include_relativistic_accelerations must be bool")
        require(self.srp, SrpSettings, "srp")
        require(self.drag, DragSettings, "drag")
        require(self.solid_tides, SolidTideSettings, "solid_tides")._native()
        require(self.ocean_tides, OceanTideSettings, "ocean_tides")._native()
        bodies = tuple(self.third_bodies)
        if any(body not in THIRD_BODY_NAMES for body in bodies) or len(set(bodies)) != len(bodies):
            raise ValueError("third_bodies accepts solar-system body names, each at most once")
        settings = tuple(self.body_gravity)
        if any(not isinstance(v, ThirdBodySettings) for v in settings):
            raise TypeError("body_gravity requires ThirdBodySettings values")
        if len({v.body for v in settings}) != len(settings):
            raise ValueError("Duplicate third-body gravity settings")
        for item in settings:
            item._native()
        object.__setattr__(self, "third_bodies", tuple(b for b in THIRD_BODY_NAMES if b in bodies))
        active = (v for v in settings if v.source != "jpl_de" or v.mu is not None or v.cb is not None)
        object.__setattr__(self, "body_gravity", tuple(sorted(active, key=lambda v: THIRD_BODY_NAMES.index(v.body))))

    @property
    def drag_area_m2(self) -> float:
        """Compatible spherical drag area: A/m multiplied by the shared mass."""
        return self._native().drag_area_m2

    @property
    def srp_area_m2(self) -> float:
        """Compatible spherical SRP area; changing mass preserves the given A/m."""
        return self._native().srp_area_m2

    def _native(self) -> n.HpopOptions:
        value = n.HpopOptions()
        if self.integrator not in ("rkf78", "rk4", "rkv89", "bulirsch_stoer", "gauss_jackson"):
            raise ValueError("integrator must be 'rkf78', 'rk4', 'rkv89', 'bulirsch_stoer' or 'gauss_jackson'")
        for name in self.__dataclass_fields__:
            if name not in ("integrator", "correction", "step_control", "interpolation", "interpolation_order", "interpolation_mu", "third_bodies", "ephemeris", "body_gravity", "srp", "drag", "solid_tides", "ocean_tides"):
                setattr(value, name, getattr(self, name))
        if self.interpolation not in ("lagrange", "hermite", "vop"):
            raise ValueError("interpolation must be lagrange, hermite or vop")
        interpolation = n.OrbitInterpolationOptions()
        interpolation.method = {"lagrange": n.OrbitInterpolationMethod.LAGRANGE,
                                "hermite": n.OrbitInterpolationMethod.HERMITE,
                                "vop": n.OrbitInterpolationMethod.VOP}[self.interpolation]
        interpolation.order = self.interpolation_order
        interpolation.vop_mu = self.interpolation_mu
        value.interpolation = interpolation
        if self.step_control not in ("relative_error", "fixed"):
            raise ValueError("step_control must be relative_error or fixed")
        value.step_control = {"relative_error": n.HpopStepControl.RELATIVE_ERROR,
                              "fixed": n.HpopStepControl.FIXED}[self.step_control]
        if self.correction not in ("full", "pseudo"):
            raise ValueError("correction must be full or pseudo")
        value.correction = {"full": n.HpopCorrection.FULL, "pseudo": n.HpopCorrection.PSEUDO}[self.correction]
        rows = list(value.third_bodies)
        for item in self.body_gravity:
            rows[THIRD_BODY_NAMES.index(item.body)] = item._native()
        for name, row in zip(THIRD_BODY_NAMES, rows):
            row.enabled = name in self.third_bodies
        value.third_bodies = rows
        value.ephemeris_model = self.ephemeris
        value.srp = self.srp._native()
        value.drag = self.drag._native()
        value.solid_tides = self.solid_tides._native()
        value.ocean_tides = self.ocean_tides._native()
        value.integrator = {"rkf78": n.HpopIntegrator.RKF78, "rk4": n.HpopIntegrator.RK4,
                            "rkv89": n.HpopIntegrator.RKV89,
                            "bulirsch_stoer": n.HpopIntegrator.BULIRSCH_STOER,
                            "gauss_jackson": n.HpopIntegrator.GAUSS_JACKSON}[self.integrator]
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.HpopOptions) -> "HpopSettings":
        fields = {name: getattr(value, name) for name in cls.__dataclass_fields__
                  if name not in ("integrator", "correction", "step_control", "interpolation", "interpolation_order", "interpolation_mu", "third_bodies", "ephemeris", "body_gravity", "srp", "drag", "solid_tides", "ocean_tides")}
        fields["interpolation"] = {n.OrbitInterpolationMethod.LAGRANGE: "lagrange",
                                   n.OrbitInterpolationMethod.HERMITE: "hermite",
                                   n.OrbitInterpolationMethod.VOP: "vop"}[value.interpolation.method]
        fields["interpolation_order"] = value.interpolation.order
        fields["interpolation_mu"] = value.interpolation.vop_mu
        rows = value.third_bodies
        fields["third_bodies"] = tuple(b for b, row in zip(THIRD_BODY_NAMES, rows) if row.enabled)
        sources = {n.ThirdBodyGravitySource.JPL_DE: "jpl_de",
                   n.ThirdBodyGravitySource.CB_FILE: "cb_file",
                   n.ThirdBodyGravitySource.USER_SPECIFIED: "user_specified"}
        fields["body_gravity"] = tuple(ThirdBodySettings(b, sources[row.source], row.user_mu, row.cb)
                                      for b, row in zip(THIRD_BODY_NAMES, rows)
                                      if row.source != n.ThirdBodyGravitySource.JPL_DE or
                                      row.user_mu is not None or row.cb is not None)
        fields["ephemeris"] = value.ephemeris_model
        fields["srp"] = SrpSettings._read(value.srp)
        fields["drag"] = DragSettings._read(value.drag)
        fields["solid_tides"] = SolidTideSettings._read(value.solid_tides)
        fields["ocean_tides"] = OceanTideSettings._read(value.ocean_tides)
        integrators = {n.HpopIntegrator.RKF78: "rkf78", n.HpopIntegrator.RK4: "rk4",
                       n.HpopIntegrator.RKV89: "rkv89",
                       n.HpopIntegrator.BULIRSCH_STOER: "bulirsch_stoer",
                       n.HpopIntegrator.GAUSS_JACKSON: "gauss_jackson"}
        return cls(**fields, integrator=integrators[value.integrator],
                   correction={n.HpopCorrection.FULL: "full", n.HpopCorrection.PSEUDO: "pseudo"}[value.correction],
                   step_control={n.HpopStepControl.RELATIVE_ERROR: "relative_error",
                                 n.HpopStepControl.FIXED: "fixed"}[value.step_control])


@dataclass(frozen=True, slots=True)
class Orbit:
    """Canonical configuration returned by orbit constructors and handle reads."""
    initial: Cartesian
    propagator: n.OrbitPropagator = n.OrbitPropagator.TWO_BODY
    input_frame: n.FrameId = n.FrameId.GCRF
    tle: n.Tle = n.Tle("", "")
    step_seconds: int = 60
    hpop: HpopSettings = HpopSettings()
    mission: missions.Plan = missions.Plan()

    def _native(self) -> n.OrbitInput:
        value = n.OrbitInput()
        value.initial = require(self.initial, Cartesian, "initial")._native()
        value.propagator, value.input_frame = self.propagator, self.input_frame
        value.tle, value.step_seconds = self.tle, self.step_seconds
        if self.propagator == n.OrbitPropagator.MISSION:
            value.mission = require(self.mission, missions.Plan, "mission")._native()
            value.mission.end_epoch(self.initial.epoch)
        if self.propagator == n.OrbitPropagator.HPOP or (
            self.propagator == n.OrbitPropagator.MISSION and self.mission.propagation == "hpop"
        ):
            value.hpop = require(self.hpop, HpopSettings, "hpop")._native()
        return value

    @classmethod
    def from_input(cls, value: n.OrbitInput) -> "Orbit":
        """Copy a low-level SI input, without its object ID or analysis bounds."""
        require(value, n.OrbitInput, "orbit")
        return cls(Cartesian._read(value.initial), value.propagator, value.input_frame,
                   n.Tle(value.tle.line1, value.tle.line2), value.step_seconds,
                   HpopSettings._read(value.hpop), missions.Plan._read(value.mission))


def TwoBody(*, initial: Keplerian | Cartesian | n.CartesianState) -> Orbit:
    if isinstance(initial, Keplerian):
        initial = initial._cartesian()
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    return Orbit(require(initial, Cartesian, "initial"))


def Mission(*, initial: Keplerian | Cartesian | n.CartesianState,
            segments: tuple[missions.Coast | missions.Impulse, ...] | list[missions.Coast | missions.Impulse],
            propagation: Literal["two_body", "hpop"] = "two_body",
            hpop: HpopSettings = HpopSettings()) -> Orbit:
    """Forward coast/impulse sequence covering the whole scenario; exact burns are post-burn.

    Example: segments=[Coast(u.s(600)), Impulse((100, 0, 0)), Coast(u.s(6600))].
    Impulses use SI m/s, default VNC axes. No finite thrust or automatic targeting.
    """
    plan = missions.Plan(tuple(segments), propagation)
    if isinstance(initial, Keplerian):
        initial = initial._cartesian(mu=hpop.gravity_mu if propagation == "hpop" else None)
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    orbit = Orbit(require(initial, Cartesian, "initial"), n.OrbitPropagator.MISSION,
                  hpop=hpop, mission=plan)
    orbit._native()
    return orbit


def J2(*, initial: n.J2MeanElements) -> Orbit:
    return Orbit(Cartesian._read(n.j2_mean_ellipse_state(require(initial, n.J2MeanElements, "initial"))),
                 n.OrbitPropagator.J2_SECULAR)


def J4(*, initial: n.J4MeanElements) -> Orbit:
    return Orbit(Cartesian._read(n.j4_mean_ellipse_state(require(initial, n.J4MeanElements, "initial"))),
                 n.OrbitPropagator.J4_SECULAR)


def Sgp4(*, tle: n.Tle) -> Orbit:
    value = n.OrbitInput()
    value.propagator, value.tle = n.OrbitPropagator.SGP4, require(tle, n.Tle, "tle")
    return Orbit.from_input(value)


def Hpop(*, initial: Keplerian | Cartesian | n.CartesianState,
         gravity_model: str = "", gravity_mu: float = 3.986004415e14,
         gravity_degree: int = 8, gravity_order: int | None = None,
         integrator: Literal["rkf78", "rk4", "rkv89", "bulirsch_stoer", "gauss_jackson"] = "rkf78", max_step_seconds: float = 60,
         min_step_seconds: float = 0.001, fixed_step_seconds: float = 10,
         relative_tolerance: float = 1e-12, position_tolerance_m: float = 1e-3,
         velocity_tolerance_mps: float = 1e-6, minimum_altitude_m: float = 0,
         third_bodies: tuple[str, ...] = (), ephemeris: str = "de440s",
         sun_mu: float | None = None, moon_mu: float | None = None,
         body_gravity: tuple[ThirdBodySettings, ...] = (), srp: SrpSettings | None = None,
         drag: DragSettings | None = None, solid_tides: SolidTideSettings | None = None,
         include_relativistic_accelerations: bool = False,
         satellite_mass_kg: float = 1000.0, ocean_tides: OceanTideSettings | None = None,
         include_secular_variations: bool = False,
         correction: Literal["full", "pseudo"] = "full",
         step_control: Literal["relative_error", "fixed"] = "relative_error",
         interpolation: Literal["lagrange", "hermite", "vop"] = "hermite", interpolation_order: int = 7,
         interpolation_mu: float = 3.986004415e14,
         report_ephemeris_on_fixed_step: bool = False,
         time_regularization: bool = False, regularization_exponent: float = 1.5,
         steps_per_orbit: int = 120, vop: bool = False) -> Orbit:
    """HPOP orbit with gravity, tides, third bodies, drag, spherical SRP and numerical integration.
    vop selects universal-variable variation of parameters for RKF78/Bulirsch-Stoer.
    Other methods retain the inactive draft; VOP integration uses gravity_mu, independently
    of interpolation and interpolation_mu. Disabling vop uses Cartesian Cowell equations.
    step_control selects relative_error or fixed; RK4/Gauss-Jackson always use fixed steps.
    Fixed Bulirsch-Stoer uses eighteenth-order extrapolation without error control.
    Gauss-Jackson correction selects the post-convergence full or two-body-only force update.
    Time regularization uses a finite exponent 0<n<=10 and positive steps_per_orbit;
    see HpopSettings for the bound-ellipse requirement and physical step bounds.

    Built-in EGM96 supports up to degree 20; installed gravity files up to 2190 (EGM2008 order limit 2159).
    Satellite mass is in kg (finite and positive); spherical forces retain their explicit
    A/m ratios. Mass only determines their compatible areas, without changing acceleration.
    third_bodies=("sun", "moon") requires explicitly supplied DE440s ephemerides.
    sun_mu and moon_mu are optional overrides in m^3/s^2; None uses DE440 constants.

    Scientific queries use Lagrange, Hermite or VOP interpolation (order 0..29; Hermite odd 3..29).
    VOP uses independent interpolation_mu in m^3/s^2 to transport osculating states
    to the query epoch; it does not change the force model or the integration equations.
    Fixed ephemeris reporting uses Orbit.step_seconds and truncates integration steps
    to those output times. Query sampling on analysis.states() remains independent.
    Omitted gravity_order uses gravity_degree; order=0 retains only zonal terms.
    """
    if isinstance(initial, Keplerian):
        initial = initial._cartesian(mu=gravity_mu)
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    overrides = tuple(ThirdBodySettings(body, "user_specified", mu)
                      for body, mu in (("sun", sun_mu), ("moon", moon_mu)) if mu is not None)
    settings = HpopSettings(
        gravity_degree, gravity_degree if gravity_order is None else gravity_order, integrator,
        relative_tolerance, position_tolerance_m, velocity_tolerance_mps,
        min_step_seconds, max_step_seconds, fixed_step_seconds, minimum_altitude_m,
        gravity_model.lower(), gravity_mu, third_bodies, ephemeris, tuple(body_gravity) + overrides,
        SrpSettings() if srp is None else require(srp, SrpSettings, "srp"),
        DragSettings() if drag is None else require(drag, DragSettings, "drag"),
        SolidTideSettings() if solid_tides is None else require(solid_tides, SolidTideSettings, "solid_tides"),
        include_relativistic_accelerations, satellite_mass_kg,
        OceanTideSettings() if ocean_tides is None else require(ocean_tides, OceanTideSettings, "ocean_tides"),
        include_secular_variations, correction, step_control, interpolation, interpolation_order,
        interpolation_mu, report_ephemeris_on_fixed_step, time_regularization, regularization_exponent,
        steps_per_orbit, vop)
    settings._native()  # Fail at construction, before changing a project.
    return Orbit(require(initial, Cartesian, "initial"), n.OrbitPropagator.HPOP, hpop=settings)
