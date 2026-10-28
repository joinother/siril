#!/usr/bin/env python3
"""Copy the ELF dependency closure of libsiril-ohos.so into a HAP lib directory.

Every copied file is stored under its DT_SONAME so the HarmonyOS loader can
resolve it at runtime.  Libraries that the system already provides (libc,
libm, libz, NAPI, hilog, ...) are skipped.

Usage:
  python3 scripts/collect-ohos-libs.py \
      --root libsiril-ohos.so \
      --prefix <sysroot> \
      --sysroot <ohos native sysroot> \
      --out harmonyos/entry/libs/arm64-v8a
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SYSTEM_LIBS = {
    "libc.so", "libm.so", "libdl.so", "libpthread.so", "librt.so", "libz.so",
    "libstdc++.so", "libc++_shared.so", "libunwind.so", "libclang_rt.builtins.so",
    "libace_napi.z.so", "libhilog_ndk.z.so", "libnapi.so", "libets_utils.z.so",
    "libipc_capi.so", "libsamgr.so", "libbundle_ndk.z.so", "libability_runtime.so",
    "libc++.so", "liblog.so",
}


def run(args):
    return subprocess.run(args, capture_output=True, text=True, check=False)


def soname_of(path: Path, readelf: str) -> str:
    result = run([readelf, "-d", str(path)])
    for line in result.stdout.splitlines():
        match = re.search(r"\(SONAME\)\s+Library soname:\s+\[(.+?)\]", line)
        if match:
            return match.group(1)
    return path.name


def needed_of(path: Path, readelf: str):
    result = run([readelf, "-d", str(path)])
    return re.findall(r"\(NEEDED\)\s+Shared library:\s+\[(.+?)\]", result.stdout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--prefix", required=True)
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--readelf", default=None)
    args = ap.parse_args()

    readelf = args.readelf or shutil.which("llvm-readelf") or shutil.which("readelf")
    if not readelf:
        print("llvm-readelf not found", file=sys.stderr)
        return 2

    search_dirs = [Path(args.prefix) / "lib", Path(args.sysroot) / "usr/lib/aarch64-linux-ohos"]
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    root = Path(args.root)
    queue = [root]
    seen = set()
    copied = []

    while queue:
        current = queue.pop()
        current = current.resolve()
        if current in seen or not current.exists():
            continue
        seen.add(current)

        for needed in needed_of(current, readelf):
            if needed in SYSTEM_LIBS or needed in ("libc.so.6",):
                continue
            target = None
            for directory in search_dirs:
                candidate = directory / needed
                if candidate.exists():
                    target = candidate
                    break
            if target is None:
                print(f"warning: {needed} (needed by {current.name}) not found", file=sys.stderr)
                continue
            resolved = target.resolve()
            soname = soname_of(resolved, readelf)
            destination = out_dir / soname
            if not destination.exists():
                shutil.copy2(resolved, destination)
                copied.append(soname)
            queue.append(resolved)

    # the engine itself
    root_soname = soname_of(root.resolve(), readelf)
    shutil.copy2(root.resolve(), out_dir / root_soname)

    print(f"copied {len(copied)} libraries + {root_soname} into {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
