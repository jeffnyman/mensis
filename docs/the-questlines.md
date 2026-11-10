# The questlines

The NPCs' stories, read from the data: what state each character is in, what moves them on,
what they say in each state, and what they hand over. It builds on
[event flags](the-flags.md), which finds the 24 flag ranges that hold the characters' states.
This page turns each range into a **sheet**. It then reads one character's story from hers,
and the story she is part of: [the safe places](#the-safe-places), where six people can be sent
and what becomes of them. [Four more stories](#four-more-stories) follow: Djura, Eileen, Alfred
with the Queen of Cainhurst, and Gascoigne's daughters.

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
flag, so a talk script hands an item over just by setting it. A second template,
`NPCアイテム入手_何度でも_XX` ("…any number of times", 9100), clears its flag again after the
handover, so the gift can be had again: the Blood of Arianna and of Adella, Iosefka's Blood Vial,
Blue Elixirs, Queenly Flesh. The two are started 35 times. A sheet marks a state that sets one of
their flags **gives**, with the item. 32 of the 35 show up that way, among them the Tiny Music
Box, three Hunter Badges, the Flamesprayer and the Old Woman's Sedatives. Of the other three,
Queenly Flesh is given by an event, not a talk script. Tonsil Stone and one of the Blood Vials
are set by scripts the sheets don't select.

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

## The safe places

The Old Woman is one of six people you can send to safety. There are two refuges: Oedon Chapel,
kept by the blind man at the sanctuary (`避難所の盲人`, range 1100, the Oedon Chapel Dweller),
and Iosefka's Clinic. The clinic's doctor has two ranges: `診療所の女医`, "the clinic's woman
doctor" (1120), and `ニセ女医`, "the false woman doctor" (1140). Everything in this section is read from
their events in the Cathedral Ward (`m24_00`) and Central Yharnam (`m24_01`) scripts, and from
the residents' sheets.

| Resident (range) | Met in | On the way to chapel / clinic | At the chapel / clinic |
|---|---|---|---|
| `偏屈な老人`, the cantankerous old man (1160) | Cathedral Ward | 1168 / 1169 | 1164 / 1163 |
| `孤独な老婆`, the lonely old woman (1180) | Central Yharnam | 1193 / 1194 | 1181 / 1190 |
| `娼婦`, the prostitute (1220) | Cathedral Ward | 1232 / 1233 | 1224 / 1223 |
| `囚われた尼僧`, the captive nun (1300) | Yahar'gul, Unseen Village | 1313 / 1314 | 1304 / 1309 |
| `感染者の乞食`, the infected beggar (1200) | Forbidden Woods | 1208 / 1209 | 1205 / 1204 |
| `少女`, the girl (1260) | Central Yharnam | 1265 / 1266 | — / 1268 |

As with the Old Woman, each resident's initialization event moves them from "on the way" to
"there" on a later load.

**Which places you can offer.** A resident's dialogue offers a refuge only once its flag is
set. The chapel's is 9430, set by the dweller's talk script. The clinic's is 9431, and only one
script sets it: the false doctor's (`t241314`). The real doctor's range has nothing like it. The
false doctor arrives at Evening: `ニセ女医_通常` ("normal") moves her from 1140, "not here yet",
to 1141 once flag 9800 is on. Until she has told you about it, the clinic can't be offered.

### The chapel

The dweller's `避難所の盲人_住人を保護_XX` ("shelter the residents", event 12400708) is started
four times: for the old man, the old woman, the nun and the prostitute. Each copy waits until
its resident is at the chapel, at the clinic or dead, whichever comes first. Only the first
counts: it adds one to a 3-bit count of refugees (12400733) and sets the resident's "arrived"
talk flag. Around that count:

* `…_避難人数カウント` ("count of refugees") sets 72400309 once the count reaches three.
* `…_最後の避難者_行き先判定` ("the last refugee: where did they go") records whether the most
  recent of the four went to the chapel (72400307) or the clinic (72400308).
* `…_依頼成功` ("request succeeded", 1104) needs all four at the chapel, alive. `…_依頼失敗`
  ("request failed", 1105) waits until all four copies of the shelter event have ended, so every
  fate is settled, and at least one of them isn't at the chapel.
* `…_惨劇1` and `…_惨劇2` ("tragedy 1, 2", dweller states 1102 and 1103) follow the first and
  second of the four to die. Four identification events per tragedy (`…_人物特定_老人`, `_老婆`,
  `_娼婦`, `_尼僧`) record who it was. The second set waits for the first to have fired.

The beggar and the girl aren't among the four. The beggar reaches the chapel through his own
event (`感染者の乞食_避難所状態へ`, "to the sanctuary state"). The girl never gets there: sending
her to the chapel (1265) and her running off in despair after her mother's death (1264,
`少女_母が死んだので自暴自棄`) both lead to the same state, 1267. From 1267, or from 1271, its
later form once flag 9802 (the Eccentric Moon) is on, the event `少女_吸血豚が血塗れのリボンを落とす`
("the blood-sucking pig drops the bloody ribbon") applies. It checks that you don't already own
the Red Messenger Ribbon. Only the clinic route (1266, then 1268) brings her anywhere.

### The beggar

`感染者の乞食_他NPC殺害` ("the infected beggar kills another NPC", event 12400629) is the chapel's
danger. It runs on load, and it kills one resident at a time, always in the same order:

| Victim | Their state | Only while they're |
|---|---|---|
| the old man | 1166 | at the chapel (1164, 1165) |
| the old woman | 1191 | at the chapel (1181, 1184–1188) |
| the nun | 1312 | at the chapel (1304–1307) |
| the dweller | 1108 | in any of his states 1100–1105 |
| the prostitute | 1231 | at the chapel (1224–1226, 1228, 1229) |

He has to have kills to spend. Two common events, `感染者の乞食_他NPC殺害カウント_地上` and
`…_ダンジョン` ("kill count: surface", "dungeon"), add one to a 3-bit counter (70000200–202, at
most 7) whenever a boss-defeat flag changes. That covers the main game's bosses and the Chalice
Dungeons' alike. Both count only while he is at the chapel (1205, or transformed, 1207) and talk
flag 72400360 is set (in his chapel conversations). Each run of the killing event then spends
one kill on the next victim. In his "on the
way" state (1208), the event goes straight to a killing and spends nothing. When the dweller is
the victim, `避難所の盲人_乞食に殺される` ("killed by the beggar") leaves his body as one you can
loot.

His `感染者の乞食_満足` ("satisfied", 1211) is the other way his story closes. It needs the
dweller dead, and every other resident gone, at the clinic or dead, none of them by his hand
(the five "killed by the beggar" flags 72400934–938 all off).

### The nun and the prostitute

The nun's initialization has a branch of its own. If both women are at the chapel (the nun in
1304 or 1305, the prostitute in 1224–1227), and the prostitute has given her blood (72400942,
set by `娼婦_血の施しを行った`, "the prostitute gave her blood") and the nun has already talked to
you there (72400380), it sets the nun to 1306 and the prostitute to 1230. The prostitute's own
events treat 1230 exactly as her death (1222), and the dweller's tragedy events count it as
hers. The nun also has a state `娼婦に嫉妬`, "jealous of the prostitute" (1305).

The prostitute's last stage is elsewhere. From 1228, with talk flag 72400486, the Central Yharnam
map's initialization moves her to 1229. There a second placement of hers (NpcParam 6113) stands
beside a `c2521` character:

* `娼婦_娼婦が産んだ月の幼生を見たのでSAN値UP` ("saw the moon larva the prostitute bore, so sanity
  up") gives three points of `SAN値` ("sanity") through common event 9350 when you enter a region
  of the map (2412302) while she is in 1228 or 1229.
* `娼婦_月の幼生が死亡したら死亡させる` ("if the moon larva dies, make her die") ends her (1236)
  the moment the `c2521` is killed while she lives.

### The clinic

The false doctor takes residents in only in two of her states: `受付中`, "receiving" (1142, after
talk flag 72410320 in her script), and `共犯（外）`, "accomplice, outside" (1144).
`ニセ女医_住人を保護した` ("has sheltered a resident") is started six times: for the old man (1163),
the beggar (1204), the girl (1268), the old woman (1190), the prostitute (1223) and the nun
(1309). Each adds one to a 4-bit count (12410646), which sets 72410328 at four. `…_全員保護した`
("has sheltered everyone", 72410333) needs all six.

What becomes of them is the next event. `ニセ女医_月の使者生成_XX` ("generate a moon messenger")
is started six times, each with a `c2500` character in the clinic, the flag that makes it
appear, and the animation it plays. Five of the flags are residents' "at the clinic" states:
the old man's, the beggar's, the girl's, the old woman's and the prostitute's. The beggar's and
the prostitute's play animations of their own (7001, 7000). The nun has none. The sixth flag is
1124, the real doctor's `どこにもいない`, "nowhere". Her range's other states are 1121, "comes to
exist", 1122, "likes you more", and 1123, "fed up with you hitting the door". So a moon
messenger appears for each of those five sent there, and one for the doctor herself.

The false doctor's own states run on: `共犯（診療所）`, "accomplice, in the clinic" (1143), when
you step inside; `敵対`, "hostile" (1145); `狂った`, "gone mad" (1146), once 9802 is on; and death
(1148, 1149).

## Four more stories

The hunters' and the girl's ranges, read the same way. Three of the characters are named by the
game's own text. Alfred introduces himself ("Oh, beg pardon, you may call me Alfred"). Range
1320 is in Old Yharnam and drops the Powder Keg Hunter Badge, and Djura's attire is "of the
retired hunter Djura", the "old hunter Djura in Old Yharnam". Range 1360 drops the Crow Hunter
Badge, and its character says "Leave the hunting of hunters to me". Eileen's attire is that of
"Eileen the Crow, Hunter of Hunters".

### Djura (1320–1339, `獣の守護者`, "the guardian of beasts")

| State | Set by | When |
|---|---|---|
| 1320 | `…_通常`, normal | you are in his area and he is none of hostile, dead or friendly |
| 1321, 1322 | `…_警告`, warning; `…_威嚇`, threatening | you step into regions near him; his team switches to 23, which his hostile state also uses |
| 1325 | `…_友好`, friendly | from normal, once his talk flag 72300305 is set |
| 1323 | `…_敵対`, hostile | below |
| 1324 | `…_死亡`, dead | |

The choice is a menu of his: "Hunt the Beasts of Old Yharnam" or "Spare the Beasts of Old
Yharnam". Choosing to spare them is the branch labelled はい ("yes"). It sets 72300305 and plays
"Yes, very good.", and it gives the Powder Keg Hunter Badge if you haven't been given it (state
友好の証を入手, "obtained the token of friendship"). Choosing to hunt them plays "Of course, I
thought as much." and sets 72300307, and his hostility event waits on that flag. Leaving the
menu (回答保留, "answer on hold") is remembered (72300313), and his later greetings include
回答催促 ("pressing for an answer").

He turns hostile when you damage him, or when that "hunt" flag is set. Once he is friendly, he
also turns hostile through `獣の守護者を裏切る` ("betray the guardian of beasts"). That template is
started 34 times, once per creature he guards (31 `c1090`, 2 `c1290`, one `c0000`). Damaging
any one of them while he is friendly ends its copy, and any of those 34 flags turns him.

### Eileen (1360–1379, `復讐者`, "the avenger")

| State | Set by |
|---|---|
| 1362 | `…_初対面（聖堂街A）`, "first meeting (Cathedral Ward A)": entering the region `…_ヘンリック出現` in the Cathedral Ward |
| 1361 | `…_面識有（聖堂街B）`, "acquainted (Cathedral Ward B)": a conversation in the Central Yharnam map (talk flag 72410540) |
| 1363 | `…_面識有（聖堂街A）（ヘンリック出現）`, "acquainted, Henryk appears": from 1361 by the same region, or from 1362 after a conversation (72400520) |
| 1365 | `…_共闘`, "fighting together": she warps in beside you once Henryk's AI (NpcParam 6400, "Old Hunter Henryk") turns to state 3 |
| 1366 | `…_ヘンリック単独撃破`, "Henryk killed alone": he dies while she is only acquainted (1363) |
| 1367 | `…_ヘンリック共闘撃破`, "Henryk killed together": he dies while she fights alongside (1364, 1365) |
| 1373, 1374, 1375 | `…_一時的にいない（衰弱ルート）`, `（闇堕ちルート）`, `…_どこにもいない`: "temporarily absent (weakened route)", "(fallen route)", "nowhere" |
| 1370, 1372 | `…_衰弱`, "weakened"; `…_闇堕ち`, "fallen into darkness" |
| 1371 | `…_衰弱2`, "weakened 2" |
| 1368, 1369 | death, hostile |

Henryk's appearance (`…_ヘンリック出現`) waits on her being at 1363–1365. It is closed for good
once he is dead, or once the first half of the Forbidden Woods is cleared (9467), or once the
Eccentric Moon has risen (9802). Her place for that fight is named after Gascoigne's room
(`…_立ち位置03_ガスコイン部屋出口A`, "stand point 03, Gascoigne room exit A").

What follows depends on that fight. When the first half of the Forbidden Woods is cleared
(12700710):

* **Henryk killed together** (1367) leads to the weakened route (1373), and at the Eccentric Moon
  to 衰弱, "weakened" (1370).
* **Any other acquaintance** (1361, 1363, 1364, 1365, or hostile 1369) leads to the fallen route
  (1374), and at the Eccentric Moon to 闇堕ち, "fallen into darkness" (1372).
* **Only a first meeting**, or none (1360, 1362), leads to "nowhere" (1375).

Henryk killed without her (1366) is in none of the three; the events leave her there. From
weakened, she moves on to "weakened 2" (1371) when a second character placed in the Cathedral
Ward dies (NpcParam 6410, which has no name in the text). Her 強衰弱会話初回 conversation ("severely
weakened, first conversation") then gives a blood gem. Her first greeting gives four Bold
Hunter's Marks, and the Crow Hunter Badge is what her character drops.

### Alfred and the Queen (1340–1359, `血族狩り`, "the hunter of the blood kin"; 1040–1059, `穢れた血族の女王`, "queen of the defiled bloodline")

Alfred starts in the Cathedral Ward. At Night (9801), his initialization moves him from 1340 to
1344. His menu offers "Talk about the Healing Church", "Talk about Byrgenwerth" and "About the
Corrupted Vilebloods". It offers "Give Unopened Summons" as well, but only while you own the item
(`f47(3, 4308, …)`). Giving it, and confirming, takes the summons (`c1_52(3, 4308, -1)`). He
says "Ah-hah! Is that... the sigil of Cainhurst?", gives the Wheel Hunter Badge, and sets
72400465, and `…_招待状を渡した` ("the invitation was handed over") moves him to 1347 and saves.
His last line of the scene is "Ahh, I feel my master's hand at work."

That one handover moves two stories:

* **Alfred goes to Cainhurst.** The castle's initialization moves him from 1347 to 1348, and
  talking to him there leads to 1351, "already spoken to". After that, `…_古城以外に移動した`
  ("moved somewhere other than the old castle", common 9770) notices you leave Forsaken Cainhurst
  Castle, and both maps' initialization events set 1343. In the Cathedral Ward, 1343 leaves a
  placement of his as a body you can loot.
* **The Queen is reduced to flesh.** Her initialization moves her from 1040 to 1042 as soon as
  Alfred is in 1347, 1348 or 1349. The other way to 1042 is through 1041: she is attacked 50
  times (`…_50回攻撃されたら肉塊準備へ`, "attacked 50 times: get the lump of flesh ready"). In 1042,
  seeing the flesh gives three `SAN値` points (`…_肉塊を見たらSAN値上昇`), and an action button
  gives Queenly Flesh through the repeatable item template, or "It writhes eerily." if you already
  have it.

The summons itself comes from the castle: `女王の間_無記名の招待状` ("the Queen's chamber: the
unsigned invitation") awards the Unopened Summons from an object there, once 12500810 is set,
the own flag of `ボス部屋後_城表示制御` ("after the boss room: castle display control").
Alfred has two Cathedral Ward placements, each with its own copy of `…_敵対_XX` ("hostile",
from a talk flag or 90% health) and `…_死亡_XX`.

### The girl and her elder sister (1260–1279, `少女`; 1440–1459)

The girl's family is named by the music box she gives: it "Plays a song shared by her mother and
father", and inside the lid "Two names can be made out, however faintly. Viola and Gascoigne."

The girl speaks from her window in Central Yharnam (`t241320`, a `c9010` window talker). Her
first conversation asks you to find her mother. The menu is "Find Girl's Mother" or "Refuse",
and choosing to find her gives the Tiny Music Box. Her states then follow the night: `母を探している序盤へ` ("looking for her
mother, early"), then later stages by the time-of-day flags. A brooch you might carry changes her
lines (ブローチを持っている, "has the brooch"). Her mother's death makes her
`母が死んだので自暴自棄`, "in despair because her mother died" (1264). [The safe
places](#the-chapel) cover where she can be sent.

Her window script has a second speaker. Once the girl has gone (1267, at the clinic 1268, or
1269), the branch 少女姉, "the girl's elder sister", answers instead: "Oh, you haven't by any
chance... seen my little sister, have you?" and "She's still quite small, and wears a big white
ribbon." Her branches test whether you carry the ribbon or the brooch (リボンを持っている,
ブローチを持っている). Giving her the ribbon sets talk flag 72410421, `少女の姉_リボンを渡す`
("give the elder sister the ribbon") sets 1272, and the girl's state event moves that to 1273.
Range 1440–1459 has no events of its own. Its one script, `t241332`, is the elder sister's
again: "What a perfect ribbon... And now it's mine..." And while the girl herself is still
there, her window can be taken over by Patches (パッチに乗っ取られている, once his flag 1439 is
on), as the Old Woman's can.

## What this doesn't cover yet

* **The other sheets.** They are generated. Patches (1420), the Hunter's Dream ranges (the
  doll, Gehrman), the "headmaster" and "master of the nightmare" ranges, and the DLC characters
  (who aren't in the 24 ranges) are still to be read.
* **Untranslated conditions.** Talk functions other than `f15` print as numbers. Some are
  pinned down by their use. `f23()` returns the number of the option chosen from a `c1_19` menu:
  Djura's "Spare the Beasts" is option 2, and `f23() == 2` leads to はい. `f22()` answers a
  `c1_17` confirmation, with 1 for yes, as in Alfred's "Give Unopened Summons?". `f47(type, id,
  …)` tests that you own an item: `f47(3, 4308, …)` is the Unopened Summons. `f45(12, 0, …)`
  picks one of a line's two copies, and `f1()` is likely a distance. The sheets show them as
  numbers.
* **Event conditions besides flags.** An event that moves a character on may also wait on a
  region, a boss or the time. The sheet lists the event, and its dump says the rest
  (`events.py dump`).

## Files

| Path | |
|---|---|
| `tools/story.py` | the ranges, and a sheet per range |
| `work/story/npcs/<lo>-<hi>.md`, `index.md` | the sheets: states, talk scripts, every conversation and gift |
