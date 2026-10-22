"""Builds "FlyingVehicleOverhaul - Placements.esm": the mod's own vehicle placements as ordinary plugin references, the way
Carfield and Parked Vehicles place theirs (2026-10-21, the user: "do we use the same placement style as Carfield?" -> no: the DLL
created them at run time into the save; this replaces that).

Layout copied from carfield.esm, which places vehicles in New Atlantis too:
  GRUP WRLD > WRLD override (copied from Starfield.esm) > world children (type 1) > exterior block (4) > sub-block (5)
  > CELL override (copied) > cell children (6) > temporary children (9) > REFR (NAME base + DATA position, rotation).

Input: the scanner T recordings (plugin/placements/*.tsv), merged and lined up in rows by bake_own_spots.py. They were recorded in
the planet frame the game uses at run time; a city worldspace is laid over its planet whole cells away (New Atlantis 0001251B:
-7, +17 cells in x / y; -238.97 m in z, measured from Parked Vehicles' references against their live vehicles). Here they go
back into the city's own frame and into the cell that holds each position.

Every spot gets the REV-8 (SFBGS004.esm, shipped with the game): the same vehicle in a row, parked in parallel.

    python scripts/make_placements_esm.py [out.esm]      # default: plugin/build/FlyingVehicleOverhaul - Placements.esm
"""
import glob, math, mmap, os, struct, sys, zlib, collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bake_own_spots as own            # merge, rows, align
import bake_placement_spots as spots    # DATA path, subrecords, cell_grids

STARFIELD = os.path.join(spots.DATA, "Starfield.esm")
NAME = "FlyingVehicleOverhaul - Placements.esm"
MASTERS = ["Starfield.esm", "SFBGS004.esm"]
REV8 = 0xFE000033            # SFBGS004.esm Rev8 (a light master: FE + light-master index 0 + local id; as Vehicles - Real.esm writes it)
CELL_SIZE = 100.0
# Planet world editor id prefix -> (city world file, local id, z offset). x / y shifts are measured from the recordings.
OVERLAYS = {"JemisonPlanetWorld": ("Starfield.esm", 0x01251B, -238.97)}
FIRST_ID = 0x800
TAIL = struct.pack("<IHH", 0, 0x245, 0)   # timestamp, form version 581, version control


def sub(t, v):
    if len(v) > 0xFFFF:
        return b"XXXX" + struct.pack("<HI", 4, len(v)) + t + struct.pack("<H", 0) + v
    return t + struct.pack("<H", len(v)) + v


def split(body):
    """[(tag, value)] of a record body, XXXX sizes resolved."""
    out, o, big = [], 0, None
    while o < len(body):
        t, n = body[o:o + 4], struct.unpack_from("<H", body, o + 4)[0]
        o += 6
        if t == b"XXXX":
            big = struct.unpack_from("<I", body, o)[0]
            o += n
            continue
        if big is not None:
            n, big = big, None
        out.append((t, body[o:o + n]))
        o += n
    return out


# Localized names of the overridden worlds (Starfield.esm stores a string id; this file is not localized).
WORLD_NAMES = {0x01251B: "New Atlantis"}


def ck_style(rec):
    """A master's WRLD / CELL record rewritten the way the Creation Kit saves an override (compared with carfield.esm,
    2026-10-21). A raw copy crashed the game in cell streaming (FUN_14041EFA0, null worldspace): the WRLD carried
    Starfield.esm's own large-reference index (RNAM), file offset table (OFST) and cell sizes (CLSZ), which the game then
    reads against this file. The CK leaves those out and keeps the master's. Cells: no XWCN / XWCU, XCLC's 3
    trailing pad bytes zeroed. (The CK also sets 0x0008 in DATA's second word on some cells, with or without references in
    them, so it is left as the master has it.)"""
    t, size, flags, fid = rec[:4], *struct.unpack_from("<III", rec, 4)
    body = rec[24:]
    if flags & 0x40000:
        body = zlib.decompress(body[4:])
    out = []
    for tag, v in split(body):
        if t == b"WRLD":
            if tag in (b"RNAM", b"OFST", b"CLSZ"):
                continue
            if tag == b"FULL" and len(v) == 4:
                if fid & 0xFFFFFF not in WORLD_NAMES:
                    sys.exit(f"no name known for world {fid:08X}")
                v = WORLD_NAMES[fid & 0xFFFFFF].encode() + b"\0"
        else:
            if tag in (b"XWCN", b"XWCU"):
                continue
            if tag == b"XCLC" and len(v) == 12:
                v = v[:9] + b"\0\0\0"
        out.append(sub(tag, v))
    body = b"".join(out)
    if flags & 0x40000:
        body = struct.pack("<I", len(body)) + zlib.compress(body)
    return t + struct.pack("<III", len(body), flags, fid) + rec[16:24] + body


def grup(label, gtype, content):
    return b"GRUP" + struct.pack("<I", 24 + len(content)) + label + struct.pack("<iIHH", gtype, 0, 0x245, 0) + content


CLEAR = 4.0  # m: a spot this close to another placement mod's vehicle is left out (it would stand inside it with that mod loaded)


def OtherVehicles(wfile, wlocal):
    """(mod, x, y, z) of the vehicles Carfield and Parked Vehicles place in this worldspace (installed copies; none when absent)."""
    out = []
    for plugin, spec in spots.MODS.items():
        path = os.path.join(spots.DATA, plugin)
        if not os.path.exists(path):
            continue
        masters, recs = spots.read(path)
        for t, fid, body, world in recs:
            if t != "REFR" or world is None or spots.owner(masters, plugin, world) != (wfile, wlocal) and                     spots.owner(masters, plugin, world)[1] != wlocal:
                continue
            n, d = spots.first(body, b"NAME"), spots.first(body, b"DATA")
            if n and d and len(d) >= 12 and struct.unpack_from("<I", n)[0] in spec["bases"]:
                out.append((plugin,) + struct.unpack_from("<3f", d))
    return out


def raw_records(path, wanted_world, wanted_cells):
    """The WRLD record and the wanted CELL records of that world, as raw bytes (header + body), from a master."""
    out = {}
    with open(path, "rb") as fh:
        d = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
        hsz = struct.unpack_from("<I", d, 4)[0]

        def walk(o, e, inside):
            while o < e:
                t = d[o:o + 4]
                s = struct.unpack_from("<I", d, o + 4)[0]
                if t == b"GRUP":
                    label, gt = struct.unpack_from("<Ii", d, o + 8)
                    if gt == 0 and struct.pack("<I", label) != b"WRLD":
                        o += s
                        continue
                    if gt == 1 and label != wanted_world:
                        o += s
                        continue
                    walk(o + 24, o + s, inside or gt == 1)
                    o += s
                    continue
                fid = struct.unpack_from("<I", d, o + 12)[0]
                if (t == b"WRLD" and fid == wanted_world) or (t == b"CELL" and inside and fid in wanted_cells):
                    out[fid] = ck_style(bytes(d[o:o + 24 + s]))
                o += 24 + s
        walk(24 + hsz, len(d), False)
    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "plugin", "build", NAME)
    tsvs = sorted(glob.glob(os.path.join(HERE, "..", "plugin", "placements", "*.tsv")))
    recorded = own.merge(own.load(tsvs))
    for g in own.rows(recorded):
        own.align(g)
    by_world = collections.defaultdict(list)
    for s in recorded:
        key = next((k for k in OVERLAYS if s["world"].startswith(k)), None)
        if not key:
            sys.exit("no overlay known for " + s["world"])
        by_world[OVERLAYS[key]].append(s)

    refs = collections.defaultdict(list)     # city cell local id -> [(x, y, z, heading)]
    worlds = {}
    for (wfile, wlocal, zoff), ss in by_world.items():
        cells = spots.cell_grids(wfile, {(wfile.lower(), wlocal)})
        grid = {(gx, gy): cl for cf, cl, wf, wl, gx, gy in cells}
        cell_grid = {cl: (gx, gy) for cf, cl, wf, wl, gx, gy in cells}
        # The x / y shift in whole cells: where each recording falls on the planet minus the grid of the cell it was recorded in.
        votes = collections.Counter()
        for s in ss:
            g = cell_grid.get(s["cell"] & 0xFFFFFF)
            if g:
                votes[(math.floor(s["x"] / CELL_SIZE) - g[0], math.floor(s["y"] / CELL_SIZE) - g[1])] += 1
        (sx, sy), n = votes.most_common(1)[0]
        print(f"{wfile} {wlocal:06X}: shift {sx}, {sy} cells ({n} of {len(ss)} recordings agree), z {zoff}")
        others = OtherVehicles(wfile, wlocal)
        for s in ss:
            x, y, z = s["x"] - sx * CELL_SIZE, s["y"] - sy * CELL_SIZE, s["z"] - zoff
            clash = next((o for o in others if math.dist(o[1:], (x, y, z)) < CLEAR), None)
            if clash:
                print(f"  spot {s['key']:016X} left out: {math.dist(clash[1:], (x, y, z)):.1f} m from {clash[0]}'s vehicle")
                continue
            cell = grid.get((math.floor(x / CELL_SIZE), math.floor(y / CELL_SIZE)))
            if cell is None:
                sys.exit(f"no cell at {x:.1f}, {y:.1f}")
            refs[cell].append((x, y, z + 0.3, s["h"]))
        worlds[wlocal] = (wfile, sorted(refs))

    next_id = FIRST_ID
    top = b""
    total = 0
    for wlocal, (wfile, cells) in worlds.items():
        raw = raw_records(STARFIELD, wlocal, set(cells))
        if wlocal not in raw or any(c not in raw for c in cells):
            sys.exit("a WRLD / CELL record was not found in Starfield.esm")
        blocks = collections.defaultdict(lambda: collections.defaultdict(list))
        cell_grid = {cl: (gx, gy) for cf, cl, wf, wl, gx, gy in spots.cell_grids(wfile, {(wfile.lower(), wlocal)}) if cl in set(cells)}
        for cl in cells:
            gx, gy = cell_grid[cl]
            blocks[(gx // 32, gy // 32)][(gx // 8, gy // 8)].append(cl)
        world_children = b""
        for (bx, by), subs in sorted(blocks.items()):
            block = b""
            for (ux, uy), cls in sorted(subs.items()):
                sub_block = b""
                for cl in cls:
                    temp = b""
                    for x, y, z, h in refs[cl]:
                        body = sub(b"NAME", struct.pack("<I", REV8)) + sub(b"DATA", struct.pack("<6f", x, y, z, 0.0, 0.0, h))
                        temp += b"REFR" + struct.pack("<III", len(body), 0, (len(MASTERS) << 24) | next_id) + TAIL + body
                        next_id += 1
                        total += 1
                    label = struct.pack("<I", cl)
                    sub_block += raw[cl] + grup(label, 6, grup(label, 9, temp))
                block += grup(struct.pack("<hh", uy, ux), 5, sub_block)
            world_children += grup(struct.pack("<hh", by, bx), 4, block)
        top += raw[wlocal] + grup(struct.pack("<I", wlocal), 1, world_children)
    wrld = grup(b"WRLD", 0, top)

    with open(STARFIELD, "rb") as fh:
        head = fh.read(64)
    hedr_ver = struct.unpack_from("<f", head, 24 + 6)[0]
    items = [(b"HEDR", struct.pack("<fII", hedr_ver, total + sum(len(c) for _, c in worlds.values()) + len(worlds), next_id)),
             (b"CNAM", b"Flying Vehicle Overhaul\0")]
    for m in MASTERS:
        items += [(b"MAST", m.encode() + b"\0"), (b"DATA", struct.pack("<Q", 0))]
    tes4 = b"".join(sub(t, v) for t, v in items)
    header = b"TES4" + struct.pack("<II", len(tes4), 0x101) + head[12:24]   # ESM + light, like carfield.esm
    data = header + tes4 + wrld
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    open(out_path, "wb").write(data)
    print(f"{total} vehicles in {sum(len(c) for _, c in worlds.values())} cells -> {os.path.normpath(out_path)} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
