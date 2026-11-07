# SPDX-License-Identifier: GPL-3.0-or-later
"""Read Bloodborne's map layouts (MSB) and diff them between builds.

    python -I tools/msb.py check <msb ...>
    python -I tools/msb.py dump  <msb> [parts|enemies|regions|events|treasure|models]
    python -I tools/msb.py diff  <old msb> <new msb> <out.md>

An MSB (map/mapstudio/<map>.msb.dcx) is the map's layout: every model it uses, every placed part
(map pieces, objects, enemies, collision, navmesh), every region (points and volumes that event
scripts test) and every map event (treasure, generators, messages, object actions...).

Layout follows SoulsFormats' MSBB (JKAnderson, MIT). After a 16-byte "MSB " header come four
param lists, in this order:

    MODEL_PARAM_ST, EVENT_PARAM_ST, POINT_PARAM_ST, PARTS_PARAM_ST
    list   { i32 version, i32 offset count, i64 name offset, i64 entries[count - 1], i64 next list }
    model  { i64 name, u32 type, i32 id, i64 sib path, i32 instance count, ... }
    event  { i64 name, i32 event id, u32 type, i32 id, i32 0, i64 entity data, i64 type data }
           entity data { i32 part index, i32 region index, i32 entity id, ... }
           treasure    { i32 0, i32 0, i32 part index, i32 0, i32 item lot x3, ... }
    region { i64 name, i32 0, i32 id, u32 shape, f32 pos[3], f32 rot[3], i32 0, i64 a, i64 b,
             i64 shape data, i64 entity data }   entity data { i32 entity id }
    part   { i64 description, i64 name, i32 instance id, u32 type, i32 id, i32 model index,
             i64 sib path, f32 pos[3], f32 rot[3], f32 scale[3], u32 groups[24], i32 0,
             i64 entity data, i64 type data, ... }   entity data { i32 entity id, ... }
           enemy type data { i32 0, i32 0, i32 think param, i32 NPC param, i32 talk id,
                             i32 chara-init param, i32, i32 collision index, i16, i16 0, i32 0,
                             i16 patrol points[8], i32 initial anim, i32 damage anim }

All offsets inside an entry are relative to the entry's start.
"""

from __future__ import annotations

import argparse
import io
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls

PART_TYPES = {
    0: "MapPiece",
    1: "Object",
    2: "Enemy",
    4: "Player",
    5: "Collision",
    8: "Navmesh",
    9: "DummyObject",
    10: "DummyEnemy",
    11: "ConnectCollision",
}
EVENT_TYPES = {
    1: "Sound",
    2: "SFX",
    4: "Treasure",
    5: "Generator",
    6: "Message",
    7: "ObjAct",
    8: "SpawnPoint",
    9: "MapOffset",
    10: "Navmesh",
    11: "Environment",
    13: "WindSFX",
    14: "PatrolInfo",
    15: "DarkLock",
    16: "PlatoonInfo",
    17: "MultiSummon",
}
SHAPES = {
    0: "Point",
    1: "Circle",
    2: "Sphere",
    3: "Cylinder",
    4: "Square",
    5: "Box",
    6: "Composite",
}
LISTS = ["MODEL_PARAM_ST", "EVENT_PARAM_ST", "POINT_PARAM_ST", "PARTS_PARAM_ST"]


def _wstr(d: bytes, off: int) -> str:
    # Find the UTF-16 terminator on an even boundary; bounded, so a bad offset raises instead of
    # spinning past the end of the data.
    end = off
    while True:
        end = d.index(b"\0\0", end)
        if (end - off) % 2 == 0:
            return d[off:end].decode("utf-16-le")
        end += 1


class Msb:
    def __init__(self, data: bytes):
        self.d = data
        if data[:4] != b"MSB ":
            raise ValueError("not an MSB")
        p = 0x10  # "MSB " file header: magic, i32 1, i32 0x10, u8 0, u8 0, u8 1, u8 0xFF
        lists = {}
        for expected in LISTS:
            _ver, cnt, name_off = struct.unpack_from("<iiq", data, p)
            entries = struct.unpack_from(f"<{cnt - 1}q", data, p + 16)
            nxt = struct.unpack_from("<q", data, p + 16 + 8 * (cnt - 1))[0]
            name = _wstr(data, name_off)
            if name != expected:
                raise ValueError(f"expected {expected}, found {name}")
            lists[name] = entries
            p = nxt
        if p != 0:
            raise ValueError("last list's next offset is not 0")
        self.models = [self._model(o) for o in lists["MODEL_PARAM_ST"]]
        self.events = [self._event(o) for o in lists["EVENT_PARAM_ST"]]
        self.regions = [self._region(o) for o in lists["POINT_PARAM_ST"]]
        self.parts = [self._part(o) for o in lists["PARTS_PARAM_ST"]]
        for e in self.events:
            e["part"] = (
                self.parts[e["part_index"]]["name"]
                if 0 <= e["part_index"] < len(self.parts)
                else None
            )
            e["region"] = (
                self.regions[e["region_index"]]["name"]
                if 0 <= e["region_index"] < len(self.regions)
                else None
            )
            if "treasure_part_index" in e:
                i = e["treasure_part_index"]
                e["treasure_part"] = (
                    self.parts[i]["name"] if 0 <= i < len(self.parts) else None
                )

    def _model(self, o):
        d = self.d
        name_off, mtype = struct.unpack_from("<qI", d, o)
        return {"name": _wstr(d, o + name_off), "type": mtype}

    def _event(self, o):
        d = self.d
        name_off, event_id, etype, _id, _z, ent_off, type_off = struct.unpack_from(
            "<qiIiiqq", d, o
        )
        part_i, region_i, entity = struct.unpack_from("<iii", d, o + ent_off)
        e = {
            "name": _wstr(d, o + name_off),
            "event_id": event_id,
            "type": EVENT_TYPES.get(etype, str(etype)),
            "part_index": part_i,
            "region_index": region_i,
            "entity": entity,
        }
        if etype == 4 and type_off:
            tp, lots = (
                struct.unpack_from("<8xi4x", d, o + type_off)[0],
                struct.unpack_from("<3i", d, o + type_off + 16),
            )
            e["treasure_part_index"], e["item_lots"] = (
                tp,
                [x for x in lots if x not in (0, -1)],
            )
        return e

    def _region(self, o):
        d = self.d
        name_off, _z, _id, shape = struct.unpack_from("<qiiI", d, o)
        pos = struct.unpack_from("<3f", d, o + 20)
        rot = struct.unpack_from("<3f", d, o + 32)
        ent_off = struct.unpack_from("<q", d, o + 0x48)[
            0
        ]  # 0x40 is the shape data (box size...)
        return {
            "name": _wstr(d, o + name_off),
            "shape": SHAPES.get(shape, str(shape)),
            "pos": pos,
            "rot": rot,
            "entity": struct.unpack_from("<i", d, o + ent_off)[0],
        }

    def _part(self, o):
        d = self.d
        _desc_off, name_off, _inst, ptype, _id, model_i, _sib = struct.unpack_from(
            "<qqiIiiq", d, o
        )
        pos, rot, scale = (
            struct.unpack_from("<3f", d, o + 0x28 + 12 * k) for k in range(3)
        )
        ent_off, type_off = struct.unpack_from("<qq", d, o + 0x4C + 24 * 4 + 4)
        p = {
            "name": _wstr(d, o + name_off),
            "type": PART_TYPES.get(ptype, str(ptype)),
            "model": self.models[model_i]["name"]
            if 0 <= model_i < len(self.models)
            else None,
            "pos": pos,
            "rot": rot,
            "scale": scale,
            "entity": struct.unpack_from("<i", d, o + ent_off)[0],
        }
        if ptype in (2, 10) and type_off:
            think, npc, talk, chara_init = struct.unpack_from("<8x4i", d, o + type_off)
            p.update(think=think, npc=npc, talk=talk, chara_init=chara_init)
        return p


def fmt3(v):
    return "(" + ", ".join(f"{x:.2f}" for x in v) + ")"


def describe(kind, x):
    if kind == "parts":
        s = f"{x['type']:12} {x['name']:28} model {x['model']:10} pos {fmt3(x['pos'])} rot {fmt3(x['rot'])} entity {x['entity']}"
        if "npc" in x:
            s += f"  npc {x['npc']} think {x['think']} talk {x['talk']} init {x['chara_init']}"
        return s
    if kind == "regions":
        return (
            f"{x['shape']:9} {x['name']:32} pos {fmt3(x['pos'])} entity {x['entity']}"
        )
    if kind == "events":
        s = f"{x['type']:12} {x['name']:32} id {x['event_id']} part {x['part']} region {x['region']} entity {x['entity']}"
        if "item_lots" in x:
            s += f"  treasure {x['treasure_part']} lots {x['item_lots']}"
        return s
    return f"{x['type']:3} {x['name']}"


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("msb", nargs="+")
    d = sub.add_parser("dump")
    d.add_argument("msb")
    d.add_argument("what", nargs="?", default="parts")
    f = sub.add_parser("diff")
    f.add_argument("old")
    f.add_argument("new")
    f.add_argument("out")
    a = ap.parse_args()

    if a.cmd == "check":
        tot = {
            "models": 0,
            "events": 0,
            "regions": 0,
            "parts": 0,
            "enemies": 0,
            "treasure": 0,
        }
        for path in a.msb:
            m = Msb(souls.load(path))
            tot["models"] += len(m.models)
            tot["events"] += len(m.events)
            tot["regions"] += len(m.regions)
            tot["parts"] += len(m.parts)
            tot["enemies"] += sum(1 for p in m.parts if p["type"] == "Enemy")
            tot["treasure"] += sum(1 for e in m.events if e["type"] == "Treasure")
        print(
            f"{len(a.msb)} maps read: "
            + ", ".join(f"{v:,} {k}" for k, v in tot.items())
        )
        return 0

    if a.cmd == "dump":
        m = Msb(souls.load(a.msb))
        what = a.what
        items = {
            "parts": m.parts,
            "regions": m.regions,
            "events": m.events,
            "models": m.models,
            "enemies": [p for p in m.parts if p["type"] == "Enemy"],
            "treasure": [e for e in m.events if e["type"] == "Treasure"],
        }[what]
        kind = (
            "parts" if what == "enemies" else "events" if what == "treasure" else what
        )
        for x in items:
            print(describe(kind, x))
        return 0

    old, new = Msb(souls.load(a.old)), Msb(souls.load(a.new))
    out = [f"# Map layout diff: {os.path.basename(a.old)}\n"]
    summary = ["| List | Added | Removed | Changed |", "|---|---:|---:|---:|"]
    body = []
    for kind in ("parts", "regions", "events"):
        o = {x["name"]: x for x in getattr(old, kind)}
        n = {x["name"]: x for x in getattr(new, kind)}
        added = sorted(set(n) - set(o))
        removed = sorted(set(o) - set(n))
        changed = []
        for k in sorted(set(o) & set(n)):
            fo, fn = describe(kind, o[k]), describe(kind, n[k])
            if fo != fn:
                changed.append((fo, fn))
        summary.append(f"| {kind} | {len(added)} | {len(removed)} | {len(changed)} |")
        if added or removed or changed:
            body.append(f"\n## {kind}\n")
            body.append("```diff")
            body += [f"+ {describe(kind, n[k])}" for k in added]
            body += [f"- {describe(kind, o[k])}" for k in removed]
            for fo, fn in changed:
                body += [f"- {fo}", f"+ {fn}"]
            body.append("```")
    with open(a.out, "w", encoding="utf-8") as fo_:
        fo_.write("\n".join(out + summary + body) + "\n")
    print("\n".join(summary))
    return 0


if __name__ == "__main__":
    sys.exit(main())
