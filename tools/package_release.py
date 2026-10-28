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
    parser.add_argument("--output-dir", type=Path, required=True, help="Release root; a version-named subfolder is created automatically")
    parser.add_argument("--version", required=True, help="e.g. 0.72.0")
    parser.add_argument("--traffic-content", choices=("auto", "air", "legacy"), default="auto", help="Legacy format only")
    parser.add_argument("--format", choices=("bundle", "fomod", "legacy"), default="bundle")
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--source-dir", type=Path)
    parser.add_argument("--sfse-dir", type=Path)
    parser.add_argument("--schema", type=Path)
    parser.add_argument("--test-results", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    from package_release_bundle import build_release, version_folder
    output=version_folder(args.output_dir,args.version)
    if args.format in ("bundle","fomod"):
        if args.traffic_content != "auto":
            parser.error("--traffic-content requires --format legacy; the current FOMOD offers parked vehicles only")
        from package_fomod import build
        builder=build_release if args.format=="bundle" else build
        builder(args.data_dir or root / "Data", args.source_dir or root / "source",
              args.sfse_dir or root / "source/third_party/sfse", args.output_dir.resolve() if args.format=="bundle" else output,
              args.version, args.schema, args.test_results)
        return
    output.mkdir(parents=True, exist_ok=True)
    version = args.version
    binary = output / f"FlyingVehicleOverhaul-{version}.zip"
    source_zip = output / f"FlyingVehicleOverhaul-{version}-source.zip"
    use_air = args.traffic_content == "air" or (args.traffic_content == "auto" and (root / "Data/FVO_AirTraffic.esm").is_file())
    city = "FVO_AirTraffic.esm" if use_air else ("FVO_CityTraffic.esm" if (root / "Data/FVO_CityTraffic.esm").is_file() else "FVO_CityPlacements.esm")
    files = [(root / "Data" / name, name) for name in (
        "FVO_NPCPilots.esm", city, "SFSE/Plugins/FlyingVehicleOverhaul.dll")]
    for source, _ in files:
        if not source.is_file():
            raise FileNotFoundError(source)
    guide = root / "docs" / f"NEXUS_DESCRIPTION_{version}.txt"
    if guide.is_file():
        files.append((guide, "Docs/FlyingVehicleOverhaul.txt"))
    if use_air:
        air_guide = root / "docs/AIR_TRAFFIC.md"
        if not air_guide.is_file():
            raise FileNotFoundError(air_guide)
        files.append((air_guide, "Docs/AirTraffic.md"))
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
        if archive.read("SFSE/Plugins/FlyingVehicleOverhaul.dll") != files[2][0].read_bytes():
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
