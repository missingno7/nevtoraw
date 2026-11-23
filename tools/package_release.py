"""Create deterministic source and Windows archives from this working tree."""
from pathlib import Path
import argparse
import hashlib
import re
import subprocess
import zipfile


def main():
    repository = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=repository / "nevtoraw.exe")
    parser.add_argument("--output", type=Path, default=repository / "out/release")
    args = parser.parse_args()
    version_text = (repository / "src/version.h").read_text(encoding="utf-8")
    version = re.search(r'version = "([0-9.]+)"', version_text).group(1)
    binary = args.binary.resolve()
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
    for kind in ("source", "windows-x64"):
        stem = f"nevtoraw-{version}-{kind}"
        path = output / f"{stem}.zip"
        contents = dict(payloads)
        if kind == "windows-x64":
            contents["nevtoraw.exe"] = binary.read_bytes()
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for name, data in sorted(contents.items()):
                entry = zipfile.ZipInfo(f"{stem}/{name}", date_time=(1980, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = 0o100644 << 16
                archive.writestr(entry, data, compresslevel=9)
        with zipfile.ZipFile(path) as archive:
            assert archive.testzip() is None
        archives.append(path)
        print(path)
    (output / "SHA256SUMS.txt").write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n" for path in archives), encoding="ascii")


if __name__ == "__main__":
    main()
