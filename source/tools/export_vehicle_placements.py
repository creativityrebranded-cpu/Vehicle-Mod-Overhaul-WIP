"""Convert scanner recordings to this mod's own placement dataset for review and baking.

python export_vehicle_placements.py placement-markers.tsv --output native-vehicle-placements.json
Coordinates are kept in native world metres; runtime load-order prefixes are never baked.
This exports positions only. It does not spawn vehicles or modify a save.
"""
import argparse
import csv
import json
import math
from pathlib import Path


def convert(path):
    placements = {}
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        for line, row in enumerate(csv.DictReader(stream, delimiter="\t"), 2):
            master = row["world_file"]
            if master == "UNRESOLVED" or Path(master).name != master or not master.lower().endswith((".esm", ".esp", ".esl")):
                raise ValueError(f"Line {line}: worldspace master must be resolved before baking")
            local = int(row["world_local"], 16)
            if not 0 < local <= 0xFFFFFF:
                raise ValueError(f"Line {line}: invalid file-local worldspace ID")
            position = [float(row[axis]) for axis in ("x", "y", "z")]
            yaw = float(row["yaw"])
            if not all(math.isfinite(value) and abs(value) <= 1e7 for value in position) or not math.isfinite(yaw):
                raise ValueError(f"Line {line}: invalid position or heading")
            key = f"{int(row['key'], 16):016X}"
            point = {
                "id": f"marker_{key}",
                "worldspace": {"master": master, "local_id": f"{local:06X}", "editor_id": row["world_editor_id"]},
                "position_m": position,
                "heading_rad": math.remainder(yaw, 2 * math.pi),
                "label": row.get("label", ""),
            }
            if key in placements and placements[key] != point:
                raise ValueError(f"Line {line}: conflicting records for placement {key}")
            placements[key] = point
    return {"format": "flying-vehicle-overhaul.placements.v1", "placements": list(placements.values())}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    data = convert(args.recording)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".building")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(args.output)
    print(f"Stored {len(data['placements'])} native placement positions in {args.output}")


if __name__ == "__main__":
    main()
