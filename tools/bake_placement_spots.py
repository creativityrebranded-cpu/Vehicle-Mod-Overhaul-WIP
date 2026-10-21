"""Bake the vehicle placements of the optional placement mods into plugin/src/placement_spots.h.

The DLL's placement patch (realvehicles.cpp) gives each of these spots a vehicle type picked from every vehicle
in the load order, with a fixed seed, per location. The spots come from the plugin files themselves, so the patch
recognises the mods' vehicles by where they stand, not by form ids (carfield.esm writes its new records under a
master's index). Nothing here is required at run time: a mod that is not loaded keeps its spots inactive.

    python scripts/bake_placement_spots.py            # reads the installed plugins, writes the header
"""
import struct, zlib, sys, os, collections, hashlib

DATA = r"C:\Program Files (x86)\Steam\steamapps\common\Starfield\Data"
OUT = os.path.join(os.path.dirname(__file__), "..", "source", "src", "placement_spots.h")  # the repo layout

def subrecords(body):
    i = 0
    while i + 6 <= len(body):
        t = body[i:i+4]; n = struct.unpack_from('<H', body, i+4)[0]
        if t == b'XXXX':
            big = struct.unpack_from('<I', body, i+6)[0]; i += 10
            t = body[i:i+4]; yield t, body[i+6:i+6+big]; i += 6 + big; continue
        yield t, body[i+6:i+6+n]; i += 6 + n

def first(body, tag):
    for t, v in subrecords(body):
        if t == tag: return v
    return None

def read(path):
    d = open(path, 'rb').read()
    hsz = struct.unpack_from('<I', d, 4)[0]
    masters = [v[:-1].decode('latin1') for t, v in subrecords(d[24:24+hsz]) if t == b'MAST']
    recs = []  # (type, fid, body, worldspace file id or None, inPackin)
    def walk(o, e, world, packin):
        while o < e:
            t = d[o:o+4]; s = struct.unpack_from('<I', d, o+4)[0]
            if t == b'GRUP':
                label, gtype = struct.unpack_from('<Ii', d, o+8)
                w = label if gtype == 1 else world          # world children
                walk(o+24, o+s, w, packin); o += s; continue
            fl, fid = struct.unpack_from('<II', d, o+8)
            b = d[o+24:o+24+s]
            if fl & 0x40000:
                try: b = zlib.decompress(b[4:])
                except Exception: b = b''
            recs.append((t.decode('latin1'), fid, b, world))
            o += 24 + s
    walk(24 + hsz, len(d), None, False)
    return masters, recs

def owner(masters, plugin, fid):
    """(file name, local id) a file-relative form id belongs to."""
    idx = fid >> 24
    return (masters[idx] if idx < len(masters) else plugin), fid & 0xFFFFFF

# What counts as a vehicle placement, per mod (file-relative base ids as written in the file).
MODS = {
    "carfield.esm": {
        # Its six vehicle types are recognised wherever they are parked (stand-ins). Its placements are NOT baked: Carfield is a
        # Creation by Swordhands_Bowman and none of its data ships with this mod (2026-10-21).
        "spots": False,
        "bases": {0x2000806, 0x2000809, 0x200080B, 0x2000814, 0x2000020, 0x2000021, 0xFE000033, 0xFE001005, 0xFE002005},
    },
    "Vehicles.esm": {
        # leveled pack-ins (drivable and static) and the vehicle props (Vehicle_Ref8/Deimog/MoonJumper_Static)
        "bases": {0x2000903, 0x2000904, 0x2000905, 0x2000906, 0x2000800, 0x20008E7, 0x20008E8, 0x20008E9, 0x20008F8,
                  0x200080B, 0x200080C, 0x20008DD},
    },
}

def main():
    spots = []; masters_used = []; own_bases = []
    def master_index(name):
        if name.lower() not in [m.lower() for m in masters_used]: masters_used.append(name)
        return [m.lower() for m in masters_used].index(name.lower())
    per = collections.Counter()
    for mi, (plugin, spec) in enumerate(MODS.items()):
        path = os.path.join(DATA, plugin)
        if not os.path.exists(path):
            print("not installed:", plugin, "(its spots are left out)"); continue
        masters, recs = read(path)
        for t, fid, b, world in recs:
            if t == 'FURN' and fid in spec["bases"]:
                ed = first(b, b'EDID'); ed = ed[:-1].decode('latin1') if ed else ''
                file, local = owner(masters, plugin, fid)
                standin = struct.unpack_from('<Q', hashlib.sha1(f"{plugin.lower()}|{fid:08X}".encode()).digest())[0]
                own_bases.append((mi, plugin, file, local, standin, ed))
        for t, fid, b, world in recs:
            if t != 'REFR' or world is None or not spec.get("spots", True): continue
            name = first(b, b'NAME'); data = first(b, b'DATA')
            if not name or not data or len(data) < 24: continue
            base = struct.unpack_from('<I', name)[0]
            if base not in spec["bases"]: continue
            x, y, z, rx, ry, rz = struct.unpack_from('<6f', data)
            wfile, wlocal = owner(masters, plugin, world)
            # A stable spot key: the mod, the reference's file id (low 24 bits) and its place in metres.
            h = hashlib.sha1(f"{plugin.lower()}|{fid & 0xFFFFFF:06X}|{round(x)}|{round(y)}".encode()).digest()
            key = struct.unpack_from('<Q', h)[0]
            # The stand-in: the kind of vehicle the mod placed here (its base or pack-in); the same key at every spot of that kind.
            standin = struct.unpack_from('<Q', hashlib.sha1(f"{plugin.lower()}|{base:08X}".encode()).digest())[0]
            spots.append((mi, master_index(wfile), wlocal, x, y, z, rz, key, standin, plugin, fid))
            per[(plugin, wfile, wlocal)] += 1
    for ob in own_bases: master_index(ob[2])  # every file a stand-in is resolved through is in kFiles
    with open(OUT, 'w', newline='\n') as f:
        f.write("#pragma once\n// Generated by scripts/bake_placement_spots.py from the installed plugins. Do not edit by hand.\n")
        f.write("// Vehicle placements of the optional placement mods: the mod, the worldspace (master file + local id), position,\n")
        f.write("// heading and a stable key. Only spots of a loaded mod are active (realvehicles.cpp placement patch).\n")
        f.write("#include <cstdint>\nnamespace placementspots {\n")
        f.write("// standin: the kind of vehicle the mod placed (its base or pack-in); every spot of one kind in one location gets the same vehicle.\n")
        f.write("struct Spot { std::uint8_t mod, worldFile; std::uint32_t worldLocal; float x, y, z, heading; std::uint64_t key, standin; };\n")
        f.write("inline constexpr const char* kMods[] = { " + ", ".join(f'"{m}"' for m in MODS) + " };\n")
        f.write("inline constexpr const char* kFiles[] = { " + ", ".join(f'"{m}"' for m in masters_used) + " };\n")
        f.write(f"inline constexpr unsigned kSpotCount = {len(spots)};\n")
        # A mod's own vehicle types: recognised wherever they stand (no spot needed). Resolved at run time through the file
        # the plugin writes them under (carfield.esm uses a master's index for its new records).
        f.write("// A mod's own vehicle types (stand-ins recognised anywhere): the mod, the file its id is written under, the local id, the stand-in.\n")
        f.write("struct StandinBase { std::uint8_t mod, file; std::uint32_t local; std::uint64_t standin; };\n")
        f.write("inline constexpr StandinBase kStandinBases[] = {\n")
        for mi, plugin, file, local, standin, edid in own_bases:
            f.write(f"    {{ {mi}, {master_index(file)}, 0x{local:06X}, 0x{standin:016X}ull }},  // {plugin} {edid}\n")
        f.write("};\n")
        f.write("inline constexpr Spot kSpots[] = {\n")
        for mi, wf, wl, x, y, z, rz, key, standin, plugin, fid in spots:
            f.write(f"    {{ {mi}, {wf}, 0x{wl:06X}, {x:.2f}f, {y:.2f}f, {z:.2f}f, {rz:.4f}f, 0x{key:016X}ull, 0x{standin:016X}ull }},  // {plugin} {fid:08X}\n")
        f.write("};\n}\n")
    print(f"{len(spots)} spots -> {os.path.normpath(OUT)}")
    for (plugin, wf, wl), n in sorted(per.items(), key=lambda kv: -kv[1])[:60]:
        print(f"  {plugin:14} {wf:22} {wl:06X}  {n}")

if __name__ == "__main__":
    main()
