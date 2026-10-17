"""Weather files use shared native sampling, persistence and immutable analysis resources."""
from datetime import date, timedelta
from dataclasses import replace
from pathlib import Path
import hashlib
import csv
import io
import math
import tempfile
import unittest
import orbitforge as of
from orbitforge import units as u

ROOT = Path(__file__).resolve().parents[3]
HEADER = ('DATE,BSRN,ND,KP1,KP2,KP3,KP4,KP5,KP6,KP7,KP8,KP_SUM,'
          'AP1,AP2,AP3,AP4,AP5,AP6,AP7,AP8,AP_AVG,CP,C9,ISN,F10.7_OBS,F10.7_ADJ,'
          'F10.7_DATA_TYPE,F10.7_OBS_CENTER81,F10.7_OBS_LAST81,F10.7_ADJ_CENTER81,F10.7_ADJ_LAST81\n')

def weather(flux=150):
    return HEADER + ''.join(
        f'{date(2016,12,24)+timedelta(days=i)},1,1,30,30,30,30,30,30,30,30,240,'
        f'15,15,15,15,15,15,15,15,15,0,0,0,{flux},500,OBS,150,600,500,600\n'
        for i in range(14))

class WeatherTests(unittest.TestCase):
    @unittest.skipUnless(of.time_frames_available(), 'requires ERFA')
    def test_sampling_hash_catalog_and_native_density(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'weather.csv'; path.write_text(weather(),encoding='ascii')
            parsed=of.load_space_weather(path)
            self.assertEqual(parsed.info.sha256,hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(parsed.info.format,'celestrak-csv')
            clock=of.make_builtin_time_provider()
            at=clock.from_utc(of.UtcDateTime(2016,12,30,12,0,0))
            sample=parsed.sample(at,clock,of.AtmosphereModel.NRLMSISE00,
                                 of.GeomagneticUpdate.THREE_HOURLY,of.GeomagneticSource.AP)
            self.assertEqual(sample.ap_history,[15]*7)
            self.assertEqual((sample.f107,sample.f107_average,sample.ap),(150,150,15))
            with self.assertRaises(of.SpaceWeatherCoverageError):
                parsed.sample(clock.from_utc(of.UtcDateTime(2016,12,24)),clock,
                              of.AtmosphereModel.NRLMSISE00,of.GeomagneticUpdate.DAILY,
                              of.GeomagneticSource.AP)
            with self.assertRaises(AttributeError): sample.f107=20
            options=of.DragOptions(); options.enabled=True; options.weather_model='celestrak'
            options.atmosphere=of.AtmosphereModel.NRLMSISE00
            options.geomagnetic_update=of.GeomagneticUpdate.THREE_HOURLY
            result=of.evaluate_drag((6778137,0,0),(0,7000,0),(1.5e11,0,0),
                                    clock.to_utc(at),options,sample)
            direct=of.AtmosphereInput(); direct.utc=clock.to_utc(at)
            direct.geodetic=(0,0,400000); direct.ap_history=[15]*7
            self.assertEqual(result.density_kg_m3,
                             of.evaluate_atmosphere(of.AtmosphereModel.NRLMSISE00,direct).density_kg_m3)
            with self.assertRaises(ValueError):
                of.evaluate_drag((6778137,0,0),(0,7000,0),(1.5e11,0,0),clock.to_utc(at),options)
            parsed.validate_interval(at,at,clock,of.AtmosphereModel.NRLMSISE00,
                                     of.GeomagneticUpdate.THREE_HOURLY,of.GeomagneticSource.KP)
            if of.data_catalog_available():
                catalog=of.DataCatalog(Path(folder)/'catalog')
                receipt=catalog.install_space_weather(path)
                self.assertEqual(receipt.info.sha256,parsed.info.sha256)
                path.write_text('broken')
                with self.assertRaises(RuntimeError): catalog.install_space_weather(path)
                self.assertEqual(catalog.weather_sources[0].info.sha256,receipt.info.sha256)
                del catalog
            self.assertEqual(parsed.sample(at,clock,of.AtmosphereModel.NRLMSISE00,
                             of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP).f107,150)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), 'requires CSPICE/ERFA')
    def test_file_drag_reopens_and_keeps_inactive_manual_drafts_out_of_identity(self):
        clock=of.make_builtin_time_provider(); first=clock.from_utc(of.UtcDateTime(2016,12,30,12,0,0))
        last=of.Epoch(first.tai_nanoseconds+600*10**9)
        eop=ROOT/'data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json'
        spice=ROOT/'data/samples/spice/de440s_2016_leap/manifest.json'
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); file=root/'weather.csv'; file.write_text(weather())
            data=of.DataSnapshot.load(eop_manifest=eop).with_ephemeris('de440s',spice,root/'runtime')
            data=data.with_space_weather('celestrak',file)
            project=of.Project('Weather',first,last)
            drag=of.orbits.DragSettings(enabled=True,atmosphere='nrlmsise00',weather_model='celestrak',
                                       geomagnetic_update='three_hourly',geomagnetic_source='ap')
            orbit=of.orbits.Hpop(initial=of.orbits.Cartesian(first,(6778137,0,0),(0,6300,4300)),
                                gravity_degree=0,drag=drag)
            sat=project.objects.satellite('Sat',orbit=orbit)
            analysis=project.prepare(data=data); before=analysis.states(sat,step=u.s(60))
            saved=root/'weather.orbitforge'; project.save(saved,analysis=analysis)
            restored=of.Project.open(saved,data=data)
            self.assertEqual(restored.objects[sat.id].orbit.hpop.drag,drag)
            sat.orbit=replace(orbit,hpop=replace(orbit.hpop,drag=replace(drag,f107=999,kp=8)))
            same=project.prepare(data=data).states(sat,step=u.s(60))
            self.assertEqual(same.input_sha256,before.input_sha256)
            self.assertEqual(same.position.values,before.position.values)
            file.write_text(weather(250))
            changed=data.with_space_weather('celestrak',file)
            with self.assertRaises(of.DataChangedError): restored.prepare(data=changed)
            self.assertEqual(analysis.states(sat,step=u.s(60)).position.values,before.position.values)
            del restored,analysis,data,changed

    def test_invalid_choices_and_disabled_file_drafts(self):
        for kw in ({'weather_model':'../file'}, {'geomagnetic_update':'hourly'}, {'geomagnetic_source':'other'}):
            with self.assertRaises((ValueError,TypeError)): of.orbits.DragSettings(**kw)
        drag=of.orbits.DragSettings(weather_model='missing',geomagnetic_update='three_hourly_cubic_spline',geomagnetic_source='kp')
        project=of.Project('Draft',of.Epoch(0),of.Epoch(600*10**9))
        orbit=of.orbits.Hpop(initial=of.orbits.Cartesian(of.Epoch(0),(7000000,0,0),(0,7500,0)),gravity_degree=0,drag=drag)
        sat=project.objects.satellite('Sat',orbit=orbit)
        project.prepare()
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'draft.orbitforge'; project.save(path)
            self.assertEqual(of.Project.open(path).objects[sat.id].orbit.hpop.drag,drag)

    @unittest.skipUnless(of.ephemeris_available() and of.time_frames_available(), 'requires CSPICE/ERFA')
    def test_geomagnetic_jump_splits_steps_and_converges_forward_and_backward(self):
        clock=of.make_builtin_time_provider()
        center=clock.from_utc(of.UtcDateTime(2016,12,30,21,0,0))
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); file=root/'storm.csv'
            rows=list(csv.reader(io.StringIO(weather())))
            for row in rows[1:]:
                if row[0]=='2016-12-30': row[19]='400'; row[20]='63'
            with file.open('w',newline='') as output: csv.writer(output).writerows(rows)
            data=of.DataSnapshot.load(eop_manifest=ROOT/'data/samples/eop/iers_c04_2016_12_28_2017_01_04/manifest.json')
            data=data.with_ephemeris('de440s',ROOT/'data/samples/spice/de440s_2016_leap/manifest.json',root/'runtime').with_space_weather('celestrak',file)
            for direction in (1,-1):
                initial=of.Epoch(center.tai_nanoseconds-direction*13*10**9)
                target=of.Epoch(initial.tai_nanoseconds+direction*300*10**9)
                first,last=(initial,target) if direction==1 else (target,initial)
                positions=[]
                methods = [(name,step,"full") for name,step in
                           (("rkf78",60),("rkf78",7),("rkf78",1),("rkv89",60),("rkv89",7),
                            ("bulirsch_stoer",60),("bulirsch_stoer",7))]
                methods += [("gauss_jackson",1,"full"),("gauss_jackson",.7,"pseudo"),("rk4",.5,"full")]
                methods = [(name, step, correction, "relative_error") for name, step, correction in methods]
                methods[-1:-1] = [(name, 7, "full", "fixed") for name in ("rkf78", "rkv89", "bulirsch_stoer")]
                methods[-1:-1] = [(name, 86400, "full", "relative_error")
                                  for name in ("rkf78", "rkv89", "bulirsch_stoer")]
                for integrator,step,correction,control in methods:
                    project=of.Project('Storm',first,last)
                    orbit=of.orbits.Hpop(initial=of.orbits.Cartesian(initial,(6628137,0,0),(0,7750,0)),
                        gravity_degree=0,integrator=integrator,correction=correction,step_control=control,max_step_seconds=step,fixed_step_seconds=step,relative_tolerance=1e-13,
                        position_tolerance_m=1e-5,velocity_tolerance_mps=1e-8,
                        drag=of.orbits.DragSettings(enabled=True,atmosphere='nrlmsise00',weather_model='celestrak',geomagnetic_update='three_hourly'))
                    sat=project.objects.satellite('Sat',orbit=orbit)
                    positions.append(project.prepare(data=data).states(sat,step=u.s(300)).position.values[-1 if direction==1 else 0])
                # 0.1 mm against independent fixed RK4 stepping across a 15->400 Ap jump.
                # This is a trajectory convergence bound, not a physical atmosphere error.
                for position in positions[:-1]: self.assertLess(math.dist(position,positions[-1]),1e-4)
            del data


class NaturalSplineTests(unittest.TestCase):
    @unittest.skipUnless(of.time_frames_available(), 'requires ERFA')
    def test_constant_spline_and_native_nrl_history(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'weather.csv'; path.write_text(weather(150))
            data=of.load_space_weather(path)
            time=of.make_builtin_time_provider()
            at=time.from_utc(of.UtcDateTime(2016,12,31,23,59,60))
            for source in (of.GeomagneticSource.AP,of.GeomagneticSource.KP):
                jr=data.sample(at,time,of.AtmosphereModel.JACCHIA_ROBERTS,
                               of.GeomagneticUpdate.THREE_HOURLY_CUBIC_SPLINE,source)
                self.assertAlmostEqual(jr.kp,3)
                self.assertAlmostEqual(jr.ap,15)
                a=data.sample(at,time,of.AtmosphereModel.NRLMSISE00,
                              of.GeomagneticUpdate.THREE_HOURLY,source)
                b=data.sample(at,time,of.AtmosphereModel.NRLMSISE00,
                              of.GeomagneticUpdate.THREE_HOURLY_CUBIC_SPLINE,source)
                self.assertEqual(a.ap_history,b.ap_history)


class FxmWeatherTests(unittest.TestCase):
    @unittest.skipUnless(of.time_frames_available(), 'requires ERFA')
    def test_fxm_sampling_callback_matches_independent_reference(self):
        source=ROOT/'tests/reference/propagators'
        parsed=of.load_space_weather(source/'weather_fxm.fxm')
        self.assertEqual(parsed.info.format,'stk-flux-geomag')
        self.assertTrue(parsed.info.solar_flux_adjusted)
        self.assertEqual(parsed.info.sha256,hashlib.sha256((source/'weather_fxm.fxm').read_bytes()).hexdigest())
        with self.assertRaises(AttributeError): parsed.info.solar_flux_adjusted=False
        time=of.make_builtin_time_provider()
        def distance(at):
            utc=time.to_utc(at)
            self.assertEqual((utc.hour,utc.minute,utc.second,utc.nanosecond),(20,0,0,0))
            day=utc.day-24 if utc.month==12 else utc.day+7
            return 149597870700.0*(.97+.005*day)
        count=0
        for line in (source/'weather_fxm.txt').read_text().splitlines():
            if not line.startswith('S '): continue
            _,model,ns,flux,average,ap,kp=line.split()
            actual=parsed.sample(of.Epoch(int(ns)),time,
                of.AtmosphereModel.JACCHIA_ROBERTS if model=='0' else of.AtmosphereModel.NRLMSISE00,
                of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP,sun_distance_m=distance)
            self.assertAlmostEqual(actual.f107,float(flux),delta=3e-11)
            self.assertAlmostEqual(actual.f107_average,float(average),delta=3e-11)
            self.assertEqual(actual.ap,float(ap))
            self.assertAlmostEqual(actual.kp,float(kp),delta=1e-14)
            count+=1
        self.assertEqual(count,74)
        first=time.from_utc(of.UtcDateTime(2016,12,29,20,0,0))
        last=time.from_utc(of.UtcDateTime(2017,1,3,20,0,0))
        parsed.validate_interval(first,last,time,of.AtmosphereModel.NRLMSISE00,
                                 of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP,distance)
        with self.assertRaises(of.SpaceWeatherCoverageError):
            parsed.sample(first,time,of.AtmosphereModel.NRLMSISE00,
                          of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP)
        def missing(at):
            if time.to_utc(at).day==30: raise ValueError('missing Sun distance')
            return distance(at)
        with self.assertRaisesRegex(ValueError,'missing Sun distance'):
            parsed.validate_interval(first,last,time,of.AtmosphereModel.NRLMSISE00,
                                     of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP,missing)
        for invalid in (0,-1,float('nan'),float('inf')):
            with self.assertRaises(of.SpaceWeatherCoverageError):
                parsed.sample(first,time,of.AtmosphereModel.NRLMSISE00,
                              of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP,lambda _:invalid)

    @unittest.skipUnless(of.data_catalog_available(), 'requires data catalog')
    def test_fxm_catalog_reopens_adjusted_metadata_and_retains_pinned_resource(self):
        with tempfile.TemporaryDirectory() as folder:
            catalog=of.DataCatalog(Path(folder)/'catalog')
            try:
                fixture=ROOT/'tests/reference/propagators/weather_fxm.fxm'
                installed=catalog.install_space_weather(fixture,'fxm')
                old=of.load_space_weather(installed.file)
                self.assertTrue(installed.info.solar_flux_adjusted)
                reopened=of.DataCatalog(Path(folder)/'catalog')
                try:
                    self.assertTrue(reopened.weather_sources[0].info.solar_flux_adjusted)
                finally:
                    del reopened
                broken=Path(folder)/'broken.fxm';broken.write_text('BEGIN OBSERVED\n')
                with self.assertRaises(RuntimeError): catalog.install_space_weather(broken,'fxm')
                self.assertEqual(old.info.sha256,catalog.weather_sources[0].info.sha256)
            finally:
                del catalog


class MeanInterpolationTests(unittest.TestCase):
    @unittest.skipUnless(of.time_frames_available(), 'requires ERFA')
    def test_csv_api_matches_independent_scipy_and_every_bin_integral(self):
        reference=(ROOT/'tests/reference/propagators/space_weather_mean.txt').read_text().splitlines()
        rows=list(csv.reader(io.StringIO(weather())))
        # Add only flux guards; geomagnetic cells outside the reference remain missing.
        for day in (23,22):
            guard=rows[1].copy(); guard[0]=f'2016-12-{day}'
            guard[3:11]=['']*8; guard[12:20]=['']*8
            rows.insert(1,guard)
        for line in reference:
            if not line.startswith('D '): continue
            _,d,slot,ap,kp=line.split(); row=rows[int(d)+3]; slot=int(slot)
            row[12+slot]='' if float(ap)<0 else ap
            row[3+slot]='' if float(kp)<0 else str(round(float(kp)*10))
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'mean.csv'
            with path.open('w',newline='',encoding='ascii') as output:
                csv.writer(output).writerows(rows)
            parsed=of.load_space_weather(path); clock=of.make_builtin_time_provider()
            def value(source,ns):
                sample=parsed.sample(of.Epoch(int(ns)),clock,of.AtmosphereModel.JACCHIA_ROBERTS,
                    of.GeomagneticUpdate.THREE_HOURLY_INTERPOLATED,
                    of.GeomagneticSource.AP if source=='0' else of.GeomagneticSource.KP)
                return sample.ap if source=='0' else sample.kp
            points=windows=0
            for line in reference:
                fields=line.split()
                if fields[0]=='C':
                    self.assertAlmostEqual(value(fields[1],fields[2]),float(fields[3]),delta=1e-10)
                    points+=1
                elif fields[0]=='Q':
                    mean=sum(value(fields[1],fields[i])*float(fields[i+1]) for i in range(5,15,2))
                    self.assertAlmostEqual(mean,float(fields[4]),delta=1e-10)
                    windows+=1
            self.assertEqual((points,windows),(1314,218))

    def test_high_level_interpolated_draft_round_trips(self):
        epoch=of.Epoch(0)
        with tempfile.TemporaryDirectory() as folder:
            project=of.Project('Mean interpolation',epoch,of.Epoch(10_000_000_000))
            drag=of.orbits.DragSettings(weather_model='celestrak',geomagnetic_update='three_hourly_interpolated',geomagnetic_source='kp')
            sat=project.objects.satellite('Sat',orbit=of.orbits.Hpop(
                initial=of.orbits.Cartesian(epoch,(7000000,0,0),(0,7500,0)),gravity_degree=0,drag=drag))
            path=Path(folder)/'mean.orbitforge'; project.save(path)
            reopened=of.Project.open(path)
            self.assertEqual(reopened.objects[sat.id].orbit.hpop.drag,drag)
            reopened.save(path,overwrite=True)
            self.assertEqual(of.Project.open(path).objects[sat.id].orbit.hpop.drag.geomagnetic_update,'three_hourly_interpolated')
        with self.assertRaises(ValueError):
            of.orbits.DragSettings(geomagnetic_update='unknown')


class MonthlyWeatherTests(unittest.TestCase):
    @unittest.skipUnless(of.time_frames_available(), 'requires ERFA')
    def test_monthly_sampling_and_history_match_independent_reference(self):
        root=ROOT/'tests/reference/propagators'
        weather=[of.load_space_weather(root/'weather_cssi.dat'),of.load_space_weather(root/'weather_monthly.csv')]
        self.assertTrue(weather[0].info.cssi_predictions)
        self.assertFalse(weather[1].info.cssi_predictions)
        with self.assertRaises(AttributeError): weather[0].info.cssi_predictions=False
        clock=of.make_builtin_time_provider();count=0
        updates=(of.GeomagneticUpdate.DAILY,of.GeomagneticUpdate.THREE_HOURLY,
                 of.GeomagneticUpdate.THREE_HOURLY_INTERPOLATED,of.GeomagneticUpdate.THREE_HOURLY_CUBIC_SPLINE)
        for line in (root/'weather_monthly.txt').read_text().splitlines():
            if not line.startswith('S '):continue
            row=line.split();dataset,model,update,source,ns=map(int,row[1:6])
            sample=weather[dataset].sample(of.Epoch(ns),clock,
                of.AtmosphereModel.JACCHIA_ROBERTS if model==0 else of.AtmosphereModel.NRLMSISE00,
                updates[update],of.GeomagneticSource.AP if source==0 else of.GeomagneticSource.KP)
            for actual,expected in zip((sample.f107,sample.f107_average,sample.kp,sample.ap),map(float,row[6:10])):
                self.assertAlmostEqual(actual,expected,delta=3e-10)
            if int(row[10]):
                for actual,expected in zip(sample.ap_history,map(float,row[11:])):
                    self.assertAlmostEqual(actual,expected,delta=3e-10)
            else:self.assertIsNone(sample.ap_history)
            count+=1
        self.assertEqual(count,1056)
        boundary=clock.from_utc(of.UtcDateTime(2015,12,25,20,0,0))
        with self.assertRaises(of.SpaceWeatherCoverageError):
            weather[0].sample(of.Epoch(boundary.tai_nanoseconds-1),clock,of.AtmosphereModel.NRLMSISE00,
                              of.GeomagneticUpdate.DAILY,of.GeomagneticSource.AP)

    @unittest.skipUnless(of.data_catalog_available(), 'requires data catalog')
    def test_cssi_catalog_reopens_metadata_and_keeps_raw_bytes(self):
        fixture=ROOT/'tests/reference/propagators/weather_cssi.dat'
        with tempfile.TemporaryDirectory() as folder:
            catalog=of.DataCatalog(Path(folder)/'catalog')
            try:
                installed=catalog.install_space_weather(fixture,'cssi')
                self.assertTrue(installed.info.cssi_predictions)
                self.assertEqual(Path(installed.file).read_bytes(),fixture.read_bytes())
                reopened=of.DataCatalog(Path(folder)/'catalog')
                try:self.assertTrue(reopened.weather_sources[0].info.cssi_predictions)
                finally:del reopened
                broken=Path(folder)/'bad.dat';broken.write_text('2015 11 2017 4 2015 10 18\n')
                with self.assertRaises(RuntimeError):catalog.install_space_weather(broken,'cssi')
                self.assertEqual(catalog.weather_sources[0].info.sha256,installed.info.sha256)
            finally:del catalog
