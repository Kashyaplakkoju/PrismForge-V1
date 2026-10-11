"""Audit a local PrismForge development bundle's PE dependencies."""
import argparse
import json
from pathlib import Path
import subprocess
from package_apps import audit_imports

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directory', type=Path)
args = parser.parse_args()
vswhere = Path('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
vs = Path(subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip())
dumpbin = sorted((vs / 'VC/Tools/MSVC').glob('*/bin/Hostx64/x64/dumpbin.exe'))[-1]
files = {'Windows/' + p.relative_to(args.directory).as_posix(): p
    for p in args.directory.rglob('*') if p.is_file() and p.suffix.lower() in ('.exe', '.dll')}
report = audit_imports(files, dumpbin)
(args.directory / 'DEPENDENCY-AUDIT.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(f'Dependency/architecture audit passed: {len(report)} PE files')
