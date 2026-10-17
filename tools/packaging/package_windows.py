"""Build a relocatable Windows desktop folder with runtime DLLs, plugins and offline data."""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def find_crt(vc_root, toolset):
    """VS may ship its default redistributable under a different servicing patch."""
    redist = vc_root / 'Redist/MSVC'
    versions = [toolset]
    default = vc_root / 'Auxiliary/Build/Microsoft.VCRedistVersion.default.txt'
    if default.is_file():
        version = default.read_text(encoding='utf-8-sig').strip()
        if re.fullmatch(r'\d+\.\d+\.\d+(?:\.\d+)?', version):
            if tuple(map(int, version.split('.')[:2])) >= tuple(map(int, toolset.split('.')[:2])):
                versions.append(version)
    for version in versions:
        roots = list((redist / version / 'x64').glob('Microsoft.VC*.CRT'))
        if len(roots) == 1:
            return roots[0]
    raise RuntimeError('Cannot locate a compatible app-local MSVC runtime from this Visual Studio installation')


def validate_output_locations(output, reports):
    """Keep local diagnostics outside the distributable directory."""
    output, reports = output.resolve(), reports.resolve()
    if output.is_relative_to(reports) or reports.is_relative_to(output):
        raise ValueError('Package output and local reports must be separate, non-nested directories')


def write_package_manifest(output, reports, executable, dll_sources):
    validate_output_locations(output, reports)
    entries = [{'path': path.relative_to(output).as_posix(), 'bytes': path.stat().st_size,
                'sha256': digest(path)} for path in sorted(output.rglob('*')) if path.is_file()]
    manifest = {'schema': 1, 'created_utc': datetime.now(timezone.utc).isoformat(),
                'executable_sha256': digest(executable),
                'dll_sources': [{'file': entry['file'], 'sha256': entry['sha256']}
                                for entry in dll_sources], 'files': entries}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    shutil.copy2(output / 'manifest.json', reports / 'package-manifest.json')
    (reports / 'dll-sources.json').write_text(
        json.dumps(dll_sources, indent=2) + '\n', encoding='utf-8')
    return entries


def verify_normal_launch(executable, cwd, environment, reports):
    """Launch with no flags (Explorer-equivalent), and close only our own visible window."""
    user32 = ctypes.WinDLL('user32', use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.PostMessageW.restype = wintypes.BOOL
    windows = []
    with (reports / 'normal-launch-stdout.log').open('wb') as stdout, (reports / 'normal-launch-stderr.log').open('wb') as stderr:
        process = subprocess.Popen([str(executable)], cwd=cwd, env=environment, stdout=stdout, stderr=stderr)

        @callback_type
        def inspect(window, _):
            pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid and user32.IsWindowVisible(window):
                title = ctypes.create_unicode_buffer(512)
                user32.GetWindowTextW(window, title, len(title))
                # Identify the completed workbench, not the splash, for a graceful close.
                if title.value.startswith('OrbitForge  |  '):
                    windows.append((window, title.value))
            return True

        try:
            deadline = time.monotonic() + 30
            while not windows and time.monotonic() < deadline and process.poll() is None:
                user32.EnumWindows(inspect, 0)
                time.sleep(0.1)
            if not windows:
                raise RuntimeError('Packaged EXE did not open its normal desktop window')
            # Allow the normal frame loop to run, then request a graceful close.
            time.sleep(2)
            if process.poll() is not None:
                raise RuntimeError('Normal desktop exited before its window was closed')
            # Re-enumerate to avoid sending WM_CLOSE to a stale or reparented native handle.
            windows.clear()
            user32.EnumWindows(inspect, 0)
            if len(windows) != 1:
                raise RuntimeError('Cannot uniquely identify the normal desktop window')
            if not user32.PostMessageW(windows[0][0], 0x0010, 0, 0):  # This process only.
                raise ctypes.WinError(ctypes.get_last_error())
            if process.wait(timeout=30) != 0:
                raise RuntimeError('Normal desktop launch/close failed')
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
    return {'command': [str(executable)], 'window_title': windows[0][1], 'exit_code': process.returncode}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--dependency-prefix', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reports', type=Path, required=True)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--version', help='Require the desktop build to have this release version')
    parser.add_argument('--zip', action='store_true', help='Also create a ZIP with one top-level application folder')
    args = parser.parse_args()
    if os.name != 'nt':
        raise RuntimeError('Windows packaging requires native PE dependency inspection')
    build = args.build_dir.resolve(strict=True)
    prefix = args.dependency_prefix.resolve(strict=True)
    cache = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
    version = re.search(r'^CMAKE_PROJECT_VERSION:[^=]+=(.+)$', cache, re.M).group(1).strip()
    if args.version and args.version != version:
        raise ValueError(f'Desktop version {version} differs from requested release {args.version}')
    linker = Path(re.search(r'^CMAKE_LINKER:[^=]+=(.+)$', cache, re.M).group(1))
    dumpbin = linker.with_name('dumpbin.exe').resolve(strict=True)
    vc_root = next(parent for parent in linker.parents if parent.name == 'VC')
    toolset = next(parent.name for parent in linker.parents if parent.parent.name == 'MSVC')
    crt_root = find_crt(vc_root, toolset)
    source = build / 'apps/desktop/Release'
    executable = source / 'orbitforge_desktop.exe'
    if not executable.is_file():
        raise RuntimeError('Build the Release desktop before packaging')
    output = args.output.resolve()
    reports = args.reports.resolve()
    validate_output_locations(output, reports)
    archive = output.parent / f'{output.name}.zip'
    if args.zip and archive.exists():
        raise RuntimeError(f'Archive already exists: {archive}')
    output.mkdir(parents=True, exist_ok=False)
    reports.mkdir(parents=True, exist_ok=False)
    packaged_executable = output / 'OrbitForge.exe'
    shutil.copy2(executable, packaged_executable)
    shutil.copytree(source / 'resources', output / 'resources')
    for name in ('proj', 'gdal'):
        shutil.copytree(prefix / 'share' / name, output / 'resources' / name)
    plugins = []
    for subdir in ('platforms', 'imageformats', 'tls'):
        target = output / 'plugins' / subdir
        target.mkdir(parents=True)
        files = ([prefix / 'Qt6/plugins/platforms/qwindows.dll'] if subdir == 'platforms'
                 else sorted((prefix / 'Qt6/plugins' / subdir).glob('*.dll')))
        if subdir == 'tls' and not any(path.name == 'qschannelbackend.dll' for path in files):
            raise RuntimeError('The Windows data downloader requires the Qt Schannel TLS backend')
        for path in files:
            shutil.copy2(path, target / path.name)
            plugins.append(target / path.name)
    # Put redistributables beside the EXE before resolving so System32's optional
    # installations cannot mask a missing application-local C++ runtime/loader.
    for path in [*crt_root.glob('*.dll'), prefix / 'bin/vulkan-1.dll']:
        shutil.copy2(path, output / path.name)
    (output / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=plugins\n', encoding='utf-8')
    resolver = Path(__file__).with_name('resolve_windows_runtime.cmake').resolve()
    command = ['cmake', f'-DEXECUTABLE={packaged_executable}',
               '-DPLUGINS=' + ';'.join(map(str, plugins)),
               f'-DSEARCH_DIRECTORIES={output};{prefix / "bin"}', f'-DDUMPBIN={dumpbin}',
               f'-DREPORT={reports / "resolved-dlls.txt"}', '-P', str(resolver)]
    run = subprocess.run(command, capture_output=True)
    (reports / 'dependency-resolution.log').write_bytes(run.stdout + run.stderr)
    run.check_returncode()
    dll_sources = []
    for line in (reports / 'resolved-dlls.txt').read_text(encoding='utf-8').splitlines():
        path = Path(line).resolve(strict=True)
        if not path.is_relative_to(prefix) and not path.is_relative_to(output):
            raise RuntimeError(f'Dependency resolved outside the chosen prefix: {path}')
        target = output / path.name
        if target.exists() and digest(target) != digest(path):
            raise RuntimeError(f'Conflicting runtime DLL: {path.name}')
        if path != target:
            shutil.copy2(path, target)
        dll_sources.append({'file': path.name, 'source': str(path), 'sha256': digest(target)})
    # Preserve dependency notices, including statically linked libraries and data licenses.
    repository = Path(__file__).resolve().parents[2]
    for relative, name in (
        ('modules/providers/sgp4/third_party/LICENSE', 'SGP4.txt'),
        ('modules/core/src/propagation/integrators/rklib-LICENSE.txt', 'rklib-LICENSE.txt'),
        ('modules/providers/spice/third_party/SatelliteToolbox-LICENSE.txt', 'SatelliteToolbox.txt'),
    ):
        (output / 'notices').mkdir(exist_ok=True)
        shutil.copy2(repository / relative, output / 'notices' / name)
    for notice in sorted((prefix / 'share').glob('*/copyright')):
        target = output / 'notices' / notice.parent.name / 'copyright'
        target.parent.mkdir(parents=True)
        shutil.copy2(notice, target)
    (output / 'README.txt').write_text(
        f'OrbitForge {version} Windows x64\n\n解压整个文件夹，然后双击 OrbitForge.exe。\n'
        '请保留同目录 DLL、plugins 和 resources 文件夹。\n'
        '先新建场景（Ctrl+N），默认UTC今天零点至次日零点，再插入对象（Ctrl+Shift+I）。\n'
        '数据 → 数据中心：统一管理 C04、finals2000A 和 UTC/闰秒，支持完整文件下载、导入与自动更新。\n'
        '打开或重新计算时检查当前数据；数据变化须确认后继续，项目记录实际使用的数据哈希。\n'
        '计算时段须在数据覆盖范围内，EOP 可在数据中心设置有限外推；UTC/闰秒不外推。\n'
        '日期、时间、UTC/TAI/TT合并输入；点击日历图标展开选择器。\n'
        '数值框支持直接输入和键盘微调，轨道支持经典瞬时根数与位置/速度。\n'
        '传播器可选二体与J2长期摄动，点击“模型…”查看模型信息；显示步长在轨道页设置。\n'
        '初态支持GCRF/ITRF位置速度，轨道根数在GCRF下定义，μ继承中心天体。\n'
        'J2使用平均椭圆；其输入位置速度是辅助开普勒状态，切换模型不执行瞬时/平均转换。\n'
        '也可从启动页明确选择“打开二体示例”。新场景默认不包含卫星。\n'
        '双击场景树对象或右键“属性”打开工作区标签页；可以弹出或放回。\n'
        '基本分组设置轨道和轨迹范围，二维/三维分组设置显示与模型。\n'
        '卫星属性可设置轨迹线型、颜色和粗细；对象右键可生成报告与曲线。\n'
        '默认显示一圈，显示步长60秒；跳转后按需补算，默认卫星为点标记。\n'
        '三维默认GCRF惯性系，可切换ITRF；二维为ITRF/WGS84。\n'
        'Ctrl+Space 播放/暂停；Ctrl+0 适配轨道；Ctrl+L 定位卫星；Ctrl+T 跳转时间。\n\n'
        'Ctrl+J 收起/展开时间轴，保留播放控制、当前时间和浏览范围。\n'
        'Ctrl+3 打开二维地图，与三维共享时间和选择；滚轮缩放、拖动平移、双击复位。\n\n'
        'Run OrbitForge.exe from this folder.\n'
        'DLLs, Qt plugins, offline Earth imagery and example model are included.\n'
        'Keep the folder intact. A Vulkan-capable GPU and its vendor driver are required.\n'
        'No Python, Qt SDK or development PATH is required to launch the desktop.\n'
        'Earth imagery credit: NASA Goddard Space Flight Center. See resources/earth/NOTICE.txt.\n'
        'The bundled C++ runtime is from Microsoft Visual Studio, used under its redistributable terms.\n'
        'This is a development preview. See the project documentation for supported models and limitations.\n', encoding='utf-8')
    entries = write_package_manifest(output, reports, executable, dll_sources)
    if args.verify:
        environment = {key: value for key, value in os.environ.items()
                       if not key.upper().startswith(('QT_', 'QML', 'VSG', 'ROCKY', 'GDAL', 'PROJ', 'VK_'))}
        system_root = Path(os.environ['SystemRoot'])
        environment['PATH'] = os.pathsep.join(map(str, (system_root / 'System32', system_root)))
        environment['ORBITFORGE_DATA_HOME'] = str(reports / 'user-data')
        # Run from an unrelated empty directory to verify relative resource discovery.
        cwd = reports / 'empty-cwd'
        cwd.mkdir()
        smoke = reports / 'portable-smoke'
        command = [str(packaged_executable), '--portable-smoke', str(smoke)]
        run = subprocess.run(command, cwd=cwd, env=environment, capture_output=True, timeout=90)
        (reports / 'portable-stdout.log').write_bytes(run.stdout)
        (reports / 'portable-stderr.log').write_bytes(run.stderr)
        run.check_returncode()
        report = json.loads((smoke / 'report.json').read_text(encoding='utf-8'))
        if report['failure'] or not all(report[key] for key in
                ('shared_device', 'shared_scene', 'independent_cameras', 'all_windows_closed_before_shutdown')):
            raise RuntimeError('Packaged desktop smoke failed')
        normal_launch = verify_normal_launch(packaged_executable, cwd, environment, reports)
        for entry in entries:
            if digest(output / entry['path']) != entry['sha256']:
                raise RuntimeError(f"Packaged file changed during launch: {entry['path']}")
        (reports / 'verification.json').write_text(json.dumps({
            'passed': True, 'path': environment['PATH'], 'cwd': str(cwd),
            'vulkan_validation_enabled': report['vulkan_validation_enabled'],
            'scope': 'same-host relocated folder with development environment paths removed; not a clean VM',
            'command': command, 'normal_launch': normal_launch}, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.zip:
        with zipfile.ZipFile(archive, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
            for path in sorted(output.rglob('*')):
                if path.is_file():
                    bundle.write(path, (Path(output.name) / path.relative_to(output)).as_posix())
        with zipfile.ZipFile(archive) as bundle:
            if bundle.testzip() is not None:
                raise RuntimeError('ZIP integrity check failed')
        (reports / 'archive.json').write_text(json.dumps({'path': str(archive), 'bytes': archive.stat().st_size,
                                                        'sha256': digest(archive)}, indent=2) + '\n', encoding='utf-8')
    total = sum(entry['bytes'] for entry in entries)
    print(json.dumps({'output': str(output), 'files': len(entries), 'bytes': total,
                      'runtime_dlls': len(list(output.glob('*.dll'))), 'verified': args.verify,
                      'zip': str(archive) if args.zip else None}))


if __name__ == '__main__':
    main()
