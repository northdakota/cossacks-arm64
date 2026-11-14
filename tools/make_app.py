#!/usr/bin/env python3
"""Assembles "Cossacks on Mac.app" from a release build of cossacks-run.

The app carries the libraries the program depends on (SDL 2 and Unicorn, built for macOS 11 by
tools/build_mac_deps.py; FFmpeg is linked in statically, reduced and LGPL, by tools/build_ffmpeg.py),
named relative to the app, and is signed locally (ad hoc). It contains no file of the game: the
game's folder is found or asked for at the first launch (app_mode.cpp).

Derived from zeus-poseidon-arm64's tools/make_app.py (GPL-2.0-or-later).

Usage: python3 -I tools/make_app.py [--binary PROGRAM] [--version X.Y] [OUT.app]
       (defaults: build/release/cossacks-run, 0.0, build/Cossacks on Mac.app)
"""
import os
import plistlib
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYSTEM = ("/usr/lib/", "/System/")
DEPS = os.path.join(ROOT, "build", "deps")
LIBS = os.path.join(DEPS, "mac", "lib")
APP_NAME = "Cossacks on Mac"
EXECUTABLE = "cossacks"

# Licences shipped with the app: (file in the app, source in the repository or build/deps).
LICENCES = [
    ("LICENSE.txt", os.path.join(ROOT, "LICENSE")),
    ("NOTICE.txt", os.path.join(ROOT, "NOTICE")),
    ("licences/SDL2-LICENSE.txt", os.path.join(DEPS, "SDL2-2.32.10", "LICENSE.txt")),
    ("licences/Unicorn-COPYING.txt", os.path.join(DEPS, "unicorn-2.1.4", "src", "qemu", "COPYING")),
    ("licences/FFmpeg-LICENSE.md", os.path.join(DEPS, "ffmpeg-9.0.2", "LICENSE.md")),
    ("licences/FFmpeg-COPYING.LGPLv2.1.txt", os.path.join(DEPS, "ffmpeg-9.0.2", "COPYING.LGPLv2.1")),
]
THIRD_PARTY = """Libraries in this app, and where their sources are
=====================================================

- SDL 2.32.10 (zlib licence): https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10
- Unicorn 2.1.4, x86 only (GPL-2.0): https://pypi.org/project/unicorn/2.1.4/ (sdist), https://github.com/unicorn-engine/unicorn
- FFmpeg 9.0.2, reduced (LGPL-2.1-or-later), linked statically: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz,
  configured as in tools/build_ffmpeg.py of https://github.com/northdakota/cossacks-arm
  (the launcher's source, which lets you rebuild and relink it).

The launcher itself: GPL-2.0-or-later, https://github.com/northdakota/cossacks-arm, derived from
zeus-poseidon-arm64 by nic0lasm, https://github.com/nic0lasm/zeus-poseidon-arm64.
"""


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def dependencies(binary):
    """Non-system libraries a Mach-O binary depends on."""
    lines = run("otool", "-L", binary).splitlines()[1:]
    paths = [line.strip().split(" (")[0] for line in lines]
    return [p for p in paths if not p.startswith(SYSTEM)]


def resolve(reference, user):
    """Real path of a dependency named by `user`."""
    folder = os.path.dirname(os.path.realpath(user))
    for prefix in ("@loader_path/", "@rpath/"):
        if reference.startswith(prefix):
            name = reference[len(prefix):]
            for base in (folder, LIBS):
                if os.path.exists(os.path.join(base, name)):
                    return os.path.realpath(os.path.join(base, name))
            raise SystemExit(f"dependency not found: {reference} (named by {user})")
    if reference.startswith(("/opt/homebrew/", "/usr/local/")):
        raise SystemExit(f"{user} depends on {reference}: build with tools/build_mac_deps.py, not Homebrew")
    return os.path.realpath(reference)


def delete_rpaths(binary_file):
    listing = run("otool", "-l", binary_file).splitlines()
    for i, line in enumerate(listing):
        if line.strip() == "cmd LC_RPATH":
            run("install_name_tool", "-delete_rpath", listing[i + 2].split()[1], binary_file)


def main(argv):
    args = argv[1:]
    binary = os.path.join(ROOT, "build", "release", "cossacks-run")
    version = "0.0"
    if args[:1] == ["--binary"]:
        binary, args = os.path.abspath(args[1]), args[2:]
    if args[:1] == ["--version"]:
        version, args = args[1], args[2:]
    app = args[0] if args else os.path.join(ROOT, "build", f"{APP_NAME}.app")
    if not os.path.exists(binary):
        raise SystemExit(f"{binary} is missing: build it first")
    if os.path.exists(app):
        if not app.endswith(".app") or not os.path.exists(os.path.join(app, "Contents", "Info.plist")):
            raise SystemExit(f"{app} exists and is not an app made by this tool")
        shutil.rmtree(app)
    macos = os.path.join(app, "Contents", "MacOS")
    frameworks = os.path.join(app, "Contents", "Frameworks")
    resources = os.path.join(app, "Contents", "Resources")
    for folder in (macos, frameworks, resources):
        os.makedirs(folder)
    program = os.path.join(macos, EXECUTABLE)
    shutil.copy2(binary, program)

    copied = {}  # real path of the original -> copy in the app
    pending = [(program, binary)]
    while pending:
        target, origin = pending.pop()
        for reference in dependencies(target):
            if reference.startswith("@executable_path/"):
                continue
            real = resolve(reference, origin)
            if real == os.path.realpath(origin):
                continue
            name = os.path.basename(reference)
            if real not in copied:
                copy = os.path.join(frameworks, name)
                shutil.copy2(real, copy)
                os.chmod(copy, 0o755)
                copied[real] = copy
                run("install_name_tool", "-id", "@executable_path/../Frameworks/" + name, copy)
                pending.append((copy, real))
            run("install_name_tool", "-change", reference,
                "@executable_path/../Frameworks/" + os.path.basename(copied[real]), target)

    with open(os.path.join(app, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump({
            "CFBundleName": APP_NAME,
            "CFBundleDisplayName": APP_NAME,
            "CFBundleIdentifier": "community.cossacks-arm.mac",
            "CFBundleExecutable": EXECUTABLE,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": version,
            "CFBundleVersion": version,
            "LSMinimumSystemVersion": "11.0",
            "LSApplicationCategoryType": "public.app-category.strategy-games",
            "NSHighResolutionCapable": True,
        }, f)

    for name, source in LICENCES:
        if not os.path.exists(source):
            raise SystemExit(f"licence text missing: {source}")
        os.makedirs(os.path.dirname(os.path.join(resources, name)), exist_ok=True)
        shutil.copy2(source, os.path.join(resources, name))
    with open(os.path.join(resources, "THIRD-PARTY.txt"), "w") as f:
        f.write(THIRD_PARTY)

    # Debugging information holds the build paths, so the home folder's name of whoever built it.
    for binary_file in [program] + sorted(copied.values()):
        run("strip", "-S", "-x", binary_file)
    # The program names its libraries by @executable_path: the search paths CMake gave it towards
    # build/deps are of no use and would name the build folder.
    delete_rpaths(program)
    for library in sorted(copied.values()):
        delete_rpaths(library)
        run("install_name_tool", "-add_rpath", "@loader_path", library)
        run("codesign", "--force", "--sign", "-", library)
    run("codesign", "--force", "--sign", "-", app)

    # Last check: no file of the app may name the home folder.
    home = os.path.expanduser("~").encode()
    leaks = []
    for folder, _, files in os.walk(app):
        for name in files:
            with open(os.path.join(folder, name), "rb") as f:
                if home in f.read():
                    leaks.append(os.path.relpath(os.path.join(folder, name), app))
    if leaks:
        raise SystemExit("not fit to distribute, it names the home folder: " + ", ".join(leaks))
    size = sum(os.path.getsize(os.path.join(d, f)) for d, _, files in os.walk(app) for f in files)
    print(f"{app}: {len(copied)} libraries carried, {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main(sys.argv)
