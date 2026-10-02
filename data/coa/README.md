# Optional CoA bot conversation, memory and activities

This increment adds localized conversation, asynchronous typed decisions, persistent relationships and organized
activities to the CoA module. It requires the compatible CoA core; generic upstream AzerothCore is not a tested target.
The separate M2b local model service, model training and model downloads are excluded.

## Configuration and administration

The four behavioral master switches default to zero:

- `AiPlayerbot.CoaVoice.Enable`: English/French template replies and responder coordination.
- `AiPlayerbot.CoaBrain.Enable`: typed Jev decisions and optional separately configured generated text/RPG advice.
- `AiPlayerbot.CoaMemory.Enable`: bounded persistent relationships and event history.
- `AiPlayerbot.CoaActivity.Enable`: the shared director, recruitment and activity lifecycle.

The unconditional M0 changes protect shared chat state and bound the loopback command server. Enabling a master
switch does not enable every subfeature. Read the associated `playerbots.conf.dist` comments, configure native targets
explicitly, and keep encounter/ambient verification gates closed until the required evidence is available.

The `.playerbots voice`, `brain`, `memory` and `activity` command families expose status, diagnostics and administration.
Player `.playerbots optout` / `optin` preferences are persisted and rechecked before dispatch and emission. The
activity command family includes joining, escorts, services, consented gold rolls, bounties and raid soft reserves.
Typed-decision setup, transports, privacy and fixture formats are documented in [brain/README.md](brain/README.md).
Keys may be supplied through the existing configuration environment overrides; do not commit credentials.

## Activity families

The director coordinates duels, gang parties, dungeons, elite hunts, outpost raids, inn/city gatherings, Gurubashi
visits, escorts, rare scouts/camps, faction-leader encounters, frontline/hub/town defense, PUG raids, world bosses,
learned crafting services, seasonal visits, gold rolls, PvP gangs, bounties and gated capital/lieutenant encounters.

New activity kinds default off, their automatic weights are zero and target lists are empty. Operations validate native
group ownership, GUID/epoch, level/faction/phase, deadlines and capacity. PvP groups freeze one mutually exclusive
ruleset per group: the default mix is 50% High-Risk and 50% War Mode, configurable through
`AiPlayerbot.CoaActivity.PvpGang.HighRiskPercent`. Prior native auras/flags are captured and restoration requires actual
readback. Gold rolls use explicit proposal/acceptance and native money-only trade; they do not provide crash-safe escrow.

Town defense **requires** the packaged native core patch, and the dependency patch when `mod-coa-challenges` is
installed. Apply and rebuild as described in [../patches/README.md](../patches/README.md). These narrow owned-wave
guards prevent native inventory/challenge rewards that otherwise bypass ordinary loot suppression; they are not
applied automatically by the module.

## Verification and remaining gates

The implementation has macOS Release/unit, scoped style/SQL-idempotence and selected offline UBSan coverage.
Isolated native observations include a Hogger hunt, scout lifecycles, twenty rewardless town-defense waves and a
sixty-second War Mode patrol with current membership and ruleset restoration. Those observations do not establish
complete client, encounter or population acceptance.

The faction-leader spike remains a no-go: there was no qualifying kill or stable walking withdrawal, and three delayed
speed readbacks changed under native aura projection after immediate rollback. `Leader.SpikeVerified` remains zero;
capital/lieutenant gates must stay closed. High-Risk runs correctly aborted on leader death and restored rulesets,
but a completed High-Risk patrol remains unverified. Do not set `PvpGang.GmVerified` from those aborts.

Real-client chat/escort/crafting/trade/soft-reserve/festival/gold flows, complete raid/boss/rare/bounty/PvP encounters,
week-long exact-level faction census and large-population performance remain outstanding. Windows/Linux builds,
live generated-text provider acceptance and a successful ASan run are also outstanding. Optional features retain
their default-off configuration while this evidence is collected.
