"""Builds "FVO_NPCPilots.esm" (until 0.72.0 "SeamlessPilots.esm"): a small plugin with one quest ("FVO_NPCPilotsQuest", start-game-enabled) that holds
6 pilot ref aliases and 6 vehicle ref aliases, and 6 packages, one per pilot alias: "activate the vehicle alias".

The plugin (FlyingVehicleOverhaul.dll) fills the aliases at run time with the engine's own ForceRefTo, so the NPC runs a
QUEST package (which outranks the NPC's own schedule) that walks it to the vehicle and activates it (the game's get-in).
Works for any actor, follower or not. Records are copied in shape from Starfield.esm:
  quest   <- SQ_Passengers (001D307E) / DialogueCFTheKey_RandomIdles_LastNova02 (000B2283) for the alias layout
  package <- SQ_ActivateAlarm (002062C8): template "Activate" (0007976C), QNAM as UC08_Hadrian_Stage600_Activate_UtilityPanel
  PTDA type 4 = reference alias (value = alias id)

python scripts/make_pilots_esm.py <Starfield.esm> <out.esm>
Local ids: quest 0x800, packages 0x801..0x806 (the plugin finds the load index by probing).
"""
import struct
import sys
import zlib

PILOTS = 6
QUEST_ID = 0x800
PKG_BASE = 0x801
ACT_QUEST_ID = 0x810
ACT_PKG_BASE = 0x811
F_COMPRESSED, F_ESM = 0x40000, 0x1


def sub(t, v):
    return t + struct.pack("<H", len(v)) + v


def record(rtype, fid, body, hdr_tail):
    return rtype + struct.pack("<II", len(body), 0) + struct.pack("<I", fid) + hdr_tail + body


def read_subs(body):
    i, out = 0, []
    while i + 6 <= len(body):
        n = struct.unpack_from("<H", body, i + 4)[0]
        out.append((body[i:i + 4], body[i + 6:i + 6 + n]))
        i += 6 + n
    return out


def find_record(d, want, fid):
    def walk(a, b):
        i = a
        while i < b:
            if d[i:i + 4] == b"GRUP":
                sz = struct.unpack_from("<I", d, i + 4)[0]
                r = walk(i + 24, i + sz)
                if r:
                    return r
                i += sz
                continue
            sz, fl, rid = struct.unpack_from("<III", d, i + 4)
            if d[i:i + 4] == want and rid == fid:
                body = d[i + 24:i + 24 + sz]
                return zlib.decompress(body[4:]) if fl & F_COMPRESSED else body
            i += 24 + sz
        return None
    return walk(24 + struct.unpack_from("<I", d, 4)[0], len(d))


def main(src, dst):
    d = open(src, "rb").read()
    tail = struct.pack("<IHH", 0, 0x245, 0)  # timestamp, form version 581, vc
    # the package: MQ201_StarbornPkg (Travel template 0x2CB0, location = a reference alias), our quest and the vehicle alias,
    # stopping within 3 m. (The first version cloned SQ_ActivateAlarm, "Activate": the NPC held that package yet never walked to a
    # vehicle target, but did walk to a plain object, 2026-10-17 23:29.) The plugin boards the NPC once it is near (Mount).
    pk = read_subs(find_record(d, b"PACK", 0x00113E28))
    packs = b""
    for k in range(PILOTS):
        out = []
        for t, v in pk:
            if t == b"EDID":
                v = b"FVO_PilotGetIn%d\x00" % k
            elif t == b"QNAM":
                v = struct.pack("<I", 0x01000000 | QUEST_ID)
            elif t == b"PLDT":
                v = struct.pack("<IIfI", 8, PILOTS + k, 0.5, 0)  # reference alias (vehicle), radius 0.5 m (3 m left the NPC 5 m from the door)
            out.append((t, v))
        packs += record(b"PACK", 0x01000000 | (PKG_BASE + k), b"".join(sub(t, v) for t, v in out), tail)

    # the quest: SQ_Passengers' header fields, then the aliases
    body = sub(b"EDID", b"FVO_NPCPilotsQuest\0")
    body += sub(b"DNAM", struct.pack("<III", 0x111, 0, 0))
    body += sub(b"NEXT", b"")
    body += sub(b"ANAM", struct.pack("<I", PILOTS * 2))
    for k in range(PILOTS * 2):
        pilot = k < PILOTS
        body += sub(b"ALST", struct.pack("<I", k))
        body += sub(b"ALID", (b"Pilot%d\0" % k) if pilot else (b"Vehicle%d\0" % (k - PILOTS)))
        body += sub(b"FNAM", struct.pack("<I", 0x00100002))  # optional (may stay empty), as the vanilla aliases' 0x100000
        body += sub(b"ALFG", struct.pack("<I", 0))
        if pilot:
            body += sub(b"ALPC", struct.pack("<I", 0x01000000 | (PKG_BASE + k)))
        body += sub(b"VTCK", struct.pack("<I", 0))
        body += sub(b"ALED", b"")
    quest = record(b"QUST", 0x01000000 | QUEST_ID, body, tail)

    # Second quest (a NEW form, so existing saves are untouched): the same pilot / vehicle aliases, but its pilot alias carries an ACTIVATE
    # package (SQ_ActivateAlarm's: template Activate 0007976C, target = the vehicle alias of this quest). Decompile 2026-10-18: the Activate
    # procedure (id 0x11) runs FUN_1419E5CF0, which for a vehicle target calls the animated board routine FUN_141993710(actor, vehicle, 0)
    # first and only falls back to the gated walk-in FUN_141951FB0. The plugin fills it once the NPC has walked up (the Travel package alone
    # never boards).
    pa = read_subs(find_record(d, b"PACK", 0x002062C8))
    packs2 = b""
    for k in range(PILOTS):
        out = []
        for t, v in pa:
            if t == b"EDID":
                v = b"FVO_PilotActivate%d\x00" % k
            if t == b"PKCU":
                out.append((b"QNAM", struct.pack("<I", 0x01000000 | ACT_QUEST_ID)))
            if t == b"PTDA":
                v = struct.pack("<III", 4, PILOTS + k, 0)
            out.append((t, v))
        packs2 += record(b"PACK", 0x01000000 | (ACT_PKG_BASE + k), b"".join(sub(t, v) for t, v in out), tail)
    body2 = sub(b"EDID", b"FVO_NPCPilotsActivateQuest\x00")
    body2 += sub(b"DNAM", struct.pack("<III", 0x111, 0, 0))
    body2 += sub(b"NEXT", b"")
    body2 += sub(b"ANAM", struct.pack("<I", PILOTS * 2))
    for k in range(PILOTS * 2):
        pilot = k < PILOTS
        body2 += sub(b"ALST", struct.pack("<I", k))
        body2 += sub(b"ALID", (b"ActPilot%d\x00" % k) if pilot else (b"ActVehicle%d\x00" % (k - PILOTS)))
        body2 += sub(b"FNAM", struct.pack("<I", 0x00100002))
        body2 += sub(b"ALFG", struct.pack("<I", 0))
        if pilot:
            body2 += sub(b"ALPC", struct.pack("<I", 0x01000000 | (ACT_PKG_BASE + k)))
        body2 += sub(b"VTCK", struct.pack("<I", 0))
        body2 += sub(b"ALED", b"")
    quest2 = record(b"QUST", 0x01000000 | ACT_QUEST_ID, body2, tail)

    def group(label, recs):
        return b"GRUP" + struct.pack("<I", 24 + len(recs)) + label + struct.pack("<IIHH", 0, 0, 0x245, 0) + recs

    groups = group(b"QUST", quest + quest2) + group(b"PACK", packs + packs2)
    nrec = 2 + PILOTS * 2 + 2  # 2 quests + 12 packages + 2 groups
    hedr_ver = struct.unpack_from("<f", d, 24 + 6)[0]
    items = [(b"HEDR", struct.pack("<fII", hedr_ver, nrec, 0x820)),
             (b"CNAM", b"Flying Vehicle Overhaul (FVO)\0"),
             (b"MAST", b"Starfield.esm\0"), (b"DATA", struct.pack("<Q", 0))]
    tes4 = b"".join(sub(t, v) for t, v in items)
    header = b"TES4" + struct.pack("<II", len(tes4), F_ESM) + d[12:24]
    open(dst, "wb").write(header + tes4 + groups)
    print("quest 1, packages %d, %d bytes" % (PILOTS, len(header) + len(tes4) + len(groups)))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
