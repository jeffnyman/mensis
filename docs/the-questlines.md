# The questlines

The NPCs' stories, read from the data: what state each character is in, what moves them on,
what they say in each state, and what they hand over. It builds on
[event flags](the-flags.md), which finds the 24 flag ranges that hold the characters' states.
This page turns each range into a **sheet** and reads one character's story from hers.

```
python -I tools/story.py ranges work/base/dvdroot_ps4 work/update/dvdroot_ps4
python -I tools/story.py sheets work/base/dvdroot_ps4 work/update/dvdroot_ps4 work/story/npcs
```

The sheets quote the dialogue in full, so they stay in `work/`. This page quotes a line or two.

## What a sheet holds

**The states.** Each flag of the range, the events and talk states that set it, and how often
it is read. The events' working names say what each flag means: `孤独な老婆_診療所へ`, "the
lonely old woman: to the clinic".

**The character's talk scripts.** A talk ID is a map number plus a character number, so
`t241316` (Central Yharnam) and `t240316` (Cathedral Ward) are one character in two places. A
sheet takes the script that reads or sets the range most, then every script in any map with the
same character number. It also takes every script whose placement uses the same NpcParam family
(`6110` and `6113`), since one character can be several rows. Counting references alone doesn't
work, because the chapel's residents all check on one another. The Old Woman's range is tested six
times each by fourteen other scripts, and only three times by her own window script.

**The conversations.** A talk script is a set of state machines. Its decisions are chains of
small states, each labelled for the developers with `c1_0("…")`: 話しかけられた ("spoken to"),
発狂していない ("not gone mad"), 挨拶_初回 ("greeting, first time"). A sheet follows every
path from a group's entry to each state that speaks. Each path lists the labels on the way, and
the conditions of the branches it takes, with the earlier branches *not* taken. So an `if 1`
branch, "otherwise", becomes the negation of what came before it.

```
- path: 話しかけられた › 発狂していない › 挨拶_初回
  when flag 1181 on, flag 72400340 off
- says 2600700:
  > Oh, no, I haven't forgotten.
  > ...
```

A path whose flag tests contradict each other is never taken, and it is dropped. A state that
sets a flag replaces what the path knew about that flag, since a conversation can change its
own flags. A state that no branch leads to is marked **not reached**. As far as that script is
concerned, its lines are never spoken.

**The lines.** A script says `c1_1(<talk id>)`, or calls a helper group with talk IDs. A talk
ID is a TalkParam row, the row's `msgId` keys the subtitle, and one call plays the rows after it
too, as one conversation. Pairs like `2601100` / `2601150` usually hold the same text. The helper
chooses one of the two by a test this page hasn't identified (`f45(12, 0, …)`). Working this out corrected
how the talk text had been looked up ([what the patch changed](the-patch.md#reading-them-toolsesdpy)).

**The gifts.** One common template, `NPCアイテム入手_XX` ("NPC item obtained", event 9040), is
started once per handover with a flag and an item lot. Its own flag, 9040 + its slot, is that
flag, so a talk script hands an item over just by setting it. A sheet marks such a state
**gives**, with the item. 28 handovers are found this way: the Tiny Music Box, the Powder
Keg and Wheel Hunter Badges, the Flamesprayer, Pungent Blood Cocktails, Beast Blood Pellets,
and the Old Woman's Sedatives.

## The lonely old woman

Range 1180–1199, talk scripts `t241316` (her window in Central Yharnam) and `t240316` (Oedon
Chapel). Everything below is read from her sheet, the flag index and her initialization event
(`孤独な老婆_初期化`), which runs on every load and moves her from one state to the next.

**At her window** (`t241316`), she asks the obvious question: "Oh, you're a hunter, aren't ya?
Then, well, do you know of any safe places?" The menu offers only places you have been told
about. Flag 9430 is set by the Oedon Chapel Dweller's script and 9431 by the false doctor's,
and the window's branch 教える場所が存在する ("there is a place to tell") tests the two. Then:

| You | Her state | Set by |
|---|---|---|
| send her to the chapel | 1193, on her way; then 1181, there | `…_避難所へ移動中へ`, `…_避難所へ` |
| send her to the clinic | 1194, on her way; then 1190, there | `…_診療所へ移動中へ`, `…_診療所へ` |
| refuse | stays at 1180: "Yeah, I should've known." | |

Two things can intervene. If Patches' range has reached its last flag (1439) while she is
still at the window or on her way, the window speaks with someone else's voice. The branch is
labelled パッチに乗っ取られている, "taken over by Patches", and it points you to "an ancient,
shrouded church". (The window has a second script, `t241352`, placed at exactly the same spot.)
And her refusal to open up, "I wouldn't open my door for me own poor mother", is in `t241316`
behind two greeting states that no branch reaches. As the script stands, it never plays.

**At the clinic** (1190), she is one of the residents the false doctor's events count:
`ニセ女医_住人を保護した` ("the false doctor has sheltered a resident"), and
`ニセ女医_月の使者生成_XX` ("generate a moon messenger").

**At the chapel** (`t240316`), her initialization moves her through a series of states. The
talk script calls these 発狂 ("madness") levels:

| State | When | Her script's label, and what she says |
|---|---|---|
| 1181 | on arrival | 発狂していない, not gone mad: "Oh, no, I haven't forgotten. Do you think I owe you something?" |
| 1185 | once it is Night (9801) | 発狂lv2: "Ohh, how did we ever get into this mess..." |
| 1186 | once 会話用_森前半クリア ("for conversations: first half of the Forbidden Woods cleared", 9467) is set | 発狂lv3: she takes you for her child: "Oh, there you are. You're home early, dear." |
| | | 在庫1–3, "stock": gives Sedatives, ×3, ×2 and ×1, as "This will help you forget..." |
| 1187 | after アイテム探索準備中 ("getting ready to look for the item", talk flag 72400900) | アイテム探索中, "looking for the item": she is away. Both her placements are hidden |
| 1188 | after the event 孤独な老婆_置き手紙 ("a note left behind") and one more load | 探索後, "after the search": back, and gives one more Sedative |
| 1189 | after you answer "yes" once she is back, and she promises more: "Now, now, have some patience... Mother'll make everything better..." (talk flag 72400350) | gone. Her second placement lies at the region `孤独な老婆がアイテムを探しに行って死亡`, "the lonely old woman went to look for an item and died", as a body holding a Sedative |

The other endings are deaths. 1183 (`…_状態変化_死亡`) and 1195 (`…_死亡`) are her dying at
the chapel and in Central Yharnam. 1191 is set by `感染者の乞食_他NPC殺害`, "the infected
beggar kills another NPC". After it, the dweller's events `避難所の盲人_惨劇1_人物特定_老婆`
("tragedy 1: identifying the victim: the old woman") and its second version wait on it, so
the chapel's account of the killing can be about her.

Flag 1184 is tested by her initialization and placed like the chapel states, but nothing in the
files sets it.

## What this doesn't cover yet

* **The other 23 sheets.** They are generated. Reading each into a story like the one above is
  the next step.
* **Untranslated conditions.** Talk functions other than `f15` print as numbers: `f23()` and
  `f22()` look like a menu's answer (the branch after `f23() == 1` is labelled Yesを選んだ,
  "chose Yes"), `f45(12, 0, …)` picks one of a line's two copies, and `f1()` is likely distance.
  The sheets show them as they are.
* **Event conditions besides flags.** An event that moves a character on may also wait on a
  region, a boss or the time. The sheet lists the event, and its dump says the rest
  (`events.py dump`).

## Files

| Path | |
|---|---|
| `tools/story.py` | the ranges, and a sheet per range |
| `work/story/npcs/<lo>-<hi>.md`, `index.md` | the sheets: states, talk scripts, every conversation and gift |
