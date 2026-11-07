# SPDX-License-Identifier: GPL-3.0-or-later
"""Where every item in Bloodborne is: placed treasure and enemy drops, from the game's own data.

    python -I tools/items.py world <base dvdroot> <update dvdroot> <out.md>
    python -I tools/items.py find  <base dvdroot> <update dvdroot> <text>

The chain, every link read from the game (1.09 files laid over 1.00):

    map layout (MSB)  treasure event --> item lot(s) --> item id + category --> English name
                      enemy part --> NpcParam --> blood echoes + item lots --> ...
    event script      [2003:04] obtain item(lot) --> ...   (literal, or passed in by [2000:00])
    ShopLineupParam   item, price per tier, unlock flag (qwcId)
    Chalice layouts   map/mapstudio/m29_*/, attributed to their chalice through HolygrailExParam

Item lot categories, established by matching IDs against the tables:
    0 weapon (EquipParamWeapon)   1 armour (EquipParamProtector)   4 goods (EquipParamGoods)
    8 blood gem (GemGenParam: gems are generated, so they have no fixed name)
    15 level-dependent: an ItemLotLvdepParam row, which holds the real {category, item id}

A lot has eight slots, each {item, category, count, weight}. A slot's chance is its weight over
the total weight of all eight; a slot with weight but no item is "nothing".
Maps are named from the place-name table: mAA_BB -> place id AA*100 + BB*10.
"""

from __future__ import annotations

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import msb as msblib  # noqa: E402
import params  # noqa: E402
import souls  # noqa: E402

CATEGORY = {0: "weapon", 1: "armour", 4: "goods", 8: "blood gem", 15: "level-dependent"}


class World:
    def __init__(self, base: str, update: str):
        self.base, self.update = base, update
        P, D = params.load(self.path("param/gameparam/gameparam.parambnd.dcx"),
                           self.path("paramdef/paramdef.paramdefbnd.dcx"))
        self.rows = lambda n: params.rows(P[n], D[P[n].type])
        self.lots = self.rows("ItemLotParam")
        self.lvdep = self.rows("ItemLotLvdepParam")
        self.npcs = self.rows("NpcParam")
        msg = self.path("msg/engus/item.msgbnd.dcx")
        self.names = params.item_names(msg)
        self.npc_names, self.places = {}, {}
        for bf in souls.bnd4_read(souls.load(msg)):
            if "NPC名" in bf.name:
                self.npc_names = souls.fmg_read(bf.data)
            elif "地名" in bf.name:
                self.places = souls.fmg_read(bf.data)

    def path(self, rel: str) -> str:
        """The patched game: the update's copy of a file if it has one, else the base game's."""
        p = os.path.join(self.update, rel)
        return p if os.path.exists(p) else os.path.join(self.base, rel)

    def map_name(self, m: str) -> str:
        aa, bb = int(m[1:3]), int(m[4:6])
        return (self.places.get(aa * 100 + bb * 10) or self.places.get(aa * 100) or "").strip()

    def item_name(self, cat: int, iid: int) -> str:
        if cat == 15:
            r = self.lvdep.get(iid)
            return self.item_name(r["itemCategory"], r["itemId"]) + " (level-dependent)" if r else f"lvdep {iid}?"
        table = {0: "EquipParamWeapon", 1: "EquipParamProtector", 4: "EquipParamGoods"}.get(cat)
        if cat == 8:
            return f"blood gem (generator {iid})"
        n = self.names.get(table, {}).get(iid) if table else None
        return n.strip() if n else f"{CATEGORY.get(cat, cat)} {iid}"

    def lot(self, lot_id: int) -> list[tuple[str, int, float]]:
        """[(item, count, chance)] for one lot; chance 1.0 when the lot has a single weighted slot."""
        r = self.lots.get(lot_id)
        if not r:
            return []
        slots = [(r[f"lotItemId0{k}"], r[f"lotItemCategory0{k}"], r[f"lotItemNum0{k}"], r[f"lotItemBasePoint0{k}"])
                 for k in range(1, 9)]
        total = sum(p for *_, p in slots) or 1
        return [(self.item_name(c, i), n, p / total) for i, c, n, p in slots if i > 0]

    def maps(self) -> list[str]:
        d = self.path("map/mapstudio")
        base = os.path.join(self.base, "map/mapstudio")
        names = sorted(f for f in os.listdir(base) if f.endswith(".msb.dcx"))
        return [n[:12] for n in names]

    def layout(self, m: str):
        return msblib.Msb(souls.load(self.path(f"map/mapstudio/{m}.msb.dcx")))


def chalice_treasure(w: World):
    """{item name: [(chalice, layouts containing it, layouts of that chalice)]} for the Chalice Dungeons.

    Each Root chalice is 100 or 200 pre-built layouts (see docs/the-chalice-dungeons.md); a layout
    folder belongs to the ritual whose mapBaseId ABC names it (m29_AB_C...). Counting the layouts
    that place an item says how likely a given dungeon of that chalice is to hold it.
    """
    goods = w.names.get("EquipParamGoods", {})
    folder_owner = {}
    ms = os.path.join(w.base, "map", "mapstudio")
    folders = sorted(d for d in os.listdir(ms) if d.startswith("m29_"))
    for rid, r in w.rows("HolygrailExParam").items():
        mb = r["mapBaseId"]
        name = goods.get(rid, "").strip()
        if not mb or not name:
            continue
        for f in folders:
            if f.startswith(f"m29_{mb // 100}{(mb // 10) % 10}_{mb % 10}"):
                folder_owner[f] = name
    per_chalice = collections.defaultdict(collections.Counter)
    layouts = collections.Counter()
    for f in folders:
        owner = folder_owner.get(f)
        if not owner:
            continue  # tile library and other non-ritual folders
        for fn in os.listdir(os.path.join(ms, f)):
            lay = msblib.Msb(souls.load(os.path.join(ms, f, fn)))
            layouts[owner] += 1
            seen = {name for e in lay.events if e["type"] == "Treasure"
                    for lid in e.get("item_lots", []) for name, n, c in w.lot(lid)}
            for name in seen:
                per_chalice[name][owner] += 1
    return {name: [(ch, n, layouts[ch]) for ch, n in sorted(c.items())] for name, c in per_chalice.items()}


SHOP_EQUIP = {0: "EquipParamWeapon", 1: "EquipParamProtector", 3: "EquipParamGoods"}


def shop_series(series: int, shop_type: int) -> str:
    """ShopLineupParam row id // 10000, described from what the rows contain (see docs/the-items.md)."""
    if series == 0:
        return "starting weapons"
    if 10 <= series <= 14:
        return f"Bath Messengers, echoes, tier {series - 9}"
    if 20 <= series <= 24:
        return f"Bath Messengers, insight, tier {series - 19}"
    if 51 <= series <= 54 or 61 <= series <= 64:
        return f"shop series {series // 10}x, echoes, tier {series % 10}"
    return f"shop series {series} (type {shop_type})"


def ownership_flags(w: World) -> dict[int, str]:
    """Event flags that mean "the player owns item X", worked out from the event scripts.

    An event that waits on [3:04] / [3:16] アイテムの所有状態で判定 ("judge by item ownership")
    with the item taken from a parameter, and is started with [2000:00] イベント起動 in slot s,
    finishes when the player owns that item, and the engine sets the event's own flag: event id
    + slot. The Bath Messengers' line-up expansion (event 12101000, one slot per hunter badge) is
    the case that matters: flag 12101007 is "owns the Cainhurst Badge".
    """
    import events as evlib
    import struct
    out = {}
    evdir = os.path.join(w.base, "event")
    for fn in sorted(os.listdir(evdir)):
        if not fn.endswith(".emevd.dcx"):
            continue
        evs = {e.id: e for e in evlib.read_emevd(souls.load(w.path(f"event/{fn}")))}
        waits = {}  # event id -> (parameter source byte of the item id, item type)
        for e in evs.values():
            subst = {(idx, dst): src for idx, dst, src, ln in e.params}
            for k, (c, i, raw) in enumerate(e.instructions):
                # args: s8 condition group @0, u8 item type @1, s32 item id @4, u8 state @8
                if (c, i) in ((3, 4), (3, 16)) and (k, 4) in subst:
                    waits[e.id] = (subst[(k, 4)], raw[1])
        for e in evs.values():
            for c, i, raw in e.instructions:
                if (c, i) == (2000, 0) and len(raw) >= 12:
                    slot, target = struct.unpack_from("<iI", raw, 0)
                    if target in waits:
                        src, itype = waits[target]
                        args = raw[8:]
                        if src + 4 <= len(args):
                            item = struct.unpack_from("<i", args, src)[0]
                            table = {0: "EquipParamWeapon", 1: "EquipParamProtector", 3: "EquipParamGoods"}.get(itype)
                            name = w.names.get(table, {}).get(item, f"item {item}") if table else f"item {item}"
                            out[target + slot] = f"owns {name.strip()}"
    return out


def shops(w: World):
    """{item name: [(where, price, unlock flag)]} from ShopLineupParam.

    The unlock flag is the qwcId field: Dark Souls' world-tendency ID, reused by Bloodborne as
    the event flag a row needs before it is for sale (-1/0: always). Flags that mean "owns item X"
    (see ownership_flags) are spelled out, e.g. 12101007 (owns Cainhurst Badge).
    """
    out = collections.defaultdict(list)
    meanings = ownership_flags(w)
    for rid, r in sorted(w.rows("ShopLineupParam").items()):
        table = SHOP_EQUIP.get(r["equipType"])
        name = (w.names.get(table, {}).get(r["equipId"]) or f"{table or r['equipType']} {r['equipId']}").strip()
        flag = r["qwcId"] if r["qwcId"] > 0 else None
        if flag and flag in meanings:
            flag = f"{flag} ({meanings[flag]})"
        out[name].append((shop_series(rid // 10000, r["shopType"]), r["value"], flag, rid))
    return out


AWARD = {(2003, 4): "obtain item", (2003, 36): "obtain item (client too)"}


def event_awards(w: World):
    """Item lots given by event scripts: [2003:04] / [2003:36] アイテム取得 ("obtain item").

    The lot is either a literal argument, or a parameter (X<byte>) filled in by the
    [2000:00] イベント起動 ("start an event") calls that start the event with arguments; those
    are followed one level, which covers how the game uses them.
    """
    import events as evlib
    import struct
    iset = evlib.InstructionSet(souls.load(w.path("event/common.emedf.dcx")))
    out = collections.defaultdict(list)
    evdir = os.path.join(w.base, "event")
    for fn in sorted(os.listdir(evdir)):
        if not fn.endswith(".emevd.dcx"):
            continue
        stem = fn[:-len(".emevd.dcx")]
        evs = evlib.read_emevd(souls.load(w.path(f"event/{fn}")))
        names = evlib.read_emeld(souls.load(w.path(f"event/{stem}.emeld.dcx"))) if os.path.exists(
            w.path(f"event/{stem}.emeld.dcx")) else {}
        by_id = {e.id: e for e in evs}
        # (event id, parameter source byte) -> argument values passed by every start of that event
        passed = collections.defaultdict(list)
        for e in evs:
            for c, i, raw in e.instructions:
                if (c, i) == (2000, 0) and len(raw) >= 12:
                    target = struct.unpack_from("<I", raw, 4)[0]
                    vals = struct.unpack_from(f"<{(len(raw) - 8) // 4}i", raw, 8)
                    for k, v in enumerate(vals):
                        passed[(target, 4 * k)].append(v)
        for e in evs:
            subst = {(idx, dst): src for idx, dst, src, ln in e.params}
            for k, (c, i, raw) in enumerate(e.instructions):
                if (c, i) not in AWARD:
                    continue
                if (k, 0) in subst:
                    lots = passed.get((e.id, subst[(k, 0)]), [])
                else:
                    lots = [struct.unpack_from("<i", raw, 0)[0]]
                for lot in lots:
                    out[stem].append({"event": e.id, "name": names.get(e.id, ""), "how": AWARD[(c, i)],
                                      "lot": lot, "items": w.lot(lot)})
    return out


def fmt_items(items):
    return "; ".join(f"{name}{f' x{n}' if n > 1 else ''}{'' if c >= 0.999 else f' ({c:.0%})'}" for name, n, c in items)


def build(w: World):
    """Per map: treasure rows and enemy-drop rows."""
    treasure, enemies = collections.defaultdict(list), collections.defaultdict(list)
    for m in w.maps():
        lay = w.layout(m)
        parts = {p["name"]: p for p in lay.parts}
        for e in lay.events:
            if e["type"] != "Treasure":
                continue
            items = [it for lid in e.get("item_lots", []) for it in w.lot(lid)]
            obj = parts.get(e.get("treasure_part"))
            flag = w.lots.get(e["item_lots"][0], {}).get("getItemFlagId") if e.get("item_lots") else None
            treasure[m].append({"event": e["name"], "object": e.get("treasure_part"),
                                "pos": obj["pos"] if obj else None, "items": items, "flag": flag})
        placed = collections.Counter(p["npc"] for p in lay.parts if p["type"] == "Enemy" and p.get("npc", -1) > 0)
        for npc_id, count in sorted(placed.items()):
            r = w.npcs.get(npc_id)
            if not r:
                continue
            drops = [it for k in range(1, 7) if r[f"itemLotId_{k}"] > 0 for it in w.lot(r[f"itemLotId_{k}"])]
            enemies[m].append({"npc": npc_id, "name": w.npc_names.get(r["nameId"], "").strip(), "count": count,
                               "echoes": r["getSoul"], "drops": drops})
    return treasure, enemies


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    cmd, base, update = sys.argv[1:4]
    w = World(base, update)
    treasure, enemies = build(w)

    if cmd == "find":
        q = sys.argv[4].lower()
        for name, rows in shops(w).items():
            if q in name.lower():
                for where, price, flag, rid in rows:
                    print(f"{'shop':14} {where:38} {name}: price {price}" + (f", unlocked by flag {flag}" if flag else "")
                          + f"  (row {rid})")
        for stem, rows in event_awards(w).items():
            for r in rows:
                for name, n, c in r["items"]:
                    if q in name.lower():
                        print(f"{stem:14} event {r['event']} {r['name']}: {r['how']}: {name} x{n}")
        for name, rows in chalice_treasure(w).items():
            if q in name.lower():
                for chalice, n, total in rows:
                    print(f"{'chalice':14} {chalice:38} {name}: in {n} of {total} layouts")
        for m in w.maps():
            for t in treasure[m]:
                for name, n, c in t["items"]:
                    if q in name.lower():
                        pos = "(" + ", ".join(f"{x:.1f}" for x in t["pos"]) + ")" if t["pos"] else ""
                        print(f"{m} {w.map_name(m):30} treasure {t['object'] or '':16} {pos:26} {name} x{n}")
            for e in enemies[m]:
                for name, n, c in e["drops"]:
                    if q in name.lower():
                        who = e["name"] or f"NPC {e['npc']}"
                        print(f"{m} {w.map_name(m):30} dropped by {who} ({e['count']} placed): {name} x{n} ({c:.0%})")
        return 0

    out = ["# Where the items are\n",
           "Generated by `tools/items.py world` from the game's own data (1.09 over 1.00). Positions are",
           "map-local coordinates from the layout. Chances under 100% come from the item lot's weights.\n",
           "| Map | Place | Treasure spots | Items placed | Enemy types | Enemies placed |", "|---|---|---:|---:|---:|---:|"]
    for m in w.maps():
        out.append(f"| {m} | {w.map_name(m)} | {len(treasure[m])} | {sum(len(t['items']) for t in treasure[m])} | "
                   f"{len(enemies[m])} | {sum(e['count'] for e in enemies[m])} |")
    for m in w.maps():
        if not treasure[m] and not enemies[m]:
            continue
        out += ["", f"## {m} {w.map_name(m)}", "", "### Treasure", "",
                "| Object | Position | Items | Pickup flag |", "|---|---|---|---:|"]
        for t in treasure[m]:
            pos = "(" + ", ".join(f"{x:.1f}" for x in t["pos"]) + ")" if t["pos"] else ""
            out.append(f"| {t['object'] or ''} | {pos} | {fmt_items(t['items'])} | {t['flag'] or ''} |")
        out += ["", "### Enemies", "", "| NPC param | Name | Placed | Blood echoes | Drops |", "|---:|---|---:|---:|---|"]
        for e in enemies[m]:
            out.append(f"| {e['npc']} | {e['name']} | {e['count']} | {e['echoes']} | {fmt_items(e['drops'])} |")
    awards = event_awards(w)
    out += ["", "## Given by event scripts", "",
            'Items handed over by `[2003:04]` / `[2003:36]` アイテム取得 ("obtain item") in the event scripts.', "",
            "| Script | Event | Event name | Lot | Items |", "|---|---:|---|---:|---|"]
    for stem in sorted(awards):
        for r in awards[stem]:
            out.append(f"| {stem} | {r['event']} | {r['name']} | {r['lot']} | {fmt_items(r['items'])} |")
    out += ["", "## In the Chalice Dungeons", "",
            "Items placed as treasure in each chalice's pre-built layouts: in how many of them.", "",
            "| Item | Chalice | Layouts with it |", "|---|---|---:|"]
    for name, rows in sorted(chalice_treasure(w).items()):
        for chalice, n, total in rows:
            out.append(f"| {name} | {chalice} | {n} / {total} |")
    out += ["", "## Sold in shops", "",
            "From `ShopLineupParam`. Price per tier; the unlock flag is the row's `qwcId`.", "",
            "| Item | Where | Prices by tier | Unlock flag |", "|---|---|---|---:|"]
    for name, rows in sorted(shops(w).items()):
        groups = collections.defaultdict(list)
        for where, price, flag, rid in rows:
            groups[(where.rsplit(", tier", 1)[0], flag)].append(price)
        for (where, flag), prices in groups.items():
            out.append(f"| {name} | {where} | {' / '.join(map(str, prices))} | {flag or ''} |")
    with open(sys.argv[4], "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("\n".join(out[3:5 + len(w.maps())]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
