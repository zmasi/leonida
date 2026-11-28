#!/usr/bin/env python3
"""Fetch the pinned VKD3D-Proton release for Leonida's Linux package.

On Linux the D3D12 runtime (victory/, a Windows program) runs under Wine, and Wine needs a Direct3D 12 implementation:
VKD3D-Proton (LGPL-2.1-or-later, https://github.com/HansKristian-Work/vkd3d-proton) translates it to Vulkan. This
tool puts the 64-bit d3d12.dll and d3d12core.dll of one pinned release, with its license and a note on where its
source is, into the package folder, so a Linux player needs nothing else for D3D12 (scripts/play-linux.sh installs
them into Leonida's own Wine prefix). Every download is checked against the SHA-256 pinned below.

Standard library only (Python 3.14 or newer: compression.zstd reads the release's .tar.zst).

    python -I scripts/fetch_vkd3d_proton.py --out dist/linux/vkd3d-proton
"""
import argparse
import hashlib
import io
import os
import sys
import tarfile
import urllib.request

VERSION = "3.1"
RELEASE = (f"https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v{VERSION}/"
           f"vkd3d-proton-{VERSION}.tar.zst",
           "b85ea8219b71093a3924913dd7aae6172333dc0c069983ed510816d67f02120b")
# The release archive carries no license text: the notice and the LGPL-2.1 from the same tag.
LICENSE_FILES = {
    "COPYING": (f"https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/v{VERSION}/COPYING",
                "f51ad4bf5a3bd2b5f383fe28150b23d01e6f5d7b620b194150e7c6a620c43c1f"),
    "LICENSE": (f"https://raw.githubusercontent.com/HansKristian-Work/vkd3d-proton/v{VERSION}/LICENSE",
                "dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551"),
}
DLLS = ("d3d12.dll", "d3d12core.dll")


def fetch(url, sha256):
    with urllib.request.urlopen(url, timeout=120) as response:
        data = response.read()
    got = hashlib.sha256(data).hexdigest()
    if got != sha256:
        raise SystemExit(f"{url}: SHA-256 {got}, expected {sha256}; not used")
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", default=os.path.join("dist", "linux", "vkd3d-proton"),
                        help="folder for the DLLs, license and source note (created)")
    args = parser.parse_args()
    try:
        import compression.zstd as zstd
    except ImportError:
        raise SystemExit("Python 3.14 or newer is needed (compression.zstd reads the .tar.zst release)")

    os.makedirs(args.out, exist_ok=True)
    archive = zstd.decompress(fetch(*RELEASE))
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        for name in DLLS:
            member = tar.getmember(f"vkd3d-proton-{VERSION}/x64/{name}")
            with tar.extractfile(member) as src, open(os.path.join(args.out, name), "wb") as dst:
                dst.write(src.read())
    for name, (url, sha256) in LICENSE_FILES.items():
        with open(os.path.join(args.out, name), "wb") as dst:
            dst.write(fetch(url, sha256))
    with open(os.path.join(args.out, "SOURCE.txt"), "w", encoding="utf-8", newline="\n") as note:
        note.write(f"VKD3D-Proton {VERSION}, unmodified 64-bit d3d12.dll and d3d12core.dll from\n"
                   f"{RELEASE[0]}\n(SHA-256 {RELEASE[1]}).\n"
                   f"Licensed under the GNU Lesser General Public License 2.1 or later (LICENSE, COPYING).\n"
                   f"Source: https://github.com/HansKristian-Work/vkd3d-proton/tree/v{VERSION}\n")
    print(f"VKD3D-Proton {VERSION}: {', '.join(DLLS)}, LICENSE, COPYING and SOURCE.txt in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
