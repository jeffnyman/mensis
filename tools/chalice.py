# SPDX-License-Identifier: GPL-3.0-or-later
"""The Chalice Dungeons as the data describes them: every ritual, its layouts and its rites.

    python -I tools/chalice.py <base dvdroot> <update dvdroot> <out.md>

The chain, read from the game (1.09 over 1.00):

    chalice item (EquipParamGoods id) == HolygrailExParam row id
        -> ritualLv (depth), holygrailTypeId (0 Pthumeru, 1 Hintertomb, 2 Loran, 3 Isz),
           mapBaseId -> layout folder(s) map/mapstudio/m29_<AB>_<C>?_00/ (one MSB per variation),
           variationNum (how many variations), requiredMaterialId -> RitualRequiredMatParam
           (echoes and materials), dungeonNameBaseId -> the place-name table (地名)
    rites: DungeonFeatureParam rows, named from menu SP_メニューテキスト; each can shift the NPC
           rows (npcParamIdOffset), gem generation (gemGenParamIdOffset) and add an enemy-wide
           special effect (eneSpeffectId)

mapBaseId ABC maps to layout folders m29_AB_C*: 100 -> m29_10_00_00, 302 -> m29_30_20_00,
401 -> m29_40_10_00 + m29_40_11_00 (depths 4-5 split 200 variations over two folders of 100).
"""

from __future__ import annotations

import io
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import items as itemlib
import souls

BASES = {0: "Pthumeru", 1: "Hintertomb", 2: "Loran", 3: "Isz"}


def layout_name(map_base: int, variation: int) -> str:
    """Variation v of the ritual with mapBaseId ABC is layout m29_AB_<C*10 + v//100>_<v%100>.

    (200-variation rituals span two folders: 401 -> m29_40_10_xx for 0-99, m29_40_11_xx for 100-199.)
    """
    ab, c = map_base // 10, map_base % 10
    return f"m29_{ab:02d}_{c * 10 + variation // 100:02d}_{variation % 100:02d}"


def unique_items(w, rituals, goods) -> list[str]:
    """Each Root ritual's unique items: HolygrailExParam uniqueItemNN_head/tail is a range of
    variations, and the item is the treasure whose pickup flag (ItemLotParam.getItemFlagId) is
    uniqueBaseFlagId + NN. When a ritual is rolled, the game picks among the ranges of the unique
    items whose flags are not yet set (weighted by range size), then a variation inside that range
    (docs/the-chalice-dungeons.md#how-a-ritual-picks-its-layout). This checks the layouts: how many
    in the range, and how many outside it, place that item."""
    import msb as msblib

    by_flag = {}
    for lid, lot in w.lots.items():
        f = lot.get("getItemFlagId", -1)
        if f > 0:
            by_flag.setdefault(f, []).append(lid)
    out = [
        "",
        "## Unique items and the ranges a ritual is steered to",
        "",
        (
            "For each unique item: its range of variations, its pickup flag, the item, and how many layouts "
            "inside / outside the range place it. A ritual is rolled inside the range of an item not yet "
            "picked up, so a range whose layouts all hold the item guarantees it."
        ),
        "",
        "| Chalice | # | Variations | Flag | Item | In range | Outside |",
        "|---|---:|---|---:|---|---:|---:|",
    ]
    for rid, r in sorted(rituals.items()):
        if rid == 0 or r["ritualLv"] > 5 or r["uniqueItem00_head"] == 255:
            continue
        flag_lots = {}
        for i in range(15):
            for lid in by_flag.get(r["uniqueBaseFlagId"] + i, []):
                flag_lots[lid] = r["uniqueBaseFlagId"] + i
        has = {}
        for v in range(r["variationNum"]):
            name = layout_name(r["mapBaseId"], v)
            p = w.path(f"map/mapstudio/{name[:10]}00/{name}.msb.dcx")
            if os.path.exists(p):
                lots = {
                    lot
                    for e in msblib.Msb(souls.load(p)).events
                    for lot in e.get("item_lots", [])
                }
                has[v] = {flag_lots[lot] for lot in lots if lot in flag_lots}
        for i in range(15):
            h, t = r[f"uniqueItem{i:02d}_head"], r[f"uniqueItem{i:02d}_tail"]
            if h == 255:
                continue
            flag = r["uniqueBaseFlagId"] + i
            item = (
                ", ".join(
                    sorted(
                        {it for lid in by_flag.get(flag, []) for it, _, _ in w.lot(lid)}
                    )
                )
                or "—"
            )
            inside = sum(flag in has.get(v, ()) for v in range(h, t + 1))
            outside = sum(flag in s for v, s in has.items() if not h <= v <= t)
            out.append(
                f"| {goods.get(rid, '').strip()} ({rid}) | {i} | {h}–{t} | {flag} | {item} | "
                f"{inside} / {t - h + 1} | {outside} |"
            )
    return out


def main():
    if isinstance(sys.stdout, io.TextIOWrapper):
        sys.stdout.reconfigure(encoding="utf-8")
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    w = itemlib.World(sys.argv[1], sys.argv[2])
    rituals = w.rows("HolygrailExParam")
    mats = w.rows("RitualRequiredMatParam")
    features = w.rows("DungeonFeatureParam")
    goods = w.names.get("EquipParamGoods", {})
    sp = {}
    for bf in souls.bnd4_read(souls.load(w.path("msg/engus/menu.msgbnd.dcx"))):
        if "SP_メニューテキスト" in bf.name:
            sp = souls.fmg_read(bf.data)
    ms = os.path.join(w.base, "map", "mapstudio")
    folders = {
        d: len(os.listdir(os.path.join(ms, d)))
        for d in os.listdir(ms)
        if d.startswith("m29_")
    }

    out = [
        "# Chalice Dungeons: rituals and rites\n",
        "Generated by `tools/chalice.py` from the game's own data (1.09 over 1.00).\n",
        "| Chalice (item id) | Depth | Base | Dungeon name | Variations | Layout folders | Echoes | Materials |",
        "|---|---:|---|---|---:|---|---:|---|",
    ]
    unused = []
    for rid, r in sorted(rituals.items()):
        if rid == 0:
            continue
        mb = r["mapBaseId"]
        cands = sorted(
            f
            for f in folders
            if mb and f.startswith(f"m29_{mb // 100}{(mb // 10) % 10}_{mb % 10}")
        )
        m = mats.get(r["requiredMaterialId"])
        mlist = []
        if m:
            for k in range(1, 6):
                if m[f"materialItemId_{k}"] > 0:
                    mlist.append(
                        f"{(goods.get(m[f'materialItemId_{k}']) or str(m[f'materialItemId_{k}'])).strip()} ×{m[f'num_{k}']}"
                    )
        name = goods.get(rid, "").strip()
        place = w.places.get(r["dungeonNameBaseId"], "").strip()
        row = (
            f"| {name or '—'} ({rid}) | {r['ritualLv']} | {BASES.get(r['holygrailTypeId'], r['holygrailTypeId'])} | "
            f"{place or '—'} | {r['variationNum']} | {', '.join(f'`{f}` ({folders[f]})' for f in cands) or '—'} | "
            f"{m['useSoul'] if m else '—'} | {'; '.join(mlist)} |"
        )
        if not name and not cands:
            unused.append(row)
        else:
            out.append(row)
    if unused:
        out += [
            "",
            "## Defined but not in the game",
            "",
            "Ritual rows with no chalice item, no dungeon name and no layout files:",
            "",
            "| Ritual (id) | Depth | Base | Dungeon name | Variations | Layout folders | Echoes | Materials |",
            "|---|---:|---|---|---:|---|---:|---|",
        ] + unused
    out += [
        "",
        "## Rites",
        "",
        "`DungeonFeatureParam` rows that carry a name or change something:",
        "",
        "| Feature | Name | NPC row offset | Gem generation offset | Enemy-wide effect | Gem drop correction | Sub-feature group |",
        "|---:|---|---:|---:|---:|---:|---:|",
    ]
    for fid, f in sorted(features.items()):
        name = sp.get(f["featureNameId"], "")
        if not (
            name
            or f["npcParamIdOffset"]
            or f["gemGenParamIdOffset"]
            or f["eneSpeffectId"] > 0
        ):
            continue
        out.append(
            f"| {fid} | {name} | {f['npcParamIdOffset'] or ''} | {f['gemGenParamIdOffset'] or ''} | "
            f"{f['eneSpeffectId'] if f['eneSpeffectId'] > 0 else ''} | "
            f"{f['gemDropCorrectParamId'] if f['gemDropCorrectParamId'] > 0 else ''} | {f['subFeatureGroupId']} |"
        )
    # Rite menus: DungeonSubFeatLotParam rows, id = depth*10000 + base*1000 + rite code. Each rite
    # has its own material row (same id) and picks one feature by weight.
    sub = w.rows("DungeonSubFeatLotParam")
    out += [
        "",
        "## Rite menus",
        "",
        (
            "`DungeonSubFeatLotParam`: per depth and base, each rite's cost and the features it picks "
            "(feature: weight %). Depths 6–7 belong to the unused rituals above."
        ),
        "",
        "| Row | Depth | Base | Rite | Cost | Picks |",
        "|---:|---:|---|---|---|---|",
    ]
    for sid, s in sorted(sub.items()):
        name = sp.get(s["subFeatureNameId"], "")
        if not name or sid >= 1000000:
            continue
        m = mats.get(s["requiredMaterialId"])
        cost = "; ".join(
            f"{(goods.get(m[f'materialItemId_{k}']) or str(m[f'materialItemId_{k}'])).strip()} ×{m[f'num_{k}']}"
            for k in range(1, 6)
            if m and m[f"materialItemId_{k}"] > 0
        )
        picks = ", ".join(
            f"{s[f'subFeatureId_{i}']}: {s[f'rate_{i}']:g}"
            for i in range(1, 11)
            if s[f"rate_{i}"] > 0
        )
        out.append(
            f"| {sid} | {sid // 10000} | {BASES.get((sid // 1000) % 10, '?')} | {name} | {cost} | {picks} |"
        )
    out += unique_items(w, rituals, goods)
    with open(sys.argv[3], "w", encoding="utf-8") as fo:
        fo.write("\n".join(out) + "\n")
    print("\n".join(out[:6]))
    print(
        f"... {len(rituals) - 1} rituals ({len(unused)} not in the game) written to {sys.argv[3]}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
