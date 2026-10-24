"""Builds "FVO_ParkedVehiclesPatch.esm" (until 0.72.0 "Vehicles - Real.esm"): a small override patch for Parked Vehicles (Vehicles.esm by SarthesArai) that turns
every pack-in's static vehicle prop into the real vehicle (REV-8 / Deimog / Moon Jumper furniture) and disables the
separate wheel and turret statics the real vehicle already has. Vehicles.esm itself is not changed.

User request 2026-10-17: "the prop shouldnt be there but rather a new vehicle so modify that mod instead" (no swapping
at run time). Masters: Starfield.esm, sfbgs00d.esm, Vehicles.esm, SFBGS004.esm (REV-8), SFBGS047.esm (Moon Jumper),
sfbgs019.esm (Deimog).

python scripts/make_vehicles_real.py <Vehicles.esm> <out.esm>
"""
import struct
import sys
import zlib

# A form in a light master is written FE + the master's index among the LIGHT masters only (12 bits) + its local id
# (12 bits) -- as Bethesda's SFBGS047 writes SFBGS004's VehicleKeyword (FE000017). Light masters here, in order:
# SFBGS004 (0), SFBGS047 (1), sfbgs019 (2). (2026-10-17 20:4x: 0x03000033-style ids left every spot empty in game.)
PROP_TO_VEHICLE = {
    0x0200080B: 0xFE000033,  # Vehicle_Ref8_Static -> SFBGS004 Rev8 (runtime FE001033)
    0x020008DD: 0xFE001001,  # Vehicle_MoonJumper_Static -> SFBGS047 MoonJumper (runtime FE004001)
    0x0200080C: 0xFE002005,  # Vehicle_Deimog_Static -> sfbgs019 SFBGS019_Deimog (runtime FE005005)
}
PARTS = {0x02000ACB, 0x02000AF1, 0x02000AEE, 0x02000AF2, 0x02000AF0, 0x02000AF3}  # wheels / turret statics
MASTERS = ["Starfield.esm", "sfbgs00d.esm", "Vehicles.esm", "SFBGS004.esm", "SFBGS047.esm", "sfbgs019.esm"]
F_COMPRESSED, F_DISABLED, F_ESM, F_LIGHT = 0x40000, 0x800, 0x1, 0x100


def subs(data):
    i, out = 0, []
    while i + 6 <= len(data):
        t = data[i:i + 4]
        n = struct.unpack_from("<H", data, i + 4)[0]
        if t == b"XXXX":
            big = struct.unpack_from("<I", data, i + 6)[0]
            i += 10
            out.append((data[i:i + 4], data[i + 6:i + 6 + big]))
            i += 6 + big
            continue
        out.append((t, data[i + 6:i + 6 + n]))
        i += 6 + n
    return out


def pack_subs(items):
    out = b""
    for t, v in items:
        if len(v) > 0xFFFF:
            out += b"XXXX" + struct.pack("<HI", 4, len(v)) + t + struct.pack("<H", 0) + v
        else:
            out += t + struct.pack("<H", len(v)) + v
    return out


def body_of(d, i):
    sz, fl = struct.unpack_from("<II", d, i + 4)
    raw = d[i + 24:i + 24 + sz]
    return zlib.decompress(raw[4:]) if fl & F_COMPRESSED else raw


def record(header, body, flags):
    # header: the original 24 bytes; size and flags replaced, body stored uncompressed
    return header[:4] + struct.pack("<II", len(body), flags & ~F_COMPRESSED) + header[12:24] + body


def main(src, dst):
    d = open(src, "rb").read()
    hs = struct.unpack_from("<I", d, 4)[0]
    top = 24 + hs

    # pack-in cells: the CNAM of every PKIN
    packin_cells = set()

    def scan(a, b):
        i = a
        while i < b:
            if d[i:i + 4] == b"GRUP":
                sz = struct.unpack_from("<I", d, i + 4)[0]
                scan(i + 24, i + sz)
                i += sz
                continue
            sz = struct.unpack_from("<I", d, i + 4)[0]
            if d[i:i + 4] == b"PKIN":
                for t, v in subs(body_of(d, i)):
                    if t == b"CNAM":
                        packin_cells.add(struct.unpack("<I", v[:4])[0])
            i += 24 + sz

    scan(top, len(d))

    overridden, stats = [], {"vehicle": 0, "part": 0}

    def rewrite_ref(i, in_packin):
        sz, fl, fid = struct.unpack_from("<III", d, i + 4)
        if not in_packin:
            return None
        items = subs(body_of(d, i))
        base = next((struct.unpack("<I", v[:4])[0] for t, v in items if t == b"NAME"), None)
        if base in PROP_TO_VEHICLE:
            items = [(t, struct.pack("<I", PROP_TO_VEHICLE[base]) + v[4:] if t == b"NAME" else v) for t, v in items]
            stats["vehicle"] += 1
            overridden.append(fid)
            return record(d[i:i + 24], pack_subs(items), fl)
        if base in PARTS:
            stats["part"] += 1
            overridden.append(fid)
            return record(d[i:i + 24], pack_subs(items), fl | F_DISABLED)
        return None

    # Walks a group; returns its rebuilt bytes holding only what leads to changed refs (or b"" for none).
    def rebuild(i, cell_ctx):
        sz = struct.unpack_from("<I", d, i + 4)[0]
        gtype = struct.unpack_from("<I", d, i + 12)[0]
        label = struct.unpack_from("<I", d, i + 8)[0]
        if gtype in (6, 8, 9, 10):  # cell children / persistent / temporary
            cell_ctx = label if gtype == 6 else cell_ctx
        out, pending_cell = b"", None
        j, end = i + 24, i + sz
        while j < end:
            if d[j:j + 4] == b"GRUP":
                gsz = struct.unpack_from("<I", d, j + 4)[0]
                g = rebuild(j, cell_ctx)
                if g:
                    if pending_cell:
                        out += pending_cell
                        pending_cell = None
                    out += g
                j += gsz
                continue
            rsz = struct.unpack_from("<I", d, j + 4)[0]
            t = d[j:j + 4]
            if t == b"CELL":
                pending_cell = d[j:j + 24 + rsz]  # kept only if its children change
            elif t == b"REFR":
                r = rewrite_ref(j, cell_ctx in packin_cells)
                if r:
                    out += r
            j += 24 + rsz
        if not out:
            return b""
        return d[i:i + 4] + struct.pack("<I", 24 + len(out)) + d[i + 8:i + 24] + out

    groups = b""
    i = top
    while i < len(d):
        sz = struct.unpack_from("<I", d, i + 4)[0]
        if d[i + 8:i + 12] in (b"CELL", b"WRLD"):
            groups += rebuild(i, None)
        i += sz

    # count records + groups for HEDR
    def count(buf):
        n, k = 0, 0
        while k < len(buf):
            if buf[k:k + 4] == b"GRUP":
                n += 1
                k += 24
                continue
            n += 1
            k += 24 + struct.unpack_from("<I", buf, k + 4)[0]
        return n

    hedr = struct.pack("<fII", struct.unpack_from("<f", d, 24 + 6)[0], count(groups), 0x800)
    items = [(b"HEDR", hedr), (b"CNAM", b"Flying Vehicle Overhaul (FVO) (patch for Parked Vehicles by SarthesArai)\0")]
    items += [(b"MAST", m.encode() + b"\0") for m in MASTERS]
    items += [(b"ONAM", b"".join(struct.pack("<I", f) for f in sorted(set(overridden))))]
    items += [(b"BNAM", b"Main\0")]
    tes4 = pack_subs(items)
    header = b"TES4" + struct.pack("<II", len(tes4), F_ESM | F_LIGHT) + d[12:24]
    open(dst, "wb").write(header + tes4 + groups)
    print("pack-in cells %d; props made vehicles %d; wheel/turret parts disabled %d; records+groups %d; %d bytes"
          % (len(packin_cells), stats["vehicle"], stats["part"], count(groups), len(header) + len(tes4) + len(groups)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
