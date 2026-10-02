# Native dependencies for town defense

`m7-town-core.patch` is mandatory before enabling town defense. It narrowly excludes only a temporary creature with
both `IsLootRewardDisabled()` and the exact `coa.activity.rewardless_wave` marker from `Player::isHonorOrXPTarget`.
Drain Soul periodic shard creation and Shadowburn death-item refunds otherwise treat these synthetic waves as XP
targets even though native loot and kill XP are disabled. All unmarked creatures and other summons retain the existing
level, gray-target, and reward-target behavior. The module's installed `TownDefenseWave::CanPrepare` additionally rejects
death-item auras on an explicitly targeted owned marked wave, including generic inventory-on-death spells.

`m7-town-challenges.patch` is mandatory when the separately installed `mod-coa-challenges` dependency is present.
The playerbots module marks
only its owned temporary town-defense waves with `coa.activity.rewardless_wave` in the existing native `CustomData` map.
The patch suppresses challenge kill credit and forbidden-kill failure only when that marker, `IsSummon()`, and
`IsLootRewardDisabled()` are all present. Ordinary kills and other reward-disabled summons retain their existing behavior.

The native loot/XP guard is separate: `TownDefense.cpp` sets the existing runtime reward flag before combat, checks all
generated corpse loot fields, and removes each owned wave. The challenge patch is needed because the core dispatches
player and pet kill callbacks outside its native reward gate. It introduces no core API or database schema changes.
The playerbots native GuildTaskMgr kill hook also skips these marked owned waves. Ordinary combat spell procs and other
ability bonuses remain native; these guards do not suppress all caster kill procs or erase resources spent in combat.
The town anchor and wave templates must be scriptless so summoner callbacks cannot process an unmarked wave before
`Map::SummonCreature` returns and the caller installs its marker.

From the isolated core checkout, review and apply the core patch before enabling town defense:

```sh
patch --dry-run -p1 < modules/mod-playerbots/data/patches/m7-town-core.patch
patch -p1 < modules/mod-playerbots/data/patches/m7-town-core.patch
```

If `mod-coa-challenges` is installed, also apply its dependency patch:

```sh
patch --dry-run -p1 < modules/mod-playerbots/data/patches/m7-town-challenges.patch
patch -p1 < modules/mod-playerbots/data/patches/m7-town-challenges.patch
```

Rebuild the native server after applying it. To detect an already-applied patch, use the same dry run with `-R`; do not
apply it repeatedly or to a primary running realm without the normal reviewed deployment process. Keep town ambient
verification disabled until the isolated twenty-wave cleanup and reward observations pass.
