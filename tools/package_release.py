"""Package a native Windows/Linux executable together with matching source."""
from pathlib import Path
import argparse
import hashlib
import gzip
import io
import re
import subprocess
import sys
import tarfile
import zipfile


def main():
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("windows-x64", "linux-x64"),
                        default="windows-x64" if sys.platform == "win32" else "linux-x64")
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output", type=Path, default=repository / "out/release")
    args = parser.parse_args()
    version_text = (repository / "src/version.h").read_text(encoding="utf-8")
    version = re.search(r'version = "([0-9.]+)"', version_text).group(1)
    executable = "nevtoraw.exe" if args.platform == "windows-x64" else "nevtoraw"
    binary = (args.binary or repository / executable).resolve()
    header = binary.read_bytes()[:4096]
    if args.platform == "linux-x64":
        if header[:6] != b"\x7fELF\x02\x01" or int.from_bytes(header[18:20], "little") != 62:
            parser.error("expected a little-endian Linux x86-64 ELF executable")
    else:
        pe = int.from_bytes(header[60:64], "little")
        if header[:2] != b"MZ" or header[pe:pe+6] != b"PE\0\0\x64\x86":
            parser.error("expected a Windows x64 PE executable")
    reported = subprocess.check_output([str(binary), "--version"], text=True).strip()
    if reported != f"nevtoraw {version}":
        parser.error("binary version differs from src/version.h; rebuild first")

    files = []
    for name in ("src", "third_party", "tests", "docs", "tools", ".github"):
        files.extend(p for p in (repository / name).rglob("*") if p.is_file() and
                     "__pycache__" not in p.parts and p.suffix not in (".exe", ".o", ".pyc", ".part"))
    files.extend(repository / name for name in (
        "README.md", "CHANGELOG.md", "COPYING", "THIRD_PARTY.md", "Makefile", ".gitignore", ".gitattributes"))
    files = sorted(files, key=lambda p: p.relative_to(repository).as_posix())
    compiled = [p for p in files if p.relative_to(repository).parts[0] in ("src", "third_party") and
                p.suffix in (".cpp", ".h", ".inc")]
    if any(p.stat().st_mtime_ns > binary.stat().st_mtime_ns for p in compiled):
        parser.error("binary is older than its sources; rebuild first")
    payloads = {p.relative_to(repository).as_posix(): p.read_bytes() for p in files}
    checks = "".join(f"{hashlib.sha256(data).hexdigest()}  {name}\n" for name, data in payloads.items())
    payloads["SOURCE-SHA256.txt"] = checks.encode()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    archives = []
    for kind in ("source", args.platform):
        stem = f"nevtoraw-{version}-{kind}"
        contents = dict(payloads)
        if kind != "source":
            contents[executable] = binary.read_bytes()
        if kind == "linux-x64":
            path = output / f"{stem}.tar.gz"
            with path.open("wb") as file, gzip.GzipFile(filename="", fileobj=file, mode="wb", mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                    for name, data in sorted(contents.items()):
                        entry = tarfile.TarInfo(f"{stem}/{name}")
                        entry.size = len(data)
                        entry.mode = 0o755 if name == executable else 0o644
                        archive.addfile(entry, io.BytesIO(data))
            with tarfile.open(path) as archive:
                for entry in archive:
                    assert archive.extractfile(entry).read() == contents[entry.name.split("/", 1)[1]]
        else:
            path = output / f"{stem}.zip"
            with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
                for name, data in sorted(contents.items()):
                    entry = zipfile.ZipInfo(f"{stem}/{name}", date_time=(1980, 1, 1, 0, 0, 0))
                    entry.create_system = 3
                    entry.compress_type = zipfile.ZIP_DEFLATED
                    entry.external_attr = (0o100755 if name == executable else 0o100644) << 16
                    archive.writestr(entry, data, compresslevel=9)
            with zipfile.ZipFile(path) as archive:
                assert archive.testzip() is None
        archives.append(path)
        print(path)
    (output / "SHA256SUMS.txt").write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in archives), encoding="ascii")


if __name__ == "__main__":
    main()
