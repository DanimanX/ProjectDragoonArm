#!/usr/bin/env python3
"""extract_disc.py - turn a Panzer Dragoon Saga disc dump into the loose-file
tree the engine expects, and prove the dump is the right one.

The engine (Azel, see engine/) opens game files by bare name ("1ST_READ.PRG",
"SDDRVS.TSK", ...), so the port needs the disc's ISO9660 payload as a flat
directory. This tool:

  * reads the 2352-byte-sector .bin data track directly (no mount, no root),
  * walks ISO9660 and writes every file into <out>/,
  * for Disc 1, verifies the extracted PRGs against the sha256 manifest that
    upstream atolm publishes (config/targets/*.yaml). A wrong dump or a
    truncated transfer fails here instead of booting to a black screen.

Usage:
  tools/extract_disc.py --bin "Disc 1 (Track 1).bin" --out engine-data/disc1
  tools/extract_disc.py --7z  "Panzer Dragoon Saga (USA) (Disc 1).7z" --out engine-data/disc1
  tools/extract_disc.py --bin ... --out ... --no-verify    # skip hash gate

Exit code is non-zero if the hash gate fails, so build scripts can depend on it.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SECTOR_SIZE = 2352
DATA_SIZE = 2048
PVD_SECTOR = 16

# sha256 of the PRGs on a verified USA Disc 1 (product MK-81307), taken from
# upstream atolm's config/targets/*.yaml manifests. Size is checked too.
DISC1_MANIFEST = {
    "1ST_READ.PRG": (253650, "2bb6c2d66ec17fe3"),
    "SEGALOGO.PRG": (3620, "beb682eee488c616"),
    "TWN_CAMP.PRG": (181716, "ce2b46e46c6b23f0"),
    "TWN_CARA.PRG": (212848, "19fdb2939c72c332"),
    "TWN_E006.PRG": (49152, "b98ba08b8ecb7c88"),
    "TWN_E011.PRG": (58940, "89912a9711a2b449"),
    "TWN_E014.PRG": (49484, "57a40089c7343d82"),
    "TWN_E021.PRG": (67540, "695ce26a65d64be4"),
    "TWN_E022.PRG": (49208, "b1ae56464f6ac8d7"),
    "TWN_EXCA.PRG": (75640, "7c47b18d0aaab68f"),
    "TWN_JIRI.PRG": (49036, "43f670d27628e0af"),
    "TWN_RUIN.PRG": (51460, "3c13d3b7b67ca750"),
    "TWN_SEEK.PRG": (194928, "5dc8bdc514512171"),
    "TWN_ZOAH.PRG": (421452, "828ec2960b379135"),
}


def read_sector(f, sector: int) -> bytes:
    f.seek(sector * SECTOR_SIZE)
    raw = f.read(SECTOR_SIZE)
    if len(raw) < SECTOR_SIZE:
        return b""
    mode = raw[15]
    if mode == 1:
        return raw[16:16 + DATA_SIZE]
    if mode == 2:
        submode = raw[18]
        if submode & 0x20:
            return raw[24:24 + 2324]
        return raw[24:24 + DATA_SIZE]
    return raw[16:16 + DATA_SIZE]


class Disc:
    def __init__(self, bin_path: Path):
        self.f = open(bin_path, "rb")
        pvd = read_sector(self.f, PVD_SECTOR)
        if pvd[1:6] != b"CD001":
            raise SystemExit(f"{bin_path}: not an ISO9660 data track (no CD001 at sector 16)")
        root = pvd[156:190]
        self.root_lba = struct.unpack("<I", root[2:6])[0]
        self.root_size = struct.unpack("<I", root[10:14])[0]

    def files(self):
        out = []
        self._walk(self.root_lba, self.root_size, "", out)
        return out

    def _walk(self, lba, size, prefix, out):
        sectors = (size + DATA_SIZE - 1) // DATA_SIZE
        data = b"".join(read_sector(self.f, lba + i) for i in range(sectors))
        off = 0
        while off < len(data):
            length = data[off]
            if length == 0:
                off += 1
                while off < len(data) and data[off] == 0:
                    off += 1
                continue
            rec = data[off:off + length]
            off += length
            if len(rec) < 34:
                continue
            ext_lba = struct.unpack("<I", rec[2:6])[0]
            ext_size = struct.unpack("<I", rec[10:14])[0]
            flags = rec[25]
            name_len = rec[32]
            name = rec[33:33 + name_len].decode("ascii", "replace")
            if name in (".", "..", "", "\x00", "\x01"):
                continue
            name = name.split(";")[0]
            full = f"{prefix}{name}"
            if flags & 2:
                self._walk(ext_lba, ext_size, full + "/", out)
            else:
                out.append((full, ext_lba, ext_size))

    def read_file(self, lba, size):
        sectors = (size + DATA_SIZE - 1) // DATA_SIZE
        blob = b"".join(read_sector(self.f, lba + i) for i in range(sectors))
        return blob[:size]


def find_track1(seven_zip: Path, workdir: Path) -> Path:
    """Extract the data track out of a .7z and return its path."""
    if shutil.which("7z") is None:
        raise SystemExit("7z not found: install p7zip, or pass --bin explicitly")
    subprocess.run(["7z", "x", "-y", f"-o{workdir}", str(seven_zip)],
                   check=True, stdout=subprocess.DEVNULL)
    candidates = [p for p in workdir.rglob("*.bin") if "track 1" in p.name.lower()]
    if len(candidates) != 1:
        raise SystemExit(f"expected one 'Track 1' .bin inside {seven_zip}, found {len(candidates)}")
    return candidates[0]


def verify(out_dir: Path) -> bool:
    ok = True
    checked = 0
    for name, (size, digest_prefix) in DISC1_MANIFEST.items():
        path = out_dir / name
        if not path.exists():
            print(f"  verify: MISSING {name}", file=sys.stderr)
            ok = False
            continue
        data = path.read_bytes()
        if len(data) != size:
            print(f"  verify: {name} size {len(data)} != {size}", file=sys.stderr)
            ok = False
            continue
        digest = hashlib.sha256(data).hexdigest()
        if not digest.startswith(digest_prefix):
            print(f"  verify: {name} sha256 {digest[:16]} != {digest_prefix}…", file=sys.stderr)
            ok = False
            continue
        checked += 1
    print(f"  verify: {checked}/{len(DISC1_MANIFEST)} PRGs match the USA Disc 1 manifest")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--bin", type=Path, help="data track .bin (2352-byte sectors)")
    src.add_argument("--7z", dest="archive", type=Path, help="archive holding the disc tracks")
    ap.add_argument("--out", type=Path, required=True, help="destination data directory")
    ap.add_argument("--no-verify", action="store_true", help="skip the Disc 1 hash gate")
    ap.add_argument("--force", action="store_true", help="re-extract even if out looks populated")
    args = ap.parse_args()

    tmp = None
    if args.archive:
        tmp = Path(tempfile.mkdtemp(prefix="pds-disc-"))
        bin_path = find_track1(args.archive, tmp)
    else:
        bin_path = args.bin

    args.out.mkdir(parents=True, exist_ok=True)
    marker = args.out / ".extracted"
    if marker.exists() and not args.force:
        print(f"{args.out} already populated (pass --force to redo)")
    else:
        disc = Disc(bin_path)
        files = disc.files()
        print(f"extracting {len(files)} files from {bin_path.name} -> {args.out}")
        total = 0
        for name, lba, size in files:
            dest = args.out / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(disc.read_file(lba, size))
            total += size
        marker.write_text(f"{bin_path.name}\n{len(files)} files\n{total} bytes\n")
        print(f"wrote {len(files)} files, {total / 1048576:.1f} MiB")

    if tmp is not None:
        shutil.rmtree(tmp, ignore_errors=True)

    if not args.no_verify:
        if not verify(args.out):
            print("extract_disc: HASH GATE FAILED - this is not a known-good USA Disc 1", file=sys.stderr)
            return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
