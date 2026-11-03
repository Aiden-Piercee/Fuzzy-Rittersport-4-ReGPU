"""Builds out/FM4-<version>-patch.zip, the update the launcher downloads and unpacks over an install.

  py -3 tools/make_patch.py <version> [--build build\\fm4-release] [--launcher <publish dir>] [--notes <text|file>] [--out <zip>]

Layout (paths relative to the install root, which is what Updater.Apply unpacks against):
  fm4.exe and the DLLs the build ships beside it (amd_fidelityfx_*, avcodec, avutil, swresample, d3dcompiler_47, ...)
  launcher/...          the launcher publish output (dotnet publish launcher\\FM4Launcher -c Release -r win-x64)
  changelog.txt         only with --notes (a short text, or the path of a text file)

Nothing from the game (disc files), no shader cache, no pdb/lib/log/.old/.new/.prev files, no WebView2 profile.
Refuses when <version> differs from FM2_VERSION in src/version.h. The zip is read back and checked.
"""
import argparse
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP_EXT = ('.pdb', '.lib', '.log', '.old', '.new', '.prev', '.obj', '.exp', '.ilk')
BUILD_RUNTIME = ('.exe', '.dll')  # what the build dir ships beside fm4.exe; the rest of it is build output


def header_version():
    text = open(os.path.join(ROOT, 'src', 'version.h'), encoding='utf-8').read()
    m = re.search(r'#define\s+FM2_VERSION\s+"([0-9.]+)"', text)
    return m.group(1) if m else None


def collect(build, launcher):
    files = []
    for name in sorted(os.listdir(build)):
        path = os.path.join(build, name)
        if os.path.isfile(path) and name.lower().endswith(BUILD_RUNTIME):
            files.append((path, name))
    for dirpath, dirs, names in os.walk(launcher):
        dirs[:] = [d for d in dirs if not d.lower().endswith('.webview2')]
        for name in sorted(names):
            if name.lower().endswith(SKIP_EXT):
                continue
            path = os.path.join(dirpath, name)
            files.append((path, 'launcher/' + os.path.relpath(path, launcher).replace(os.sep, '/')))
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('version')
    ap.add_argument('--build', default=os.path.join('build', 'fm4-release'))
    ap.add_argument('--launcher', default=os.path.join('launcher', 'FM4Launcher', 'bin', 'Release', 'net9.0', 'win-x64', 'publish'))
    ap.add_argument('--notes', default=None)
    ap.add_argument('--out', default=None)
    a = ap.parse_args()

    have = header_version()
    if have != a.version:
        print('src/version.h says %s, not %s: bump it (and rebuild) first' % (have, a.version))
        return 1
    build = a.build if os.path.isabs(a.build) else os.path.join(ROOT, a.build)
    launcher = a.launcher if os.path.isabs(a.launcher) else os.path.join(ROOT, a.launcher)
    if not os.path.isfile(os.path.join(build, 'fm4.exe')):
        print('no fm4.exe in', build)
        return 1
    if not os.path.isfile(os.path.join(launcher, 'FM4Launcher.exe')):
        print('no FM4Launcher.exe in', launcher, '(dotnet publish launcher\\FM4Launcher -c Release -r win-x64)')
        return 1
    notes = None
    if a.notes:
        notes = open(a.notes, encoding='utf-8').read() if os.path.isfile(a.notes) else a.notes
    out = a.out or os.path.join(ROOT, 'out', 'FM4-%s-patch.zip' % a.version)
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    files = collect(build, launcher)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path, name in files:
            z.write(path, name)
        if notes:
            z.writestr('changelog.txt', notes.strip() + '\n')

    # read back: every name as the updater wants it
    with zipfile.ZipFile(out) as z:
        names = z.namelist()
        for n in names:
            if any(ord(c) < 32 for c in n) or '\\' in n or n.startswith('/') or ':' in n or '..' in n.split('/'):
                print('bad name in zip:', repr(n))
                return 1
        for need in ('fm4.exe', 'launcher/FM4Launcher.exe'):
            if need not in names:
                print(need, 'missing from the zip')
                return 1
        bad = z.testzip()
        if bad:
            print('corrupt entry', bad)
            return 1
        for i in z.infolist():
            print('%10d  %s' % (i.file_size, i.filename))
    print('%s: %d files, %.1f MB' % (out, len(names), os.path.getsize(out) / 1048576))
    return 0


if __name__ == '__main__':
    sys.exit(main())
