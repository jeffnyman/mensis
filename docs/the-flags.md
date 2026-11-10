# Event flags: the game's world state

Bloodborne remembers what has happened in numbered **event flags**: a door opened, a boss dead,
an item picked up, a character sent to Oedon Chapel. A flag is one bit, and about ten thousand of
them are named somewhere in the files. Nothing names their *meaning*, but every place that reads
or sets one can be found, and that is what a questline is made of: what sets this flag, and
what waits on it? This page builds that index. [The questlines](the-questlines.md) are read from it.

```
python -I tools/flags.py index work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/story/flags-1.09.tsv
python -I tools/flags.py show  work/base/dvdroot_ps4 work/update/dvdroot_ps4 1181 1180-1199
```

`index` writes every reference, one per line. `show` prints what reads and sets a flag (or a
range), grouped:

```
== 1181
set by
          1180-1199  set flag range off                 m24_00_00_00 event 12400500  [孤独な老婆_初期化; started by m24_00_00_00 50, slot 0]
               1181  set flag on                        m24_01_00_00 event 12410744  [孤独な老婆_避難所へ; started by m24_01_00_00 50, slot 0]
               ...
read by
               1181  condition: flag on                 m24_00_00_00 event 12400708  [避難所の盲人_住人を保護_XX; started by m24_00_00_00 0, slot 1]
               1181  talk: flag                         m24_00_00_00 t240316.esd group 2147483642 state 3  [c0000 NpcParam 6090]
               ...
```

The bracketed names are the event's own name from the game's `.emeld` files: Japanese working
names, here `孤独な老婆` ("the lonely old woman") and `避難所の盲人` ("the blind man at the
sanctuary").

## Where flags are read and set

There are three kinds of source. For the 1.09 game, the index holds 29,290 references to 9,317
flags and 610 ranges:

| Source | References | Reads | Sets |
|---|---:|---|---|
| Event scripts (`event/*.emevd`) | 16,199 | conditions `[3:..]`, waits, skips, ends and jumps `[1003:..]` | `[2003:02]` set, `[2003:22]` set a range, a random flag, counters, ladders, lamps; and every event's own flag (below) |
| Talk scripts (`script/talk/*.talkesdbnd`) | 4,451 | `f15(flag)` | `c1_11(flag, state)` |
| Param tables | 8,640 | `ShopLineupParam.qwcId` (a shop row is for sale), `ActionButtonParam`, `ObjActParam` | `ItemLotParam.getItemFlagId` (set when the lot is picked up) |

**Event scripts.** The instruction set ([the data](the-data.md)) names every argument. So the
flag arguments are found by name (`対象イベントフラグID`, "target event flag ID", and so on),
not from a hand-made list, and a reference is a read or a set by its instruction's class.
Flag states come from the instruction set's own enums too. One flag is `ON・OFF・CHANGE`:
"changed" on a test, "toggled" on a set. A range of flags is `論理演算タイプ`, "logical operation
type": all on, all off, any on, any off. An argument whose ID type (`基準イベントフラグIDタイプ`)
says it holds an event ID rather than a flag is skipped.

**Templates.** Many events are templates whose flags are their arguments.
`[2000:00]` starts an event in a slot with arguments, and the template copies them into its
instructions. The index follows every start, including from a map's script into `common.emevd`,
and lists a template's flags once per copy. The Oedon Chapel dweller's `避難所の盲人_住人を保護_XX`
("shelter the residents", event 12400708) is started four times, once per resident who can be
sent to the chapel:

```
[2000:00] start(slot=0, event=12400708, 1164, 72400316, 1163, 1161, 0)
[2000:00] start(slot=1, event=12400708, 1181, 72400317, 1190, 1183, 0)
...
```

In slot 1 it tests flags 1181, 1190 and 1183 and sets 72400317, so the index lists exactly those,
from that copy. Started in slot *s*, an event also has its **own flag**, its ID + *s*, which the engine
sets when that copy ends ([the items](the-items.md#unlock-flags) use this: the
Bath Messengers' badge event in slot 7 sets 12101007, "owns the Cainhurst Badge"). The index
lists 5,449 of them.

**Talk scripts.** The talk functions have no names in the files, but the numbering is
the Dark Souls one: `f15` is `GetEventStatus` and `c1_11` is `SetEventState`. The data agrees: of
the 1,615 constants of 1000 and up passed to `f15` in the 1.00 talk scripts, 1,013 are flags that
also appear in the event scripts. A talk script's state groups call one another with arguments
(`c6_<group>(…)`, read inside as `StateGroupArg(n)`), and 45% of the `f15` calls get their flag
that way.
They are followed like event templates. Every call lands on a group, and every flag resolves to
a number.

A talk script `t<id>.esd` belongs to the characters whose map placement has talk ID `<id>`.
158 of the 236 talk scripts are matched that way. Most talking NPCs are the human model `c0000`.
A human's `nameId` is 0, and its name, where it has one, is keyed by its own NpcParam ID (6500 is
"Valtr, Master of the League"). Most have none, so the index says `c0000 NpcParam 6090` where it
can't say more.

**Params.** Picking up an item lot sets its `getItemFlagId`, which is why a picked-up item stays
gone. A shop row needs its `qwcId` flag before it is for sale, and has an `eventFlag` of its own.
Two tables gate a prompt (`ActionButtonParam`) or an object action (`ObjActParam`) on a flag.

## The NPCs' state ranges

Common event 50, `プリコンストラクタ` ("pre-constructor"), runs at load. Among other things, it
gives each of 24 ranges a starting value. The ranges are 20 flags each, except 1280–1299, which
is split into two of 10:

```
[1003:03] skip 1 if any of 1180-1199 is on
[2003:02] set 1180 on
```

On a new game, no flag in the range is on, so the first one is set. After that, the range holds
whatever the story has set. The events that set the other flags in a range are named after
**one character**, so each range is one character's state, held as exactly one flag on at a time:

| Range | Its events' working name | Talk scripts |
|---|---|---|
| 1000–1019 | `人形のヒロイン`, "the doll heroine" | `t210304` |
| 1020–1039 | `拠点老人`, "the old man of the base" | `t210305` |
| 1040–1059 | `穢れた血族の女王`, "queen of the defiled bloodline" | `t250307` |
| 1060–1079 | `学長`, "the headmaster" | `t320310` |
| 1080–1099 | `悪夢の主`, "master of the nightmare" | `t260311` |
| 1100–1119 | `避難所の盲人`, "the blind man at the sanctuary" | `t240312` |
| 1120–1139 | `診療所の女医`, "the clinic's woman doctor" | `t241313` |
| 1140–1159 | `ニセ女医`, "the false woman doctor" | `t241314` |
| 1160–1179 | `偏屈な老人`, "the cantankerous old man" | `t240215`, `t241315` |
| 1180–1199 | `孤独な老婆`, "the lonely old woman" | `t240316`, `t241316` |
| 1200–1219 | `感染者の乞食`, "the infected beggar" | `t270317` |
| 1220–1239 | `娼婦`, "the prostitute" | `t240218` |
| 1240–1259 | `重病人`, "the gravely ill man" | `t241319` |
| 1260–1279 | `少女`, "the girl" (and `少女の姉`, "the girl's elder sister") | `t241320` |
| 1280–1289 | (no event sets one) | `t241321` |
| 1290–1299 | `ガスコイン（ボス部屋）`, "Gascoigne (boss room)" | `t241330` |
| 1300–1319 | `囚われた尼僧`, "the captive nun" | `t240322`, `t280322` |
| 1320–1339 | `獣の守護者`, "the guardian of beasts" | `t230323` |
| 1340–1359 | `血族狩り`, "the hunter of the blood kin" | `t240333`, `t240343` |
| 1360–1379 | `復讐者`, "the avenger" | `t240325`, `t241325` |
| 1380–1399 | (no event sets one) | `t240326` |
| 1400–1419 | `門番`, "the gatekeeper" | `t240327` |
| 1420–1439 | `クモ男パッチ`, "Patches the spider-man" | `t320329` |
| 1440–1459 | (no event sets one) | `t241332` |

The names are the developers' working names, translated literally. Several are clear on their
own (Patches is the `c1110` of the [Chalice events](the-chalice-events.md)), but this page doesn't
put player-facing names on the rest. The questline pages identify each character from what
their talk script says.

The lonely old woman's range shows the pattern. The `set by` lines for 1180–1199 are:

| Flag | Set by |
|---|---|
| 1180 | the pre-constructor: the starting state |
| 1181 | `孤独な老婆_避難所へ`, "to the sanctuary" |
| 1190 | `孤独な老婆_診療所へ`, "to the clinic" |
| 1193, 1194 | `…_避難所へ移動中へ`, `…_診療所へ移動中へ`: on the way to the sanctuary, to the clinic |
| 1183, 1195 | `…_状態変化_死亡`, `…_死亡`: dead |
| 1191 | `感染者の乞食_他NPC殺害`, "the infected beggar kills another NPC" |
| 1185–1189 | `孤独な老婆_初期化`, her initialization |

Flag 1184 is tested, by her initialization among others, but nothing in the files sets it.

Every one of these events but the pre-constructor clears the whole range and then sets one flag,
so exactly one state holds at a time. Her initialization runs each time the map loads and moves her on from where she was:
at the sanctuary (1181) with the Night flag 9801 on, she moves to 1185. Each state places her
model, or her remains, differently.

## What this doesn't cover

* **The Chalice layouts' scripts.** The 2,641 per-layout scripts in `event/m29_*/` are left out:
  they are the rituals' business ([Chalice events](the-chalice-events.md)). So the 953
  references in the `m29.emevd` library that only those scripts start have no values here, and
  `index` reports them as left out. So are 47 references in templates that no script starts.
* **Flags only the executable touches.** 331 flags are read but set nowhere in the files, even
  counting range sets. Some are known: 9800–9802 are the time of day, which the executable keeps
  ([the items](the-items.md)). The Chalice rituals' 929… flags are set by the left-out layout
  scripts. The rest aren't worked out yet.
* **Talk functions other than `f15` and `c1_11`.** `f56` also takes flag-like numbers, but what
  it tests isn't known, so it isn't counted.
* **Names.** Flags have no names in the files. The event names and the talk scripts' characters
  are the context the index gives.

## What's next

[The questlines](the-questlines.md): each NPC's range read state by state, with the talk
script's dialogue for each state, the events that move them on, and what they hand over.

## Files

| Path | |
|---|---|
| `tools/flags.py` | the index and the `show` query |
| `work/story/flags-1.09.tsv` | every reference: flag, range end, read/set/own/param, what, where, note |
