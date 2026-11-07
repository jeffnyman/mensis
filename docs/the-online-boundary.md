# The online boundary

Where Bloodborne stops doing things for itself and asks a server. This matters for anyone running
the game off a real PS4, because the console does almost everything locally and the server only
comes in at a few seams. Everything here is read from `eboot.bin`: its system imports, its
network class names, and an XML server-status document embedded in it.

## Three cases

Running the game, an emulator or a modified console faces the network in three different ways.

**1. Never needs a server.** The entire single-player game runs offline, which is how a large
share of players experienced it. The Chalice Dungeons make the split clear:

* The 100/200 pre-built layouts ship on the disc. The ritual's set-up step
  (`CSDungeonRitualHelper::STEP_Exec_forSetupHolygrail`) runs on the console and writes its
  result into the save's event flags ([the Chalice Dungeons](the-chalice-dungeons.md#glyphs)). So
  Root dungeons are created and played with no server.
* The step names that involve a server are all about *sharing*: `WaitUpload_forPresentOffering`,
  `Exec_forChangeShareLevel`, `Exec_forJoin`, `Exec_forRandomJoin`.

**2. Depends on what the server says.** Features where the console sends a request and acts on the
reply. Glyph search is the plainest: the entry screen sends eight characters, and the server
answers with a dungeon or with "No dungeons found using the given Chalice Glyph." The same holds
for blood messages, bloodstains, wandering ghosts, co-op and invasions, and the Chalice
leaderboards. Some *tuning* also arrives this way: the SOS-sign request limit the 1.09 patch
changed ([the patch](the-patch.md#2-sos-sign-requests-a-limit-read-live)) comes from network
configuration the server supplies (below).

**3. Two different servers.** The imports show the game talks to two back ends:

| Server | Reached through | Carries |
|---|---|---|
| **Sony PSN** | `libSceNp*` (`NpManager`, `NpAuth`, `NpMatching2`, `NpSignaling`, `NpScore`, `NpWebApi`, `NpCommerce`, `NpTrophy`) | sign-in, matchmaking rooms, peer-to-peer connection setup, leaderboards, trophies, the store |
| **FromSoftware's game service** | `libSceHttp` / `libSceSsl` over `libSceNet`, behind the `FrpgNetMan` and `SprjNetworkClientMan` classes | the social world: messages, ghosts, summons, Chalice sharing |

PSN handles *who you are and who you can reach*; FromSoftware's service handles *the game's shared
content*. Co-op and invasions use both: `NpMatching2` makes the room and `NpSignaling` opens the
peer-to-peer link, but which summon signs exist comes from FromSoftware's `SummonData` calls.

## The game service's request vocabulary

The executable embeds a server-status document, a block of ~130 XML tag names. It is the
configuration the console fetches from the URL in the `Game.Network.SsInfoUrl` property
(`api_ServerTimeGet` and the rest are its `<gameurl>` entries). The tags name every request the
game makes, grouped here by feature:

| Feature | `api_*` calls | Through |
|---|---|---|
| Session & system | `Login`, `ServerTimeGet`, `SyncCharaId`, `NoticeNormalGet`, `NoticeEmergencyGet`, `UserAgreementGet`, `MultiPlayNetError`, `UserPropertiesMoveCount(Check)` | FromSoft service |
| Blood messages (the orange notes) | `BloodMessCreate`, `BloodMessGetList`, `BloodMessEvaluate`, `BloodMessGetEvaluate`, `BloodMessRemove`, `BloodMessSearchAdd` | FromSoft service |
| Bloodstains & death visions | `TombMessCreate`, `TombMessGetList`, `TombMessRemove`, `DeathVisionGet` | FromSoft service |
| Chair / respawn notices | `ChairMessGetList`, `ChairMessRespawnPointNotice` | FromSoft service |
| Wandering ghosts | `WanderingGhostCreate`, `WanderingGhostGet` | FromSoft service |
| Co-op & invasion summons | `SummonDataCreate`, `SummonDataGetList`, `SummonDataRemove`, `SummonDataSummon` | FromSoft service + PSN signaling |
| Chalice Dungeon sharing | `ChannelUpload`, `ChannelShare`, `ChannelSearch`, `ChannelWordSearch`, `ChannelGetDetailsInfo`, `ChannelGetInfo`, `ChannelRandomJoin`, `ChannelAddMaterial(CompleteNotify)` | FromSoft service |
| Messengers | `MessengerShellUpload` | FromSoft service |

`ChannelWordSearch` is the glyph lookup from
[the Chalice Dungeons](the-chalice-dungeons.md#glyphs): the console hands the server eight
characters and the server names a dungeon.

The same document carries **tuning** the server controls: matchmaking level ranges
(`SummonDataCoopMatchingLevelLowerAbs`/`…Rel`/`Upper…`), summon guard timers and wait counts, the
"blood-mad hunter" and "natural enemy" percentages, and the polling interval for each feature
(`BloodMessGetListInterval`, `WanderingGhostGetInterval`, …). So the server can retune co-op and
invasion balance without a patch, which is what the 1.09 code change for the SOS-sign limit was
about: the game reads the limit live rather than caching it.

## Leaderboards and telemetry

Two more server-bound systems:

* **Leaderboards** go through PSN's `sceNpScore`. The executable names 144 score boards of the
  form `<access>_<verb>_RANKING_VID_<board>` (for example `WD_RANKING_VID_ONE_A`,
  `RL_RANKING_VID_BR_C`), plus a parallel `DBG_` set for testing. These are the Chalice Dungeon
  rankings.
* **Telemetry.** A large `Playlog_*` tag set (chr attacks, item pickups, deaths, map changes, …)
  is uploaded to Amazon S3. The upload function builds an `s3.amazonaws.com` URL for a bucket
  named `bb-playlog-prod` (or `bb-playlog-test`), and signs the request with credentials compiled
  into the binary. This is one-way analytics, not gameplay; it has no effect on what the player
  sees and nothing reads it back.

## What an emulator author faces

Given the boundary, there are two ways to handle the online features.

**Present "offline".** If PSN sign-in doesn't succeed, the game takes the paths a disconnected PS4
would: single-player works, and the server-dependent features simply report nothing available
("No dungeons found", no summon signs, no messages). Offline is a mode the game was built for, so
this is the least work. (How any particular emulator does this hasn't been examined here.)

**Reimplement the server.** The community has done this for other FromSoftware games. It means
working out the client's side of the protocol from the executable (the request code around
`FrpgNetMan` and `SprjNetworkClientMan`, and the `api_*` vocabulary above) and writing a server
that answers it. A reimplemented server needs none of FromSoftware's original data:

* For glyphs it only has to remember "this glyph means this dungeon" for dungeons created on *that*
  server. A dungeon record is close to what the console already saves per slot: a 32-bit value, a
  16-byte field (eight characters, a glyph's size) and a 48-byte string. Glyphs only have to be
  consistent within that server.
* For blood messages, bloodstains, ghosts and summons, the server is a store-and-forward of what
  players upload, with the polling intervals and matchmaking ranges it chooses to hand back.

Either way, the client half of all of this is in the executable, which is the half we have. What
the official servers actually did with a request is not, and for a replacement server it does not
need to be.

## What this doesn't cover

* The wire format of each `api_*` request and reply. Reading it means decompiling the request
  builders behind `FrpgNetMan`; the names and the S3/HTTP machinery are mapped, the field layouts
  are not.
* The PSN `NpMatching2` room and signaling flow, which is Sony middleware rather than game code.
* Anti-tamper or session validation (`Game.Network.IsCheckServerVeryfy` suggests a server-side
  check exists); what it verifies hasn't been traced.
