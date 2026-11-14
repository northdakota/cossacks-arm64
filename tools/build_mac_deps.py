#!/usr/bin/env python3
"""Builds for macOS 11 the libraries the app carries, into build/deps/mac: SDL 2 and Unicorn
(x86 only), from their sources, checked by SHA-256.

Homebrew's libraries are built for the system of the machine that built them: an app carrying
them does not start on an older Mac. With MACOSX_DEPLOYMENT_TARGET=11.0 it starts from Big Sur.

Derived from zeus-poseidon-arm64's tools/build_mac_deps.py (GPL-2.0-or-later).

Usage: python3 -I tools/build_mac_deps.py
Then:  cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \\
         -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_PREFIX_PATH=$PWD/build/deps/mac \\
         -DCOSSACKS_FFMPEG=build/deps/ffmpeg
       (with PKG_CONFIG_PATH=$PWD/build/deps/mac/lib/pkgconfig, for Unicorn)
"""
import hashlib
import os
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPS = os.path.join(ROOT, "build", "deps")
PREFIX = os.path.join(DEPS, "mac")
TARGET = "11.0"
# name: version, address, SHA-256
SOURCES = {
    "sdl": ("2.32.10", "https://github.com/libsdl-org/SDL/releases/download/release-{v}/SDL2-{v}.tar.gz",
            "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"),
    "unicorn": ("2.1.4", "https://files.pythonhosted.org/packages/b2/1b/b4248aa8422e86de690cf8e85cf8feae4c33405a097d1ebe71570bdaa6f5/"
                "unicorn-{v}.tar.gz", "00567a70e323f749b419cd86bee4f9115beab7ebba32194581c090cbb7c59cff"),
}


def run(*args, cwd=None):
    print("+", " ".join(args), flush=True)
    if subprocess.run(args, cwd=cwd).returncode != 0:
        sys.exit(f"failed: {' '.join(args)}")


def sources(name):
    """Folder of the sources of `name`, downloaded and unpacked if needed."""
    version, url, sha256 = SOURCES[name]
    url = url.format(v=version)
    archive = os.path.join(DEPS, os.path.basename(url))
    if not os.path.exists(archive):
        print("downloading", url, flush=True)
        urllib.request.urlretrieve(url, archive)
    with open(archive, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != sha256:
            sys.exit(f"{archive}: unexpected SHA-256, delete it and download again")
    with tarfile.open(archive) as tar:
        top = tar.getnames()[0].split("/")[0]
        folder = os.path.join(DEPS, top)
        if not os.path.isdir(folder):
            tar.extractall(DEPS, filter="data")
    return folder


def done(name):
    return os.path.exists(os.path.join(PREFIX, f".{name}-{SOURCES[name][0]}"))


def mark(name):
    open(os.path.join(PREFIX, f".{name}-{SOURCES[name][0]}"), "w").close()


def cmake(name, source, *options):
    build = os.path.join(DEPS, f"build-{name}")
    run("cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_INSTALL_PREFIX={PREFIX}", "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_PREFIX_PATH={PREFIX}",
        f"-DCMAKE_OSX_DEPLOYMENT_TARGET={TARGET}", *options)
    run("cmake", "--build", build)
    run("cmake", "--install", build)


def main():
    os.makedirs(PREFIX, exist_ok=True)
    os.environ["MACOSX_DEPLOYMENT_TARGET"] = TARGET
    # Source paths in the binaries (assertions' __FILE__) relative: the app must name no folder of
    # the machine that built it.
    flags = f"-O2 -ffile-prefix-map={DEPS}=deps -mmacosx-version-min={TARGET}"
    for variable in ("CFLAGS", "CXXFLAGS"):
        os.environ[variable] = f"{os.environ.get(variable, '')} {flags}".strip()
    if not done("sdl"):
        cmake("sdl", sources("sdl"), "-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF")
        mark("sdl")
    if not done("unicorn"):
        cmake("unicorn", os.path.join(sources("unicorn"), "src"), "-DUNICORN_ARCH=x86", "-DUNICORN_BUILD_TESTS=OFF")
        mark("unicorn")
    print("libraries for macOS", TARGET, "installed in", PREFIX)


if __name__ == "__main__":
    sys.exit(main())
