# Questions and answers

Short answers to what people ask about Bloodborne, each with the number and a link to where it is
worked out. Everything here comes from the game's own files and code, read on the other pages;
nothing is from play-testing.

**Jump to:** [your attacks](#your-attacks) · [defense, armor and health](#defense-armor-and-health) ·
[status effects](#status-effects) · [runes, gems and items](#runes-gems-and-items) ·
[enemies and bosses](#enemies-and-bosses) · [how enemies think](#how-enemies-think) ·
[characters and story](#characters-and-story) · [Chalice Dungeons](#chalice-dungeons) ·
[the game's files](#the-games-files)

## Your attacks

**How is attack rating worked out?**
For each damage type: the weapon's base × the move's motion value × a stat multiplier. A stat
that meets its requirement gives `1 + scaling/100 × curve(stat)/100`. Strength and Skill are
**added** for physical damage, and Arcane alone scales every element. Short of a requirement,
the multiplier falls to 0.198 at 20% or more short.
→ [Attack rating](the-combat.md#attack-rating)

**Does Bloodtinge raise my normal attacks?**
No. Bloodtinge isn't in the physical calculation at all. It has its own "blood" value, from a
separate mode of the same function.
→ [Attack rating](the-combat.md#attack-rating)

**Why does my Saw Cleaver do fire damage now?**
A fire, bolt or arcane "Add ATK" gem **converts** the whole physical base of an attribute weapon
into that element, scaled by Arcane instead of Strength and Skill. A +10 Saw Cleaver at Str/Skl
25 goes from 270 physical to 264.1 fire at Arcane 50. Only 13 plain weapons (and their Uncanny
and Lost forms) convert. With two converting gems, arcane beats fire and fire beats bolt.
→ [Elemental conversion](the-combat.md#elemental-conversion)

**Do two +10% gems give +20%?**
No, ×1.21: rates **multiply**, and flat bonuses add. That's read from the gatherer itself
(`SpEffect_AttackRates`).
→ [How an attacker's effects combine](the-combat.md#how-an-attackers-effects-combine)

**Are the gem numbers exact?**
The percentages mostly are: 6,036 of 6,279 agree with their text. The flat "Add … ATK" values are
whole numbers in the data: "Add blood ATK +0.5" stores 0. "Boosts rally potential" stores four
times what it says.
→ [Blood gems](the-combat.md#blood-gems)

**Which button plays which move, and what is a "charged R2"?**
The animations name every move. For many weapons R2 is a charge: released early it's one move,
fully charged another (the Saw Cleaver's 120% and 190%). The two forms of a trick weapon are
two sets of moves, 400 apart.
→ [Which button plays which move](the-combat.md#which-button-plays-which-move)

**Do visceral attacks have their own multiplier?**
Yes, the weapon's `throwAtkRate`: ×(100 + it)/100. The Clawmark rune raises a visceral's
**damage** rates by 10–30%, which apply after defense.
→ [Attack rating](the-combat.md#attack-rating) · [Runes](the-combat.md#runes)

## Defense, armor and health

**Do levels make me tankier?**
A little, then less and less. Defense is curve 102 of your six stats' sum, which is always your
level + 50: 10 at level 10, 71 at 30, 129 at 50, 180 at 70 and 214 at 100. Against a late boss
it hardly matters: Ludwig's entrance attack takes 896 at level 30 and 874 at level 100.
→ [Defense](the-combat.md#defense) · [What a hit costs a hunter](the-combat.md#what-a-hit-costs-a-hunter)

**What does armor do?**
It only cuts damage. Every piece's flat defense is 0, and the four pieces' cut rates
**multiply**: the full Hunter set leaves slash damage at ×0.755.
→ [What a hit costs a hunter](the-combat.md#what-a-hit-costs-a-hunter)

**What does a level cost?**
With x = level + 81: x² × (0.02 × max(0, x − 92) + 0.1) + 1, rounded. That's the Dark Souls
formula.
→ [Leveling up](the-combat.md#leveling-up)

**How does the rally work?**
A hit carries a regain amount: the weapon's `wepRegainHp` × the move's rally % / 100, plus a flat
part. The lost health waits as "grace HP" and drains away unless you hit back in time.
→ [How rally wins health back](the-combat.md#how-rally-wins-health-back)

**How much does a Blood Vial heal?**
40% of max HP. Iosefka's heals 70%, Adeline's 50%, Arianna's and Adella's 25%.
→ [Consumables](the-combat.md#consumables)

## Status effects

**What are slow poison, rapid poison and frenzy, in the data?**
Dark Souls' poison, bleeding and curse, renamed. The menu, the gem text and the stat curves all
agree.
→ [Old names, new conditions](the-combat.md#old-names-new-conditions)

**What does each one do?**
Slow poison takes 12 points + 0.1% of max HP a second for 40 seconds. Rapid poison is one burst:
250 points + 3%, or 30% of max HP, depending on the source. Frenzy takes **70%** of max HP at
once.
→ [How a condition builds up](the-combat.md#how-a-condition-builds-up)

**Does Insight make frenzy worse?**
Yes. Frenzy resistance from stats is curve 113 of Insight, falling from 100 to 60 between Insight
15 and 30, and to 10 at 99. The Hunter set adds 119 frenzy resistance.
→ [A hunter's resistance](the-combat.md#a-hunters-resistance)

**What raises poison resistance?**
Endurance (curves 110 and 112) plus your armor's values.
→ [A hunter's resistance](the-combat.md#a-hunters-resistance)

**Which bosses can be poisoned?**
Most bosses have 999 in every resistance. The exceptions include the Loran Darkbeast (slow
poison 70) and the Blood-starved Beast (rapid poison 180).
→ [Who inflicts what](the-combat.md#who-inflicts-what)

## Runes, gems and items

**What do the runes do?**
Runes are gems for the body. Clockwise Metamorphosis gives max HP ×1.05, 1.1 or 1.15; Great Lake
cuts all damage by 3–5%; Moon gives echoes ×1.1–1.3. All 83 rune effects are listed.
→ [Runes](the-combat.md#runes)

**Do Fire and Bolt Paper scale?**
They add a flat 80 fire or bolt attack for 60 seconds, flagged to scale with Arcane. Bone Marrow
Ash holds no number: the code does the boost.
→ [Consumables](the-combat.md#consumables)

**Where is a given item?**
Every item lot is joined to the map, enemy or event that gives it. For example, the Saw Spear is
on a corpse in Central Yharnam, and Simon's Bowblade drops wherever Simon dies.
→ [Where the items are](the-items.md)

**When do the shops' stock unlock?**
Each shop line has an unlock flag. The insight shop's line-up depends on flags you set by
playing.
→ [Shops](the-items.md#shops-and-a-wrong-guess-corrected)

## Enemies and bosses

**Which boss hits hardest?**
In play, the Celestial Emissary's magic (1,323 attack), then Ludwig's entrance attack (1,319) and
Laurence's stomp (1,258). The early bosses barely scale: the Cleric Beast's slam is 393.
→ [What a hit costs a hunter](the-combat.md#what-a-hit-costs-a-hunter)

**Why do later enemies hit so much harder?**
Each enemy carries an **area level** effect that multiplies its HP, attack and defense. The
Cleric Beast's level 2 is ×1.037 attack, Ludwig's level 17 ×2.488, the Orphan of Kos's level 22
×2.839.
→ [What a hit costs a hunter](the-combat.md#what-a-hit-costs-a-hunter)

**How much harder is New Game+?**
Each enemy has its own New Game+ effect, scaled by the cycle: ×1.0 on the second playthrough,
then 1.07, 1.1, 1.125, 1.15 and 1.2 from the seventh on. Early enemies gain the most: Father
Gascoigne's effect is HP ×6.08, attack ×2.328.
→ [New Game+](the-combat.md#new-game)

**How much health does a boss have?**
The enemy sheets give each enemy's HP with its area level applied. Gascoigne has 2,032, the
Cleric Beast 3,016, Ludwig 16,658.
→ [Enemy movesets](the-combat.md#enemy-movesets)

**What are a boss's attacks called?**
The developers named every attack. The Cleric Beast's are `右フック` (right hook), `踏みつけ`
(stomp) and `超叩きつけ` (super slam), and the sheets list them all with their damage, poise
damage and timing.
→ [The names come with the data](the-combat.md#the-names-come-with-the-data)

## How enemies think

**How does an enemy pick its next attack?**
Its battle script weighs each action by distance, health and state, then rolls. Each action's
chance is its weight over the total. Every enemy's sheet gives the chances by distance band.
→ [How an enemy chooses](the-combat.md#how-an-enemy-chooses)

**What does the Cleric Beast do up close, and at range?**
Up close it grabs and punches. At range it slams and stomps (38% stomp at 15 m or more). It
turns 70% of the time when you're behind it. Below 70% health it heats up: a forced combo, then
stronger moves.
→ [The Cleric Beast](the-combat.md#the-cleric-beast)

**What does Ludwig do below 75% health?**
His four heat-ups fire in order, at 90, 85, 80 and 75%, each when his AI says it's ready. Each
tips him toward one move. At the last, the vanishing leap goes from 9% to 50% of his choices.
→ [What the map scripts ask](the-combat.md#what-the-map-scripts-ask)

**When does Father Gascoigne transform?**
The map script requests it below 34% of his health, the first time through. While he carries
the music box's effect (4640) and has 34% or more, another event makes him play a special
animation instead.
→ [What the map scripts ask](the-combat.md#what-the-map-scripts-ask)

**Do bosses have a fixed opening move?**
Six do. Ludwig always opens with his jump slam, which unlocks everything else.
→ [Other bosses](the-combat.md#other-bosses)

## Characters and story

**Where can I send survivors, and what happens to them?**
Two refuges, Oedon Chapel and Iosefka's Clinic, for six people. Each has states for "on the way"
and "there", and the sheets follow every outcome.
→ [The safe places](the-questlines.md#the-safe-places)

**How does the game remember an NPC's story?**
With event flags: each NPC has a range of them, one state at a time. The flag index lists every
read and write of every flag.
→ [Event flags](the-flags.md) · [The questlines](the-questlines.md)

**What happens in Eileen's, Djura's or Alfred's story?**
Each has a sheet of states, conversations and gifts, read from the scripts.
→ [Four more stories](the-questlines.md#four-more-stories) · [The Old Hunters](the-questlines.md#the-old-hunters)

## Chalice Dungeons

**Are the Chalice Dungeons random?**
No. Each Root chalice has **100 or 200 pre-built layouts** on the disc, and the ritual picks one.
The pick is steered toward layouts holding unique items you haven't collected yet.
→ [Root chalices](the-chalice-dungeons.md#root-chalices-100-or-200-layouts-not-random-generation) · [How a ritual picks its layout](the-chalice-dungeons.md#how-a-ritual-picks-its-layout)

**Are there chalices deeper than the game offers?**
The tables define 17 rituals at depths 6 to 8 that the game doesn't contain: no item, no name,
no layout.
→ [Defined but not in the game](the-chalice-dungeons.md#defined-but-not-in-the-game)

**What do the rites (Rotted Offering and the rest) do?**
Every variant is already placed in the layout, and each rite's script **removes** the ones you
didn't choose.
→ [Rites are templates that remove things](the-chalice-events.md#rites-are-templates-that-remove-things)

## The game's files

**Does Bloodborne need a server?**
Not for single-player, Root dungeons included. Only sharing and online play do.
→ [The online boundary](the-online-boundary.md)

**What did the 1.09 patch change?**
Developer tooling switched off, a handful of balance changes, all in the data, and event, AI and
layout fixes. 99.6% of the code is unchanged.
→ [What the 1.09 patch changed](the-patch.md)

**What's left in the files from other games?**
Attacks for 116 enemies with no models, many of them Dark Souls' (Seath, Quelaag, the Gaping
Dragon), a second game's prototype, show builds, and Dark Souls words for Bloodborne's things.
The Hunter's Mark is the Darksign.
→ [Leftovers](the-leftovers.md)

**Can I get the textures and models out?**
Yes: textures as PNG, models as OBJ, and a renderer for every character and piece of equipment.
→ [Textures](the-textures.md) · [Models](the-models.md)

**How do I reproduce all this from my own copy?**
→ [Rebuilding `work/`](rebuilding-work.md)
