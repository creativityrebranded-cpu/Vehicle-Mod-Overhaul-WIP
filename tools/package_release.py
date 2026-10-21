"""Package the installable mod and standalone source; no game or external libraries needed."""
import argparse
import hashlib
import os
from pathlib import Path
import zipfile


def write_zip(destination, files):
    temporary = destination.with_suffix(destination.suffix + ".building")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for source, name in sorted(files, key=lambda item: item[1]):
            archive.write(source, name)
    with zipfile.ZipFile(temporary) as archive:
        corrupt = archive.testzip()
        if corrupt:
            raise RuntimeError(f"Archive verification failed: {corrupt}")
    temporary.replace(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = "0.63.1"
    binary = output / f"FlyingVehicleOverhaul-{version}.zip"
    source_zip = output / f"FlyingVehicleOverhaul-{version}-source.zip"
    files = [(root / "Data" / name, name) for name in (
        "SeamlessPilots.esm", "SFSE/Plugins/SeamlessVehicle.dll")]
    for source, _ in files:
        if not source.is_file():
            raise FileNotFoundError(source)
    write_zip(binary, files)
    source_root = root / "source"
    source_files = []
    for folder, directories, names in os.walk(source_root):
        directories[:] = [name for name in directories if name not in (
            "build", ".git", "__pycache__") and not name.startswith("build-")]
        for name in names:
            path = Path(folder) / name
            if path.suffix.lower() not in (".pdb", ".obj", ".exe", ".dll", ".pyc"):
                source_files.append((path, path.relative_to(source_root).as_posix()))
    write_zip(source_zip, source_files)
    with zipfile.ZipFile(binary) as archive:
        if archive.read("SFSE/Plugins/SeamlessVehicle.dll") != files[1][0].read_bytes():
            raise RuntimeError("Packaged plugin differs from repository plugin")
    sums = output / f"FlyingVehicleOverhaul-{version}-SHA256SUMS.txt"
    sums.write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        for path in (binary, source_zip)), encoding="ascii")
    for path in (binary, source_zip, sums):
        print(f"{path.name}: {path.stat().st_size:,} bytes")
    print(f"Standalone source: {len(source_files)} files")


if __name__ == "__main__":
    main()
