# The online boundary

Where Bloodborne stops doing things for itself and asks a server. This matters for anyone running
the game off a real PS4, because the console does almost everything locally and the server only
comes in at a few seams. Everything here is read from `eboot.bin`: its system imports, its
network classes and strings, and the functions that find the server and build the requests.

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
| **The game service** | `libSceHttp` / `libSceSsl` over `libSceNet`, behind the `FrpgNetMan` and `SprjNetworkClientMan` classes | the social world: messages, ghosts, summons, Chalice sharing |

PSN handles *who you are and who you can reach*. The game service handles *the game's shared
content*. Co-op and invasions use both: `NpMatching2` makes the room and `NpSignaling` opens the
peer-to-peer link, but which summon signs exist comes from the game service's `SummonData` calls.

## Online and offline at run time

The switch between the two is a small state machine, `CSNetworkFlowStep`, with four steps:
`STEP_Init`, `STEP_OfflineMode`, `STEP_OnlineMode` and `STEP_Finish`.

* **Offline is where the game waits.** `STEP_OfflineMode` checks, every update, that `FrpgNetMan`
  exists, is up, and has none of its error flags set. Until then it does nothing, and the game
  plays offline. Once the network is healthy it sets the global "online" flag and moves on.
* **Online watches for losing it.** `STEP_OnlineMode` checks the same flags and, when something
  fails, raises a named event for the game's scripts to handle: `OnLanCutError` (the connection
  dropped), `OnNpServerSignOut` (signed out of PSN) or `OnFailedGetBlockNum`.

So offline isn't a separate mode chosen at boot. It is the state the game starts in and stays in
until the network is up, and nothing in it depends on a server answering.

## Finding the server

**The server-status document.** The game service's first contact is `ss.info`, an XML document
fetched over HTTPS. The address comes from a table in the executable, chosen by the console's
region:

| Region | Address |
|---|---|
| Japan | `https://ss4.scej-network.jp:20443/bb-jp/ss.info` |
| Americas | `https://ss4.scej-network.jp:20443/bb-us/ss.info` |
| Europe | `https://ss4.scej-network.jp:20443/bb-eu/ss.info` |
| Asia | `https://ss4.scej-network.jp:20443/bb-asia/ss.info` |

The table has six region slots: a fifth repeats Europe and the sixth is empty. The host is in an
SCE Japan domain (`scej-network.jp`), not a FromSoftware one. The document is read again every
`ReloadServerStatusInfoInterval` (one of its own fields).

**Switches for developers.** The engine reads these settings from its system properties. Their
defaults are compiled in. As with [the patch's developer hooks](the-patch.md#1-developer-remote-tooling-switched-off),
where a retail console's property values come from hasn't been examined, so the defaults are what
a retail game is expected to use:

| Property | Default | Effect |
|---|---|---|
| `Game.Network.UseServer` | on | stored by the network manager at start-up |
| `Game.Network.SsInfoUrl` | empty | if set, replaces the regional `ss.info` address |
| `Game.Network.UseLocalSsInfo` | off | read `testdata:/server/ss.info.xml` instead of fetching |
| `Game.IsBetatestMode`, `Game.IsTgsMode`, `Game.IsGamescomMode` | off | any one forces the local `ss.info` |
| `Game.Network.IsCheckServerVeryfy` | on | verify the server's HTTPS certificate (below) |
| `Game.IsOfflinemasterMode` | off | not traced |

The three event modes (a beta test, and what are evidently the Tokyo Game Show and Gamescom
demos) ran against a server description stored with the build, not the live one. The retail
package doesn't contain `testdata:/server/ss.info.xml`.

**The HTTP client.** There are only two HTTP clients in the executable: the game service's and the
telemetry uploader's. The game service's is a `sceHttpCreateTemplate` template, non-blocking.
Its user agent names the exact edition, from a table of region × edition:

```
PS4Application FROMhttp/1.0 (E-EP9000-CUSA03173_00-BLOODBORNE0000EU/2014101500)
```

The table gives the title IDs of all eight retail versions, two per region: the original release
and, judging by this package's CUSA03173 for Europe, the Game of the Year Edition.

| Region | Titles |
|---|---|
| Japan | CUSA00299, CUSA03014 |
| Americas | CUSA00900, CUSA03179 |
| Europe | CUSA00207, CUSA03173 |
| Asia | CUSA01363, CUSA03023 |

**Certificate verification.** `Game.Network.IsCheckServerVeryfy` (their spelling) is passed
straight to `sceHttpsEnableOption(template, 1)` when on and `sceHttpsDisableOption` when off.
Option 1 is libSceHttp's server-certificate check. *The first version of this page guessed the
property was anti-tamper or a server-side session check. It is neither: it is the ordinary
HTTPS check that the server is who it claims to be, and it is on by default.*

## The requests

`ss.info`'s parser looks for about 130 XML tags, and their names are in the executable. Among
them is one `api_*` tag per request, giving that request's address. The executable also has a
name table with one entry per request, in the same order, from `LoginRequest` (0) to
`WanderingGhostGetRequest` (36): 37 requests.

### The envelope

Every request is built by one function (`GameService_BuildRequest` in the hand names) and sent by
another. The body is **JSON**: the serialiser behind it writes `{`, `}`, `[` and `:` around keys
and values. The builder adds:

* `MessageId`: the request's name from the table, e.g. `"ChannelWordSearchRequest"`;
* `SessionId` and `UserId`, and `?user_id=<id>` on the URL, to every request except `Login` (0)
  and `UserAgreementGet` (5), the two made before there is a session.

The JSON text goes to the HTTP request as is. No further encryption step appears on this path,
and the transport is HTTPS.

**Login** sends `ApplicationVersion`, `AuthorizationCode`, `IssuerId`, `LanguageId`, `NatType`,
`PlatformAccountId` and `RegionId`. The authorization code and issuer are what
`sceNpAuthGetAuthorizationCode` (imported) produces: PSN's proof to the game service of who the
player is. The reply that turns this into a `SessionId` hasn't been traced.

### Each request's fields

Each request has its own builder, which calls the sender with its request number. The keys each
builder writes, beyond the envelope:

| Request | Fields |
|---|---|
| `SyncCharaId` | `CharaIdNum` |
| `NoticeNormalGet` | `Language`, `Region` |
| `NoticeEmergencyGet` | `Language`, `Region`, `CheckTime` (formatted `%d-%02d-%02dT%02d:%02d:%02d`) |
| `UserPropertiesMoveCount`, `…Check` | `Count` |
| `MultiPlayNetError` | `HostUserId`, `TargetUserId` |
| `BloodMessCreate` | `BloodDataVersion`, `CharaDataVersion`, `EvaluatePlus`, `EvaluateMinus` |
| `BloodMessGetList` | `AreaId`, `AreaRegionId`, `ChannelId`, `AreaInfoList`, `GetCount`, `GetMaxCount`, `BloodDataVersion`, `CharaDataVersion`, `MessShellInfoVersion` |
| `BloodMessEvaluate` | `BloodMessId`, `EvaluateKind` |
| `BloodMessGetEvaluate` | `BloodMessId`, `BloodMessIdList` |
| `BloodMessRemove` | `BloodMessId` |
| `BloodMessSearchAdd` | `BloodMessId`, `BloodMessIdList`, `CharaId` |
| `TombMessCreate` | `AreaId`, `AreaRegionId`, `ChannelId`, `CharaId`, `TombData`, `TombDataVersion`, `DeathVisionData`, `DeathVisionDataVersion` |
| `TombMessGetList` | `AreaId`, `AreaRegionId`, `ChannelId`, `AreaInfoList`, `GetCount`, `GetMaxCount`, `TombDataVersion`, `MessShellInfoVersion` |
| `TombMessRemove` | `AreaId`, `AreaRegionId`, `ChannelId`, `TombMessId` |
| `DeathVisionGet` | `TombMessId`, `DeathVisionDataVersion` |
| `ChairMessGetList` | `ChannelId`, `CharaId`, `WarpInfoId`, `WarpInfoList` |
| `ChairMessRespawnPointNotice` | `ChannelId`, `CharaId`, `WarpInfoId` |
| `WanderingGhostCreate` | `AreaId`, `AreaRegionId`, `ChannelId`, `CharaId`, `MatchingLevel`, `RejectIgnore`, `WanderingGhostData`, `WanderingGhostDataVersion` |
| `WanderingGhostGet` | `AreaId`, `AreaRegionId`, `AreaList`, `ChannelId`, `CharaId`, `GetCount`, `GetMaxCount`, `JoinedCharaIdList`, `MatchingLevel`, `WanderingGhostDataVersion` |
| `SummonDataCreate` | `AreaId`, `AreaRegionId`, `ChannelId`, `CharaId`, `PosX`/`PosY`/`PosZ`, `MatchingLevel`, `NatType`, `Region`, `RegionFlag`, `SummonType`, `SummonMethod`, `SummonWord`, `ClientSummonTypeList`, `ClientVersion`, `SummonData`, `SummonDataVersion`, and four flags `NaturalEnemy_BloodHunter`, `…_HunterOfHunter`, `…_Vileblood`, `…_VilebloodHunter` |
| `SummonDataGetList` | the location and matching fields above, plus `CoopOrNaturalEnemyRecruitNum`, `DistanceThreshold`, `GetLimitCount`, `GetMaxCount`, `IsInvationMultiPlayRequesting`, `UnlockFlagList` |
| `SummonDataSummon` | `CharaId`, `TargetCharaId`, `TargetUserId` |
| `SummonDataRemove` | `CharaId` |
| `ChannelUpload` | `FormData`, `SpendMaterialCount`, `WishMaterialCount`, `WishMaterialMainTypeId`, `WishMaterialSubTypeId` |
| `ChannelShare` | `ChannelId`, `CharaId`, `ShareLevel` |
| `ChannelSearch` | `HolyGrailTypeId`, `RitualLevel`, `SubFeatureFlag`, `Status`, `FixedOrGeneral`, `FixedOrGeneralList`, `GetCount`, `FormDataVersion` |
| `ChannelWordSearch` | `SearchWord`, `FormDataVersion` |
| `ChannelRandomJoin` | `HolyGrailTypeId`, `RitualLevel`, `CharaId`, `RandomJoinTargetList`, `FormDataVersion` |
| `ChannelGetInfo`, `ChannelGetDetailsInfo` | `ChannelId`, `ChannelIdList` |
| `ChannelAddMaterial` | `ChannelId`, `MaterialCount`, `MaterialList`, `MaterialMainTypeId`, `MaterialSubTypeId` |
| `ChannelAddMaterialCompleteNotify` | (envelope only) |
| `MessengerShellUpload` | `CharaId`, `ShellData`, `ShellDataVersion` |

`ServerTimeGet` and `UserAgreementGet` have no builder that calls the shared sender, so they are
made some other way or not at all.

What the table shows:

* **A "channel" is a Chalice Dungeon.** The Chalice requests use `HolyGrailTypeId` (the chalice),
  `RitualLevel` and `SubFeatureFlag` (the rites), the same terms as the ritual tables in
  [the Chalice Dungeons](the-chalice-dungeons.md). `ChannelUpload` sends the dungeon as `FormData`
  with the materials spent. Messages, bloodstains and ghosts *inside* a shared dungeon carry its
  `ChannelId`, so they stay in that dungeon.
* **A glyph search is just the word.** `ChannelWordSearch` sends `SearchWord`, the eight
  characters, and the data version. The server does the lookup.
* **The payloads are opaque blobs with versions.** `TombData`, `DeathVisionData`, `SummonData`,
  `WanderingGhostData`, `ShellData` and `FormData` each travel with a `…Version`. The server
  stores and returns them without needing to understand them. A wandering ghost is a recording
  the server hands to other players.
* **Matchmaking happens on the game service.** `SummonDataGetList` sends the player's position,
  `MatchingLevel`, region and NAT type. The server picks the signs, using the level ranges it
  published in `ss.info` (below), and PSN only connects the players afterwards.

### Tuning in `ss.info`

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
  sees and nothing reads it back. It uses the second of the two HTTP clients.

## What an emulator author faces

Given the boundary, there are two ways to handle the online features.

**Present "offline".** If PSN sign-in doesn't succeed, `CSNetworkFlowStep` stays in its offline
step, and the game takes the paths a disconnected PS4 would: single-player works, and the
server-dependent features simply report nothing available ("No dungeons found", no summon signs,
no messages). Offline is a mode the game was built for, so this is the least work. (How any
particular emulator does this hasn't been examined here.)

**Reimplement the server.** The community has done this for other FromSoftware games. The client
half is now mapped: where the console looks (`ss.info` at a fixed regional address), what it
sends (the 37 JSON requests above), and what it expects to be told (the `ss.info` tags). Three
things stand between that and a working server:

* **Being found.** The address is compiled in. A replacement has to be reachable under that name,
  or the game has to be pointed elsewhere.
* **Being trusted.** Certificate verification is on, so the console only talks to a server whose
  certificate it accepts.
* **Identity.** Login proves identity with a PSN authorization code. A replacement server can't
  check that with Sony, so it needs its own idea of who a player is.

Past that, a replacement needs none of FromSoftware's original data:

* For glyphs it only has to remember "this glyph means this dungeon" for dungeons uploaded to
  *that* server. A dungeon record is the `FormData` blob the console uploads, the same record it
  saves per slot: the chalice, a packed map ID (which fixes the layout), the sub-feature picks, a
  16-byte glyph field and the name ([how a ritual picks its layout](the-chalice-dungeons.md#how-a-ritual-picks-its-layout)).
  Glyphs only have to be consistent within that server.
* For blood messages, bloodstains, ghosts and summons, the server is a store-and-forward of the
  versioned blobs players upload, filtered by `AreaId`/`ChannelId` and matching level, with the
  polling intervals and ranges it chooses to publish in `ss.info`.

Either way, the client half of all of this is in the executable, which is the half we have. What
the official servers actually did with a request is not, and for a replacement server it does not
need to be.

## What this doesn't cover

* **Replies.** The requests are mapped to field names. What the server sends back, and how each
  reply is parsed, is not.
* **Value types and the blobs.** The table gives keys, not each value's type, and the contents of
  the versioned `…Data` blobs (how a bloodstain or a ghost recording is laid out) haven't been
  read.
* **`ServerTimeGet` and `UserAgreementGet`**, which aren't built by the shared sender.
* **The PSN `NpMatching2` room and signaling flow**, which is Sony middleware rather than game code.

## How this was found

| Step | Tool |
|---|---|
| Strings for the network switches, `ss.info`, the user agents and the request names | `FindStringUses.java` |
| The functions reading them (`CSNetworkFlowStep`, the HTTP client, the `ss.info` fetch) | `Decompile.java` |
| The two HTTP clients and every caller of the request sender | `RefsTo.java`, with decompilation |
| The address tables (`ss.info` URLs, user agents, request names) | read from the prepared ELF |

The request builders, the sender and the HTTP-client functions are named in
`tools/names/eboot-1.09.hand.tsv` (`GameService_*`, `HttpClient_*`, `ServerStatus_*`), each with
its evidence. The telemetry uploader was left alone beyond what the section above says.
