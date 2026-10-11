"""Compile and test ADB launches using an isolated fake helper, never a phone."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
environment = {k.upper(): v for k, v in os.environ.items()}
vswhere = Path('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath'], text=True, env=environment).strip())
command = f'call "{vs / "Common7/Tools/VsDevCmd.bat"}" -arch=x64 -host_arch=x64 >nul && set'
# cmd.exe uses its own quoting rules, not the child CRT argument quoting.
output = subprocess.check_output('cmd.exe /d /s /c "' + command + '"', env=environment)
for line in output.decode('mbcs').splitlines():
    if '=' in line and not line.startswith('='):
        key, value = line.split('=', 1)
        environment[key.upper()] = value
tests = ROOT / 'windows/x64/Tests'
tests.mkdir(parents=True, exist_ok=True)
fixture = Path(tempfile.mkdtemp(prefix='USB & café ', dir=tests))
exe = fixture / 'adb_launch_test.exe'
compiler = Path(environment['VCTOOLSINSTALLDIR']) / 'bin/Hostx64/x64/cl.exe'
subprocess.run([str(compiler), '/nologo', '/std:c++17', '/EHsc', '/MD', '/utf-8',
    '/I' + str(ROOT / 'windows/src'), str(ROOT / 'windows/tests/adb_launch_test.cpp'),
    '/Fe:' + str(exe), '/Fo:' + str(fixture) + os.sep], env=environment, check=True)
(fixture / 'adb').mkdir()
shutil.copyfile(exe, fixture / 'adb/adb.exe')
subprocess.run([str(exe)], cwd=fixture, env=environment, check=True)
print('Fixture:', fixture)
