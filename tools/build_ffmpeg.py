#!/usr/bin/env python3
"""Builds a reduced, static FFmpeg (LGPL-2.1-or-later) into build/deps/ffmpeg, for a redistributable
app (tools/make_app.py).

Homebrew's FFmpeg is built under GPL version 3: fine to build and play at home, not to distribute
in an app that also carries Unicorn (GPL-2.0). This one has only what the launcher uses: the CD
music (Ogg Vorbis), WAV, and for the videos to come AVI with Cinepak and MS-ADPCM, plus
libswresample. Built for macOS 11 like the other libraries (tools/build_mac_deps.py).

Derived from zeus-poseidon-arm64's tools/build_ffmpeg.py (GPL-2.0-or-later).

Usage: python3 -I tools/build_ffmpeg.py   (the source archive is reused if already in build/deps)
"""
import hashlib
import os
import subprocess
import sys
import tarfile
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VERSION = "9.0.2"
URL = f"https://ffmpeg.org/releases/ffmpeg-{VERSION}.tar.xz"
SHA256 = "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e"
TARGET = "11.0"

CONFIGURE = [
    "--disable-everything", "--disable-programs", "--disable-doc", "--disable-network",
    "--disable-autodetect", "--disable-avdevice", "--disable-avfilter", "--disable-swscale",
    "--enable-static", "--disable-shared", "--enable-pic",
    "--enable-demuxer=ogg,wav,avi", "--enable-parser=vorbis",
    "--enable-decoder=vorbis,pcm_s16le,pcm_u8,cinepak,adpcm_ms",
    "--enable-protocol=file", "--enable-swresample",
    f"--extra-cflags=-mmacosx-version-min={TARGET}", f"--extra-ldflags=-mmacosx-version-min={TARGET}",
]


def main():
    deps = os.path.join(ROOT, "build", "deps")
    os.makedirs(deps, exist_ok=True)
    archive = os.path.join(deps, f"ffmpeg-{VERSION}.tar.xz")
    if not os.path.exists(archive):
        print("downloading", URL, flush=True)
        urllib.request.urlretrieve(URL, archive)
    with open(archive, "rb") as f:
        if hashlib.sha256(f.read()).hexdigest() != SHA256:
            raise SystemExit(f"{archive}: unexpected SHA-256, delete it and download again")
    source = os.path.join(deps, f"ffmpeg-{VERSION}")
    if not os.path.isdir(source):
        with tarfile.open(archive) as tar:
            tar.extractall(deps, filter="data")
    prefix = os.path.join(deps, "ffmpeg")
    env = dict(os.environ, MACOSX_DEPLOYMENT_TARGET=TARGET)
    subprocess.run(["./configure", f"--prefix={prefix}"] + CONFIGURE, cwd=source, check=True, env=env)
    subprocess.run(["make", f"-j{os.cpu_count() or 4}"], cwd=source, check=True, env=env)
    subprocess.run(["make", "install"], cwd=source, check=True, env=env)
    print("reduced FFmpeg installed in", prefix)


if __name__ == "__main__":
    sys.exit(main())
