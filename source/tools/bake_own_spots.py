"""Bake the mod's own vehicle placements (recorded with scanner T) into plugin/src/own_spots.h.

The recorder (realvehicles.cpp RecordPlacement) writes placement-markers.tsv: the surface the player aimed at, the player's
heading and the cell the player stood in. Positions are in the frame the game's references use on the planet (for New
Atlantis that is the city worldspace 0001251B moved by (-700, 1700, -239); Parked Vehicles' baked spots line up with
their live vehicles at exactly that offset), so they are spawned as recorded.

Rows (the user, 2026-10-21: markers placed side by side are meant as one row of the same vehicle, parked in parallel):
  - a tap within kMerge of an earlier one is a re-tap: the later one replaces it;
  - markers within kLink of each other (chained) form a row; a row gets one vehicle type;
  - a row that lies on a line gets one heading: square to the line when the taps were roughly square to it, else
    their mean heading.

    python scripts/bake_own_spots.py [placement-markers.tsv ...]   # default: placements/*.tsv
"""
import glob, hashlib, math, os, struct, sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
TREE = os.path.join(ROOT, "plugin") if os.path.isdir(os.path.join(ROOT, "plugin")) else ROOT  # the repo, or the standalone source
OUT = os.path.join(TREE, "src", "own_spots.h")
kMerge = 2.0      # m: closer than this is the same spot tapped twice
kLink = 9.0       # m: neighbours in one row
kLinkDz = 1.5     # m: and on the same level
kLineSpread = 1.5 # m: a row is a line when its spread across the line is under this
kSquare = math.radians(35)  # taps within this of square to the line are squared

FILES = ["Starfield.esm"]  # a cell id's load index -> its file (only Starfield.esm cells are recorded so far)

def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi

def circ_mean(hs):
    return math.atan2(sum(math.sin(h) for h in hs), sum(math.cos(h) for h in hs))

def h64(text):
    return struct.unpack_from('<Q', hashlib.sha1(text.encode()).digest())[0]

def load(paths):
    spots = []
    for path in paths:
        with open(path, encoding='utf-8') as f:
            head = f.readline().rstrip('\n').split('\t')
            for line in f:
                if not line.strip(): continue
                r = dict(zip(head, line.rstrip('\n').split('\t')))
                spots.append(dict(key=int(r['key'], 16), world=r['world_editor_id'], cell=int(r['player_cell_runtime'], 16),
                                  x=float(r['x']), y=float(r['y']), z=float(r['z']), h=float(r['yaw']), label=r.get('label', '')))
    return spots

def merge(spots):
    kept = []
    for s in spots:
        near = [k for k in kept if k['world'] == s['world'] and math.dist((k['x'], k['y'], k['z']), (s['x'], s['y'], s['z'])) < kMerge]
        for k in near:
            print(f"  re-tap: {k['key']:016X} replaced by {s['key']:016X} ({math.dist((k['x'], k['y'], k['z']), (s['x'], s['y'], s['z'])):.1f} m)")
            kept.remove(k)
        kept.append(s)
    return kept

def rows(spots):
    parent = list(range(len(spots)))
    def find(i):
        while parent[i] != i: parent[i] = parent[parent[i]]; i = parent[i]
        return i
    for i, a in enumerate(spots):
        for j in range(i + 1, len(spots)):
            b = spots[j]
            if a['world'] == b['world'] and math.hypot(a['x'] - b['x'], a['y'] - b['y']) <= kLink and abs(a['z'] - b['z']) <= kLinkDz:
                parent[find(i)] = find(j)
    groups = {}
    for i in range(len(spots)): groups.setdefault(find(i), []).append(spots[i])
    return list(groups.values())

def align(row):
    """One heading for a row on a line; returns a description."""
    if len(row) < 2: return "single"
    mx = sum(s['x'] for s in row) / len(row); my = sum(s['y'] for s in row) / len(row)
    sxx = sum((s['x'] - mx) ** 2 for s in row); syy = sum((s['y'] - my) ** 2 for s in row); sxy = sum((s['x'] - mx) * (s['y'] - my) for s in row)
    ang = 0.5 * math.atan2(2 * sxy, sxx - syy)          # major axis, measured from +x
    ax, ay = math.cos(ang), math.sin(ang)
    across = math.sqrt(sum(((s['x'] - mx) * -ay + (s['y'] - my) * ax) ** 2 for s in row) / len(row))
    if across > kLineSpread: return f"not a line (spread {across:.1f} m), headings kept"
    axis = math.atan2(ax, ay)                           # the line as a game heading (forward = (sin h, cos h))
    mean = circ_mean([s['h'] for s in row])
    square = min((wrap(axis + d) for d in (math.pi / 2, -math.pi / 2)), key=lambda p: abs(wrap(mean - p)))
    target = square if abs(wrap(mean - square)) <= kSquare else mean
    for s in row: s['h'] = wrap(target)
    return f"{'square to the line' if target == square else 'mean heading'} {target:.3f}"

def main():
    paths = sys.argv[1:] or sorted(glob.glob(os.path.join(TREE, "placements", "*.tsv")))
    if not paths: sys.exit("no placement-markers.tsv given")
    spots = load(paths)
    print(f"{len(spots)} recorded markers")
    spots = merge(spots)
    groups = sorted(rows(spots), key=lambda g: min(s['key'] for s in g))
    out = []
    for g in groups:
        how = align(g)
        row = h64("row|" + "|".join(f"{s['key']:016X}" for s in sorted(g, key=lambda s: s['key'])))
        cells = sorted({s['cell'] for s in g})
        print(f"  row {row:016X}: {len(g)} spot(s), cells {' '.join(f'{c:08X}' for c in cells)}, {how}")
        for s in g:
            fi = s['cell'] >> 24
            if fi >= len(FILES): sys.exit(f"cell {s['cell']:08X}: load index {fi:02X} is not in FILES")
            out.append((fi, s['cell'] & 0xFFFFFF, s, row, h64("location|" + s['world'])))
    with open(OUT, 'w', newline='\n') as f:
        f.write("#pragma once\n// Generated by scripts/bake_own_spots.py from placements/*.tsv (scanner T recordings). Do not edit by hand.\n")
        f.write("// The mod's own vehicle placements: the cell the spot was recorded from (file + local id), the place in the planet\n")
        f.write("// frame the game's references use, the heading, the spot's key, its row (one vehicle type per row) and its location\n")
        f.write("// (the type mix). Spawned by realvehicles.cpp when the cell is attached.\n")
        f.write("#include <cstdint>\nnamespace ownspots {\n")
        f.write("struct Spot { std::uint8_t file; std::uint32_t cellLocal; float x, y, z, heading; std::uint64_t key, row, location; };\n")
        f.write("inline constexpr const char* kFiles[] = { " + ", ".join(f'"{n}"' for n in FILES) + " };\n")
        f.write(f"inline constexpr unsigned kSpotCount = {len(out)};\n")
        f.write("inline constexpr Spot kSpots[] = {\n")
        for fi, cl, s, row, loc in out:
            f.write(f"    {{ {fi}, 0x{cl:06X}, {s['x']:.3f}f, {s['y']:.3f}f, {s['z']:.3f}f, {s['h']:.4f}f, 0x{s['key']:016X}ull, 0x{row:016X}ull, 0x{loc:016X}ull }},\n")
        f.write("};\n}\n")
    print(f"{len(out)} spots in {len(groups)} rows -> {os.path.normpath(OUT)}")

if __name__ == "__main__":
    main()
