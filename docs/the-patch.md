# What the 1.09 patch changed in the code

The update package replaces `eboot.bin` along with a few hundred data files
([the package](the-package.md#the-update-is-file-replacement)). This page covers the code side:
which functions the patch changed, what can be read from the changes, and how that was worked
out. Every step uses tools from this repository, so it doubles as a worked example of the whole
pipeline.

## Method

1. **Both executables in Ghidra.** `analyze.ps1` on `work/base/eboot.bin` (1.00) and
   `work/update/eboot.bin` (1.09). See [Loading it into Ghidra](ghidra.md).
2. **Names in both.** `NameFromStrings.java` (step tables, profiler markers, singletons) and the
   hand-named helpers via `ApplyNames.java`, carried to 1.00 with `port_names.py`.
3. **Pair the functions.** `ExportFunctionHashes.java` on each build, then
   `build_diff.py … --pairs pairs.tsv` aligns the two function lists
   ([Comparing two builds](ghidra.md#comparing-two-builds)).
4. **Decompile both sides.** `Decompile.java <dir>/ @list` on each build, for every function the
   alignment did not find identical.
5. **Diff the C.** `tools/c_diff.py` normalises the decompiled text and writes unified diffs,
   largest first.

```
python -I tools/build_diff.py work/ghidra/diff/eboot-1.00.functions.tsv work/ghidra/diff/eboot-1.09.functions.tsv \
       work/ghidra/diff/diff-1.00-to-1.09.md --pairs work/ghidra/diff/pairs-1.00-to-1.09.tsv
analyzeHeadless … -process eboot-1.00.ghidra.elf -noanalysis -readOnly -postScript Decompile.java "work/ghidra/diff/c-1.00/" "@old.list"
analyzeHeadless … -process eboot-1.09.ghidra.elf -noanalysis -readOnly -postScript Decompile.java "work/ghidra/diff/c-1.09/" "@new.list"
python -I tools/c_diff.py work/ghidra/diff/pairs-1.00-to-1.09.tsv work/ghidra/diff/eboot-1.00.functions.tsv \
       work/ghidra/diff/eboot-1.09.functions.tsv work/ghidra/diff/c-1.00 work/ghidra/diff/c-1.09 work/ghidra/diff/c-diff-1.00-to-1.09.md
```

(The output directory ends in `/`, not `\`: through `cmd.exe`, a trailing backslash escapes the
closing quote.)

### Normalising decompiled C

Two decompilations of the same function from different builds differ in many ways that mean
nothing, and `c_diff.py` removes each of them before diffing:

| Noise | Example | Treatment |
|---|---|---|
| Callees at new addresses | `FUN_0247fa50` vs `FUN_02480e40` | old addresses mapped to their 1.09 partner through the alignment (including `thunk_FUN_…`) |
| Data at new addresses | `DAT_05940640`, `PTR_FUN_057a88e0`, `LAB_…` | replaced by placeholders |
| Raw data addresses as numbers | `if (0x5946477 < p)` vs `0x59464a7` | hex literals inside the data segment (`0x54dc000–0x5ae1000`) become `0xDATA` |
| Variable numbering | `local_38`, `iVar3`, `uStack_c4` | declarations dropped; names renumbered `v1, v2, …` by first use |

What normalisation can't remove is worth knowing too, because it shows up in the diffs:

* **Inlining.** A helper inlined in one build and called in the other looks like a big change:
  the call disappears and its body appears. Most of the 8 "removed" functions in the build diff
  are helpers that 1.09 inlined.
* **Numbers that look like addresses.** `v10 = FUN_01c9be70` in 1.00 against `v10 = 30000000`
  in 1.09 is *the same constant*. 30,000,000 is `0x1C9C380`, which happens to be the start of a
  function in 1.00, so Ghidra displayed the number as a function pointer there and not in 1.09.
  Most of the 2-line "constants" diffs are this.

## Scale

| | Functions |
|---|---:|
| identical code in both builds | 158,324 (incl. 484 moved by the linker) |
| differ in binary, same decompiled logic | 22 |
| **differ in decompiled logic** | **97** |
| only in 1.00 / only in 1.09 | 8 / 8 (mostly inlining) |

The 97 hold about 7,900 changed lines of decompiled C. About a dozen of the largest (several
hundred lines each) are vector-math-heavy renderer internals: no strings, dozens of float and
SIMD temporaries, four of them copies of one template. They can't be read meaningfully until the
graphics framework itself is named, so they are listed but not interpreted here. The rest are
smaller, and the strings and named managers they touch make them readable.

## Findings

### 1. Developer remote tooling switched off

The clearest change in the patch. 1.00 shipped with developer hooks that a system property
could turn on. 1.09 removed them.

**The Lua console.** In 1.00 the Lua console object's constructor reads `Game.IsUseLuaConsole`.
If set, it remaps the `aiscript` and `luascriptpatch` mounts onto `game:/script_interroot`, the
developers' script tree, so AI scripts load from there and not from the game data. It also reads
`FsrmiWrapper.Port` (default 30000) and sets up a connection on that port:

```diff
-  WString_FromCStr(v6,L"Game.IsUseLuaConsole");
-  v3 = FUN_024ebcc0(v5,v6,0);
-  if (v3 != '\0') {
-    FUN_0246ec90(v5,L"aiscript",L"game:/script_interroot");
-    FUN_0246ec90(v5,L"luascriptpatch",L"game:/script_interroot");
-    ...
-    WString_FromCStr(v6,L"FsrmiWrapper.Port");
-    v4 = FUN_024eba00(*(undefined8 *)(v2 + 8),v6,30000);
-    FUN_?02c321e0(v4,DAT_xxx,0x10000,0,0);
-  }
+  param_1[2] = 0;
+  *param_1 = &PTR_FUN_0578d530;
+  ...
+  v1 = FUN_02450490(param_1,0x20);
```

In 1.09 the constructor only builds the object. That shrinks it from 0x40 to 0x30 bytes, and the
boot step that creates it (`SprjSystemStep::STEP_Init_forBootPhase4`) now inlines it. Along with
it:

* the graphics system's initialisation (`02377250`) no longer reads `FsrmiWrapper.Port`;
* the AI Lua debug menu (`016d7f60`) no longer checks `Game.IsUseLuaConsole`;
* of the other functions that read `Game.IsUseLuaConsole`, two shrink to 3-byte stubs, one from
  199 to 61 bytes, and one is removed. A function reading `FsrmiWrapper.Port` through the system
  properties also becomes a 3-byte stub.

**Remote debugging.** `DbgRemoteStep::STEP_Update` (a name from the step tables) polled three
remote-probe functions every update in 1.00. In 1.09 it checks its singleton and returns:

```diff
   if (DAT_xxx == 0) { AssertFailed(..., "DbgRemote"); }
-  if (*(long *)(DAT_xxx + 8) != 0) { FUN_013bc580(*(long *)(DAT_xxx + 8),param_2); }
-  if (*(long *)(v1 + 0x10) != 0) { FUN_?023b5d50(); }      // TestRemoteProbe / ClientSendTest
-  if (g_ReqReloadRemoteProbe != 0) { FUN_?023b4410(); }
-  if (*(long *)(v1 + 0x18) == 0) { return; }
-  FUN_?023b3a00();                                          // ClientSendWithReply_ReqPos
   return;
```

The three probe functions (`TestRemoteProbe`, `ClientSendWithReply_ReqPos`, …) are among the 8
removed outright.

*What this doesn't show:* whether these hooks could be enabled on a retail console. Both
depend on system properties whose source on a retail system hasn't been examined.

### 2. SOS-sign requests: a limit read live

SOS signs are the summon signs for co-op. `CSRequestGetSosStep` is the step-machine class that
fetches them. In 1.00 its `STEP_Init` read a limit once, from the network client manager's
configuration (`+0x8b4`, default 5), and cached it at `+0x128`. `STEP_Update_forSeamless` then
compared its counter at `+0x124` against the cached copy. In 1.09 the update reads the limit
every time:

```diff
 void CSRequestGetSosStep::STEP_Update_forSeamless(long param_1)
-    if (*(int *)(param_1 + 0x124) < *(int *)(param_1 + 0x128)) {
+    v1 = 5;
+    if (*(long *)(g_SprjNetworkClientMan + 0x18) != 0) {
+      v1 = *(int *)(*(long *)(g_SprjNetworkClientMan + 0x18) + 0x8b4);
+    }
+    if (*(int *)(param_1 + 0x124) < v1) {
```

So a change to the network configuration now takes effect immediately, not at the step's next
init. Two of the smallest diffs belong to the same change: both remove the `+0x128` field from a
table that registers the object's fields (`FUN_02d85480(…, param_1 + 0x128)`).

### 3. Graphics: a default scale buffer

`SprjGraphicsStep::STEP_Prepare_forSystem` creates an object (0x38 bytes in 1.00, 0x40 in 1.09)
whose 1.09 constructor allocates a 16-byte block holding four `1.0f` and hands it to
`FUN_0257f6c0(buf, 0x10, 1, data, 0)`. That is the same sequence the lightmap code uses to
build its `gxgi_dummy_lightMap_HDR_scale` constant. So 1.09 appears to give this graphics object
its own default scale of 1.0, though what it scales hasn't been traced.

### 4. Debug menu: blood gem names

The function that builds the debug menu's blood gem lists (`forGemList`, `gem%d_%d`) now looks up
each gem's name in the message repository, with `?GemName?` as the fallback. That's developer
tooling, not gameplay.

### 5. Online play, items, events

Further changes sit in functions that use `FrpgNetMan`, `SprjNetworkClientMan`,
`CSMultiPlayMan`, `CSPlatformNetworkMan`, `SprjSessionManager`, `SprjGaitem`,
`SprjEventFlagMan`, `SprjEventMan`/`SprjLuaEventMan` and `WorldChrMan`. Some examples:

* a network setup function next to `Game.Network.IsCheckServerVeryfy` and `EmergencyNotice`
  drops a read of a `FrpgNetMan` configuration value (passed on with arguments that look like a
  0–10 range);
* the character-sync function using `ChrSync.FrameSkipCount` now asks the character for a
  value through a vtable call (`+0x498`) where 1.00 computed it.

These are real logic changes, but explaining what they *mean* needs the managers' methods named
first. They are the natural next targets for hand-naming.

## The balance changes: in the data, not the code

The patch notes' headline, "Improved game balance", isn't in the executable at all. It's in the
replaced `param/gameparam/gameparam.parambnd.dcx`: the 65 tables of numbers the game plays by
([the data](the-data.md#params)).

### Reading the tables: `tools/params.py`

A PARAM file is a table of fixed-size rows with no field names. The names and layout are in a
PARAMDEF, and the game ships all 63 of them in `paramdef/paramdef.paramdefbnd.dcx`. Each field
has a Japanese display name, a storage type and an internal name:

```
behaviorVariationId   s32   行動バリエーションID   (behaviour variation ID)
isEnableDropSoulCapture:1   u8   ドロップソウル所持可   (can hold dropped souls)
```

`params.py` decodes rows through their PARAMDEF. Two details needed fixing before every table
decoded:

* **The storage type is not the internal type.** The internal type is often an enum name
  (`ACTION_BUTTON_REGION_TYPE`); the actual storage type (`s32`, `u8`, …) is the *display* type
  at offset 0x40 of the field record.
* **Padding bits share units with flags.** Bit fields of one type pack into a shared unit, and
  `dummy8 pad:5` fills the rest of the `u8` that the flags before it started. Treating padding as
  its own type made 9 tables one byte too long per row. With the fix, every field lands on its
  natural alignment (`breakSfxId`, an `s32`, at offset 12).

`params.py check` confirms every table's row stride matches its PARAMDEF: **65/65** in both
builds. `params.py diff … --msg item.msgbnd.dcx` compares two builds and labels weapons, armour
and goods with their English names.

```
python -I tools/params.py diff work/base/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx \
       work/update/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx \
       work/base/dvdroot_ps4/paramdef/paramdef.paramdefbnd.dcx work/params/gameparam-1.00-to-1.09.md \
       --msg work/update/dvdroot_ps4/msg/engus/item.msgbnd.dcx
```

### What changed

13 of the 65 tables changed. IDs were traced to names through the tables themselves: weapons
through `behaviorVariationId`, enemy attacks through `BehaviorParam` to the `NpcParam` rows of
the same behaviour variation, and characters through `nameId` and the `NPC名` text table. Where
no name exists, the enemy model was located by searching the map layouts (`map/mapstudio/*.msb`)
for its model ID.

**Player weapons**

| Change | Detail |
|---|---|
| **Boom Hammer**: cheaper to use | stamina cost of 17 moves (`BehaviorParam_PC` 1028xxxxx) cut by 17–29%: 30→25, 40→30, 50→40, 60→50, 70→50, 80→60, 90→70, 100→75, 120→90 |
| **Whirligig Saw**: three attacks toned down | attacks 3100930–3100932: poise damage (`atkSuperArmorCorrection`) 80/65 → 30, HP regained on hit (`wepRegainHpScale`) 65 → 30, damage 65 → 60 |
| one more attack (3800932) toned down | all damage corrections 350 → 320; its owner wasn't traced (no player behaviour references it) |

**Enemies: all in The Old Hunters' areas**

| Enemy | Where | Change |
|---|---|---|
| **Valtr, Master of the League / Beast Eater** | NPC 6502, 6505 | his resident effect (7915/7916) now cuts incoming damage by 25%, not 35%, for every damage type |
| enemy families 402xxx and 412xxx (models c4020/c4021, c4120) | Research Hall (m35) | 32 rows (30 + 2) gain a resident effect (5346/5351) that sets how hard each kind of hit staggers them (`dmgLv_*`) |
| **Living Failures** (403xxx) | Research Hall (m35) | can no longer pick up blood echoes the player dropped (`isEnableDropSoulCapture` 1 → 0) |
| enemy 406xxx (model c4060) | Fishing Hamlet (m36) | five attacks hit less hard: 220→120, 200→160, 180→120, 180→160, 220→160 |
| enemy 450000 (model c4500) | Hunter's Nightmare (m34) | two pairs of attacks **swap** their damage (194↔136, 229↔194): a mix-up fixed. One attack's damage level 6 → 3 |
| enemy 402xxx | Research Hall | two attacks change their hit material (sound/effect) 3 → 6 |
| AI | NpcThinkParam 7043, 7161 | 7043: the action taken when path search fails (`actTypeOnFailedPath`) changes from type 2 to 0. 7161: the "return home" time after touching an enemy wall (`BackHomeLife_OnHitEneWal`) goes from 5 seconds to 0 |

**Items, shops, gems**

| Change | Detail |
|---|---|
| **Blood Rock** can be bought | added to five shop line-ups (200025–240025) at a price of 60. The same line-ups sell Blood Stone Chunk (row xx0024) at 20 |
| **Vermin** | can no longer be used several at once (`canMultiUse` 1 → 0) |
| three blood gem generation rows | 90330, 90340, 90341 (gem effect 1013) become usable in weapons (`equipableSegmentWeapon` 0 → 1) |
| Butcher Garb | one hide-model flag cleared (`invisibleFlag28`) |

**Housekeeping:** `TalkParam`'s 1,516 changed fields are 108 rows whose unused effect and motion
slots went from `0` ("effect 0") to `-1` ("none"). That's data hygiene, not balance. Also: one
object row added, an NPC's type and team changed (6620), another's team changed (455000), and an
unnamed weapon row (38090000) given a different model.

The parameters are about a quarter of what the update replaced. Event scripts for ten maps, AI
Lua for seven, talk scripts for three, and the DLC areas' navmeshes and map layouts change too
([the package](the-package.md#the-update-is-file-replacement)). Those are the next data diffs to
make readable.

## The event scripts

The update also replaces the event scripts (EMEVD) of ten maps. Events are small programs the
maps run: wait for a condition (a flag, the player in a region, a boss dead), then act (open a
door, play a cutscene, set a flag, warp the player).

### Reading them: `tools/events.py`

As with the params, the game *describes* its own instruction set: `event/common.emedf.dcx` lists
25 instruction classes and 287 instructions with Japanese names and typed arguments. And each
map's `.emeld.dcx` names its events, again in Japanese:

```
Event 0  コンストラクタ  (constructor)
    [2000:00] イベント起動(イベントスロット番号=0, 起動イベントID=12100010, 引数=0)        start event 12100010
    [2003:23] 復活ポイント設定(復活ポイントエンティティID=2102961)                          set respawn point
    [2003:02] イベントフラグ設定(イベントフラグID=12101999, 設定フラグ=1)                   set event flag
```

`events.py check` decodes every instruction in every map and confirms its argument bytes match
the EMEDF's argument list, the way `params.py check` does for the tables. Two things came out of
it:

* **One instruction is variadic.** All 3,086 mismatches on the first run were `[2000:00]
  イベント起動` ("start an event"). After its three declared arguments it takes any number of
  extra 32-bit arguments and passes them to the event it starts. That's how one event definition
  serves many doors or enemies, and the started event reads them through its parameter table
  (shown as `X<byte>_<length>`).
* **Three instructions aren't in the EMEDF** (`2011:03`, `2004:55`, `2003:54`), used 62 times.
  They are shown as raw values.

After that, all 44,627 instructions in the base game decode with matching layouts.
`events.py diff` then compares two versions of a map event by event.

```
python -I tools/events.py diff work/base/dvdroot_ps4/event/common.emedf.dcx \
       work/base/dvdroot_ps4/event/m35_00_00_00.emevd.dcx work/update/dvdroot_ps4/event/m35_00_00_00.emevd.dcx \
       work/events/m35_00_00_00.md --eld work/update/dvdroot_ps4/event/m35_00_00_00.emeld.dcx
```

| Map | Events | Changed | Added / removed |
|---|---:|---:|---|
| common | 82 | 4 | |
| m21 Hunter's Dream | 105 | 5 | |
| m24_01 (Cathedral Ward area) | 250 | 3 | |
| m27 Forbidden Woods | 115 | 1 | 1 / 1 (a replacement) |
| m28 Yahar'gul | 101 | 2 | |
| m29 Chalice Dungeons | 428 | 1 | |
| m32 Byrgenwerth | 71 | 1 | |
| m34 Hunter's Nightmare | 118 | 1 | |
| m35 Research Hall / Astral Clocktower | 160 | 15 | |
| m36 Fishing Hamlet | 84 | 5 | |

In what follows, quoted Japanese is the developers' own event names; the English translations are
ours.

### Elevators remember where they are

The largest group of changes, in Cathedral Ward, Yahar'gul, the Astral Clocktower and the Fishing
Hamlet. Each elevator has a "goes up" and a "goes down" event (`エレベーター上がる_聖堂街B`,
"elevator goes up, Cathedral Ward B"). In 1.09 each one first checks its flags, and if the
elevator is already at the other end, restores it there without running the trip:

```diff
+    [3:00] イベントフラグ状態で判定(登録条件グループ=3, ..., 対象イベントフラグID=X0_4)      if flag A
+    [3:00] イベントフラグ状態で判定(登録条件グループ=3, ..., 対象イベントフラグID=X4_4)      and flag B
+    [1000:101] 条件グループ条件状態でラベルジャンプ(ジャンプ先ラベル=0, ...)                 else skip ahead
+    [2005:07] オブジェアニメ再現(対象オブジェクトのエンティティID=2411320, アニメID=3)       restore the object's animation state
+    [1000:103] 無条件でラベルジャンプ(ジャンプ先ラベル=1)
```

In the Clocktower the restore also disables the call levers (`ObjAct有効化 … 有効化設定=0`) and
switches collision parts (`ヒットパーツ有効化`). In the Fishing Hamlet it replaces a forced,
waited-for animation (`強制アニメ再生要求 … 終了待つか=1`). The likely fix: elevators no longer end
up in the wrong place after reloading the area or in multiplayer.

### Missing DLC handled safely

Common event 3503, `DLC削除_03` ("DLC removal 03"), is now started from the pre-constructor.
In 1.00 it cleared ten flags. In 1.09 it clears a list of DLC flags and then, if the player is in
area 34, 35 or 36 (the three DLC maps), warps them to the Hunter's Dream lamp:

```diff
+    [3:08] 現在マップIDで判定(登録条件グループ=-1, 同じか？=1, エリアNo=34, ブロックNo=0)       in m34?
+    [3:08] 現在マップIDで判定(登録条件グループ=-1, 同じか？=1, エリアNo=35, ブロックNo=0)       or m35?
+    [3:08] 現在マップIDで判定(登録条件グループ=-1, 同じか？=1, エリアNo=36, ブロックNo=0)       or m36?
+    [0:00] 条件グループ条件状態で判定(登録条件グループ=0, 条件成立条件状態=1, 対象条件グループ=-1)
+    [2003:49] 復活ポイントワープ(復活ポイントパラメータID=2102961)                             warp to respawn point
```

Respawn point 2102961 is the one the Hunter's Dream constructor sets. Given the label, this looks
like the game's handling of a save that is in the DLC when the DLC is no longer installed.

### NPCs

* `ヤマムラ無敵` ("Yamamura invincible"), now started by the Research Hall's constructor. In
  1.00 it switched a character's team. In 1.09 it makes Yamamura invincible, with his HP bar
  hidden, until flag 9471 is set.
* In the Forbidden Woods, event 12700907 is replaced by 12700910, `DLC_連盟：処刑人裏切り`
  ("League: the executioner's betrayal"). The new version also disables the character entirely
  (`キャラ無効化`), not just his AI, until his conditions are met, and only when the player is
  the host (`キャラタイプ=0`).
* Common event 9480, an NPC-kill event (`血族BJを付けてNPC殺害2_XX`), takes one more argument
  and is started for more NPCs (flags 1710–1713, 1730, 1790, 12700909). Flag 1790 is also one
  of the executioner event's conditions.
* `時計塔の女医_初期化` ("Clocktower woman doctor: initialise") starts her in a different idle
  animation (2250 → 7011).

### Bosses and enemies

* **The DLC's final boss** (`ラスボス`, the Orphan of Kos in the Fishing Hamlet) now also wakes
  when it takes damage (`属性ダメージ判定`), not only when the player comes within range. Its
  `黒ニョロ` ("black squiggle") spawns with gravity off.
* **Father Gascoigne** (on defeat), **Ludwig** (on waking) and the Orphan get a fixed update
  frequency (`更新頻度指定`: −1 → 5, 2 and 0).
* **Rom's small spiders** (`白痴の蜘蛛`, "the Vacuous Spider"), in Byrgenwerth and in the
  Chalice Dungeons, now spawn with gravity off (`重力無効化`, switched back on once they are
  placed), with some timing changes. In Byrgenwerth they also finish turning toward the player
  before acting (`キャラの旋回 … 終了待つか` 0 → 1).

### The Hunter's Dream

* `売買メッセンジャー＿ラインナップ拡充_時間帯フラグ` ("trading messenger: line-up expansion,
  time-of-day flags") now sets flags 5900–5904 when flags 6603 or 12601800 are set. The five new
  shop rows that sell Blood Rock ([above](#what-changed)) are in five line-ups, so this is
  plausibly what unlocks them; the shop rows themselves carry no flag.
* The gravestone messengers' animations (`墓石メッセンジャーアニメ可否判定`) now check the DLC
  flags on gravestone 4, not 3, and the Doll's position check follows.

## Files

| Path | |
|---|---|
| `work/ghidra/diff/pairs-1.00-to-1.09.tsv` | every non-identical pairing: bucket, old address, new address |
| `work/ghidra/diff/c-1.00/`, `c-1.09/` | one decompiled `.c` per changed function |
| `work/ghidra/diff/c-diff-1.00-to-1.09.md` | the normalised diffs, largest first |
