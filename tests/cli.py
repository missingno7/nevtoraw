"""Self-contained CLI regression checks. Run after raw-input-test.exe."""
from pathlib import Path
import hashlib
import shutil
import struct
import subprocess
import sys
import array
import importlib.util

binary = Path(sys.argv[1] if len(sys.argv) > 1 else "nevtoraw.exe").resolve()
root = Path("out/cli-tests")
assert root.resolve().parent == Path("out").resolve()
if root.exists():
    shutil.rmtree(root)
root.mkdir(parents=True, exist_ok=True)


def run(*args, code=0):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout + result.stderr


def tags(path):
    data = path.read_bytes()
    assert data[:2] == b"II"
    offset = struct.unpack_from("<I", data, 4)[0]
    count = struct.unpack_from("<H", data, offset)[0]
    return {tag: (kind, n, value) for tag, kind, n, value in
            struct.iter_unpack("<HHI4s", data[offset + 2:offset + 2 + 12 * count])}


def integers(path, tag):
    kind, count, pointer = tags(path)[tag]
    width, fmt = {3: (2, "H"), 4: (4, "I")}[kind]
    payload = pointer[:count*width] if count*width <= 4 else path.read_bytes()[struct.unpack("<I", pointer)[0]:][:count*width]
    return struct.unpack("<"+fmt*count, payload)


version = run("--version").strip()
assert version == "nevtoraw 0.2.0", version
catalog = run("cameras")
assert "98 Nikon RGB calibrations" in catalog and "Nikon Z 7 2" in catalog and "Nikon D750" in catalog
run("cameras", "unexpected", code=2)

spec = importlib.util.spec_from_file_location("nikon_import", "tools/import_nikon_calibrations.py")
nikon_import = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nikon_import)
profile = nikon_import.read_profile(Path("out/raw-input-tests/calibration.dcp"))
assert profile["model"] == "Nikon Fixture RGB" and profile["cm1"] == [1,0,0,0,1,0,0,0,1]
assert profile["cm2"] == [2,0,0,0,2,0,0,0,2]
assert nikon_import.camera_key("NIKON Z 7_2") == nikon_import.camera_key("Nikon Z 7 2")
assert nikon_import.camera_key("E5400") == nikon_import.camera_key("Nikon Coolpix 5400")
for command in ([], ["stack"], ["sr"], ["dng"], ["info"]):
    assert "--count" in run(*command, "--help")

source = Path("out/raw-input-tests/plain.dng")
nrw = Path("out/raw-input-tests/native-burst/a.NRW")
run("info", nrw)
run("dng", nrw, "-o", root / "nrw-convert")
run("stack", nrw, "--no-align", "-o", root / "nrw-stack")
run("sr", nrw, "--count", "1", "--crop", "3,5,12,10", "-o", root / "nrw-sr")
for command in ("dng", "stack", "sr"):
    destination = root / ("d1x-" + command)
    arguments = ("--no-align",) if command == "stack" else ()
    run(command, Path("out/raw-input-tests/d1x.nef"), *arguments, "-o", destination)
    output = next(destination.glob("*.dng"))
    kind, count, pointer = tags(output)[50718]
    assert (kind, count) == (5, 2)
    values = struct.unpack_from("<4I", output.read_bytes(), struct.unpack("<I", pointer)[0])
    assert values[0] / values[1] == .75 and values[2] / values[3] == 1.5
unicode_source = root / "\u017elu\u0165ou\u010dk\u00fd sn\u00edmek.dng"
shutil.copyfile(source, unicode_source)
run("info", unicode_source)
run("dng", unicode_source, "-o", root / "\u010desk\u00fd v\u00fdstup")
assert (root / "\u010desk\u00fd v\u00fdstup" / (unicode_source.stem + "_converted.dng")).exists()
burst = root / "burst"
burst.mkdir(exist_ok=True)
for index in range(17):
    shutil.copyfile(source, burst / f"frame_{index:02}.dng")
original = hashlib.sha256((burst / "frame_00.dng").read_bytes()).hexdigest()
assert "17 files sorted by filename" in run("info", burst)

text = run("stack", burst, "--no-align", "--merge", "trimmed-mean", "-o", root / "stack-all")
assert "using 17 of 17 candidates" in text and "reference 8" in text
text = run("stack", burst, "--frames", "0-16", "--count", "4", "--no-align", "-o", root / "stack-four")
assert "selected frames: 0 5 11 16" in text
text = run("stack", burst / "frame_00.dng", "-o", root / "stack-default")
assert "homography alignment" in text

for command in ("dng", "stack"):
    output = root / f"crop-{command}"
    options = ["--no-align"] if command == "stack" else []
    run(command, source, "--crop", "3,5,12,10", "-c", "none", "-o", output, *options)
    path = next(output.glob("*.dng"))
    data = tags(path)
    assert struct.unpack("<I", data[256][2])[0] == 12
    assert struct.unpack("<I", data[257][2])[0] == 10
    assert data[33422][2] == bytes([0,1,1,2])  # BGGR input shifted by odd x/y.
    assert b"crop 3,5,12,10 sensor px" in path.read_bytes()

# Small JPEG crops must keep each encoded row at the physical image width.
# Check an independent raw decoder when one is available locally.
small_jpeg = root / "small-jpeg"
run("dng", source, "--crop", "3,5,96,80", "-o", small_jpeg)
small_file = next(small_jpeg.glob("*.dng"))
assert 273 in tags(small_file) and 324 not in tags(small_file)
if shutil.which("dcraw"):
    raw = subprocess.check_output(["dcraw", "-D", "-4", "-j", "-t", "0", "-c", str(small_file)])
    magic, dimensions, white, samples = raw.split(b"\n", 3)
    assert (magic, dimensions, white) == (b"P5", b"96 80", b"65535")
    contents = source.read_bytes()
    stored = b"".join(contents[offset:offset+count] for offset, count in zip(integers(source, 273), integers(source, 279)))
    expected = b"".join(stored[((y+5)*270+3)*2:((y+5)*270+99)*2] for y in range(80))
    decoded = array.array("H")
    decoded.frombytes(samples)
    if sys.byteorder == "little":
        decoded.byteswap()
    wanted = array.array("H")
    wanted.frombytes(expected)
    assert decoded == wanted, "external decoder changed small JPEG crop rows"

for scale in (1, 2, 3, 4):
    for bayer in (False, True):
        output = root / f"sr-{scale}-{bayer}"
        arguments = ["sr", source, "--count", "1", "--scale", str(scale), "--crop", "3,5,12,10", "-o", output]
        if bayer:
            arguments += ["--bayer"]
        run(*arguments)
        path = next(output.glob("*.dng"))
        data = tags(path)
        assert struct.unpack("<I", data[256][2])[0] == 12 * scale
        assert struct.unpack("<I", data[257][2])[0] == 10 * scale
        assert struct.unpack("<H", data[262][2][:2])[0] == (32803 if bayer else 34892)

# Conversion shares the same directory indexing and selection.
text = run("dng", burst, "--frames", "1-3", "-o", root / "converted")
assert len(list((root / "converted").glob("*.dng"))) == 3
converted = root / "converted/frame_01_converted.dng"
assert version.encode() + b"\0" in converted.read_bytes()

# A valid destination can still be another frame in the input set.
collision = root / "collision"
collision.mkdir(exist_ok=True)
shutil.copyfile(source, collision / "a.dng")
shutil.copyfile(source, collision / "a_converted.dng")
before = (collision / "a_converted.dng").read_bytes()
assert "overwrite a source" in run("dng", collision, "-o", collision, code=1)
assert (collision / "a_converted.dng").read_bytes() == before
assert hashlib.sha256((burst / "frame_00.dng").read_bytes()).hexdigest() == original

for command in ("stack", "sr"):
    for options in (["--count", "0"], ["--count", "18"], ["--counts", "2"],
                    ["--frames", "0-2", "--ref", "3"], ["--frames", "0-2:0"]):
        run(command, burst, *options, code=2)
for options in (["--scale", "0"], ["--scale", "5"], ["--kernel-sigma", "nan"],
                ["--kernel-sigma", "0"], ["--crop", "0,0,0,10"]):
    run("sr", source, *options, code=2)
for options in (["--trim-percent", "20"], ["--no-align", "--align", "homography"],
                ["--no-align", "--tile", "64"], ["--tile", "17"]):
    run("stack", source, *options, code=2)
for command in ("dng", "stack", "sr"):
    run(command, source, "--crop", "0,0,1,2", code=2)
    run(command, source, "--crop", "999999,0,2,2", code=1)

# Execution controls are shared, and cache backing must not alter output.
temporary = root / "scratch"
temporary.mkdir()
for command in ("dng", "stack", "sr"):
    options = ["--no-align"] if command == "stack" else ["--scale", "4"] if command == "sr" else []
    expected = root / (command + "-resident")
    actual = root / (command + "-bounded")
    run(command, source, *options, "--threads", "1", "--memory-limit", "256M", "-c", "none", "-o", expected)
    text = run(command, source, *options, "--threads", "4", "--memory-limit", "24MiB", "--temp-dir", temporary, "-c", "none", "-o", actual)
    assert next(expected.glob("*.dng")).read_bytes() == next(actual.glob("*.dng")).read_bytes(), command
    if command == "sr":
        assert "output cache:" in text and "temporary storage" in text
    assert "RAM budget" in text and "stage work time" in text
    assert not list(temporary.iterdir())
    assert "memory budget too small" in run(command, source, "--memory-limit", "1K", code=1)
for options in (["--threads", "0"], ["--threads", "1025"], ["--threads", "-1"],
                ["--memory-limit", "0"], ["--memory-limit", "1.5G"], ["--memory-limit", "1Z"]):
    run("dng", source, *options, code=2)
run("dng", source, "--temp-dir", root / "missing-directory", code=1)

# Folder outputs stay out of subsequent source scans.
run("dng", burst)
assert "17 files sorted by filename" in run("info", burst)
assert not list(root.rglob("*.part"))
print("CLI tests passed (defaults, selection, scales, formats, errors, source protection)")
