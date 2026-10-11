"""Build pinned wxWidgets locally with explicit theme switching. Python 3.10+, CMake, VS 2026."""
import hashlib
from pathlib import Path
import os
import subprocess
import urllib.request
import zipfile
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'build/deps/wx-src'
BUILD = ROOT / 'build/deps/wx-build'
SHA256 = '458a1ef598c90174ee43622e8e63bfa1eccb451ffc2258bb4f8edcb050c5feb1'
APPEARANCE = '''wxApp::AppearanceResult wxApp::SetAppearance(Appearance appearance)
{
    // VCamdroid Studio: reuse the OS color refresh path, only for app windows.
    if ( !wxMSWImpl::InitDarkMode() )
        return AppearanceResult::Failure;
    if ( gs_appMode == AppMode_Default )
        wxDarkModeModule::SetSettings(new wxDarkModeSettings());
    const PreferredAppMode mode = appearance == Appearance::Dark
        ? AppMode_ForceDark : appearance == Appearance::Light
        ? AppMode_ForceLight : AppMode_AllowDark;
    wxMSWImpl::SetPreferredAppMode(mode);
    gs_appMode = mode;
    gs_hasChanged = true;
    for ( auto* window : wxTopLevelWindows )
        ::SendMessage((HWND)window->GetHWND(), WM_SYSCOLORCHANGE, 0, 0);
    return AppearanceResult::Ok;
}
'''
def main():
    downloads = SOURCE.parent / 'downloads'
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / 'wxWidgets-3.3.3.zip'
    if not archive.exists():
        print('Downloading official wxWidgets source', flush=True)
        urllib.request.urlretrieve('https://github.com/wxWidgets/wxWidgets/releases/download/v3.3.3/wxWidgets-3.3.3.zip', archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('wxWidgets source checksum mismatch; review the cached download')
    if not (SOURCE / 'CMakeLists.txt').exists():
        with zipfile.ZipFile(archive) as bundle:
            for name in bundle.namelist():
                if not (SOURCE / name).resolve().is_relative_to(SOURCE.resolve()):
                    raise RuntimeError('Unsafe archive path')
            bundle.extractall(SOURCE)
    path = SOURCE / 'src/msw/darkmode.cpp'
    text = path.read_text(encoding='utf-8')
    start = text.index('wxApp::AppearanceResult wxApp::SetAppearance(Appearance appearance)')
    end = text.index('// ----------------------------------------------------------------------------', start)
    patched = text[:start] + APPEARANCE + '\n' + text[end:]
    if patched != text:
        path.write_text(patched, encoding='utf-8')
    env = {k.upper(): v for k, v in os.environ.items()}
    subprocess.run(['cmake', '-S', str(SOURCE), '-B', str(BUILD), '-G', 'Visual Studio 18 2026', '-A', 'x64',
                    '-DwxBUILD_SHARED=ON', '-DwxBUILD_VENDOR=studio', '-DwxBUILD_SAMPLES=OFF', '-DwxBUILD_TESTS=OFF',
                    '-DwxUSE_WEBVIEW=OFF', '-DwxUSE_STC=OFF', '-DwxUSE_MEDIACTRL=OFF'], env=env, check=True)
    subprocess.run(['cmake', '--build', str(BUILD), '--config', 'Release', '--target', 'wxcore', '--parallel', '8'], env=env, check=True)
    print('Studio UI dependency ready (Release/x64)')
if __name__ == '__main__':
    main()
