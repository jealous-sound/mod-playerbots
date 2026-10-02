# CoA brain fixtures

`fixtures/` holds request and response pairs of the decision wire: the typed questions the CoA brain asks
Jev (TypeSafe's decision model), through OpenRouter's Decisions API or TypeSafe directly, and the answers it gets
back. They are the source of truth for two things:

- **`.playerbots brain test selftest`** (`src/Coa/Brain/Net/BrainSelfTest.cpp`). It is an offline check of the wire
  code. It needs no network, no key and no running brain.
- **The mock engine** (`AiPlayerbot.CoaBrain.Decision.Chain = mock`). It replays these answers without opening a
  socket.

Each file's `"source"` says where it comes from:

- `"documentation"`: built by hand from OpenRouter's documented Decisions API and from the shape reported for
  TypeSafe's direct API (2026-09-30). Every fixture here is of this kind except the two `openrouter-recorded-*`
  ones. The TypeSafe shapes are still unverified.
- `"recorded"`: written from a live call (`.playerbots brain probe <engine> record` does it), then reviewed by hand.
  See [Recorded on 2026-09-30](#recorded-on-2026-09-30).

These files are not installed with the server. To use them, point `AiPlayerbot.CoaBrain.FixtureDir` at this
directory in the module's source, with an absolute path.

## Files

| File | Status | What it covers |
|---|---|---|
| `openrouter-voice-v1.json` | 200 | The reference call: the director's whisper job (reply, line, tone, emote), section 5 of the M2 contract. Cost reported: 842 tokens, $0.0000354 -> 36 micro-USD. |
| `openrouter-voice-who.json` | 200 | Party chat with two bots, so `who` is asked (b0, b1) and answered `b1`. The line has 8 options plus `silent`. |
| `openrouter-noul.json` | 200 | A yes/no (`noul`) question alone: a channel line nobody should answer (p = 0.08). |
| `openrouter-choice.json` | 200 | A choice question alone: 3 lines plus `silent`. The accented text is written as `ç`. |
| `openrouter-score.json` | 200 | A score question alone. The level comes from the probabilities (argmax 0), not from rounding the score (0.7). |
| `openrouter-recorded-noul.json` | 200 | Recorded: a yes/no question alone, with a short hand-written state. p = 0.76; 329 input and 20 output tokens, cost `1.3818e-05` -> 14 micro-USD. |
| `openrouter-recorded-voice-v1.json` | 200 | Recorded: the director's whisper job (reply, line, tone, emote). Emote's probabilities sum to 0.99 and are renormalized (none 0.4 / 0.99 = 0.40404). 1104 input and 202 output tokens, cost `4.6368e-05` -> 47 micro-USD. |
| `typesafe-voice-v1.json` | 200 | The reference call in the TypeSafe dialect: model `jev-latest`, and no `id`, `provider` or `usage.cost`, so `usd_micros` is 0 (the self-test does not price tokens). |
| `openrouter-error-402.json` | 402 | Out of credit -> `exhausted`. |
| `openrouter-error-429.json` | 429 | Rate limited -> `exhausted` (the engine rests until the next UTC day, as decided). |
| `openrouter-error-400.json` | 400 | Refused request -> `bad-request`. |
| `openrouter-error-401.json` | 401 | Bad key -> `auth-failed`. |
| `openrouter-error-503.json` | 503 | Provider down -> `server-error` (counts toward the breaker). |
| `openrouter-error-spend-limit.json` | 400 | The marker rule: a "spend limit" text means `exhausted`, whatever the status. |
| `openrouter-error-in-200.json` | 200 | A 200 whose body is an error object instead of answers -> `server-error`. |
| `typesafe-error-422.json` | 422 | A FastAPI-style `{"detail": [{"msg": ...}]}` validation error -> `bad-request`, and the detail comes from `msg`. |
| `invalid-answers.json` | 200 | A parser test: line's choice is not an option, tone's score is 7 on a 5-level scale, reply's `noul` is 1.5, and emote is missing. No answer is valid -> `bad-response`. |
| `mixed-validity.json` | 200 | A parser test: reply is valid and line is not, so the response still stands. Emote's probabilities hold a key that is not an option: it is dropped and the rest renormalized (wave 0.4 / 0.8 = 0.5). An answer to a question never asked is ignored. |

Three of them are also compiled into `BrainSelfTest.cpp`, byte for byte: `openrouter-voice-v1.json`,
`openrouter-error-402.json` and `invalid-answers.json`. That way `test selftest` still checks something when
`FixtureDir` is empty. If you change one of these three files, update its copy there too. A self-test run over
this directory prints a `note:` line when a copy no longer matches its file.

## Recorded on 2026-09-30

Live calls to OpenRouter's Decisions API confirmed the URL, the type names and the field names of the
documentation fixtures, with these details:

- The URL is `https://openrouter.ai/api/alpha/decisions`, the default of `Engine.openrouter.Url`.
- The alias `~typesafe/jev-latest` resolves to `typesafe/jev-1.13-20260917`, which the response gives as `model`.
- A call typically takes 0.4 to 1.1 s.
- Only input tokens are billed: about 14 micro-USD for a yes/no question alone, about 47 for a full voice decision.
  `output_tokens` is not zero (20 and 202 here) but adds nothing to `usage.cost`.
- `usage.cost` is written either plain or in exponent form (`4.6368e-05`).
- Probabilities may sum to a little less than 1 (0.99 for emote in the voice recording); the parser renormalizes them.
- Choice and score answers also carry a `confidence`, and score answers a `legend` of the levels.
- A model that does not exist gets HTTP 400 `{"error":{"message":"Model <name> does not exist","code":400}}`
  (`bad-request`), and an invalid key HTTP 401 `{"error":{"message":"User not found.","code":401}}`
  (`auth-failed`).

## File format

```json
{
  "fixture": "openrouter-voice-v1",
  "source": "documentation",
  "note": "Where it comes from and what it shows.",
  "dialect": "openrouter",
  "url": "https://openrouter.ai/api/alpha/decisions",
  "status": 200,
  "request": {"model": "...", "state": "...", "questions": {"reply": {"type": "noul", "instructions": "...", "criteria": {"true": "...", "false": "..."}}}},
  "response": {"id": "...", "model": "...", "answers": {"reply": {"type": "noul", "noul": 0.87}}, "usage": {"input_tokens": 842, "output_tokens": 0, "cost": 0.0000354}},
  "expect": {
    "error": "none",
    "model": "typesafe/jev-1.13",
    "answers": {"reply": {"valid": true, "calibrated": true, "p": 0.87}},
    "usage": {"input_tokens": 842, "output_tokens": 0, "usd_micros": 36}
  }
}
```

The top-level members:

- **`fixture`**: the name printed by the self-test. If it is missing, the file name without `.json` is used.
- **`dialect`**: a compiled-in dialect of `SystemOneDialects.cpp` (`openrouter`, `typesafe`, `jevlocal`, `mock`).
  Its field names are used to read the file; a `CoaBrain.DialectFile` override is not. If `dialect` is missing, the
  start of the file name is used (`openrouter-...`).
- **`url`, `note`, `source`**: for people only.
- **`status`**: the HTTP status of the response.
- **`request`**: the exact body sent. Its members are model, state, then the questions in the order they were
  asked. Each question has its type, instructions and criteria.
- **`response`**: the body received, as JSON. A recorded body that was not JSON is kept as `{"raw": "<text>"}`.

`expect` holds the checks. Every entry is optional:

- **`error`**: a `BrainErrorName`: `none`, `bad-response`, `exhausted`, `bad-request`, `auth-failed`,
  `misconfigured`, `server-error`... If it is missing, a 200 must parse (`none`), and any other status is not
  checked.
- **`detail`**: a piece of text the error detail must contain (the provider's message).
- **`model`**: the response's model, as the parser reports it.
- **`answers.<question>`**: the checks on one answer:
  - `valid`: true when missing. An answer that is missing counts as invalid.
  - `choice`: the chosen option key.
  - `p`: its probability.
  - `level`: the score level.
  - `score`: the fractional score.
  - `calibrated`: whether p came from the provider.
  - `order`: the first option keys of the distribution, best first.
- **`usage`**: `input_tokens`, `output_tokens`, `usd_micros` (`ceil(usage.cost x 1e6)`, or 0 when the provider
  reports no cost), and `cost_reported`.

Numbers compare with a tolerance of 1e-4.

Fixture files are UTF-8 JSON. The documentation fixtures are plain ASCII, with accented letters written as `é`
escapes. The three compiled-in ones must stay ASCII, because they are pasted into a C++ source file. The strings
follow the compiled-in question set `voice-v1` and the state layout of `BrainStateBuilder` (M2 contract, sections 6
and 7).

A fixture whose `expect.error` is not `none` records a failure or a rejection. Such a fixture tests the parser: a
tool that replays answers, like the mock engine, must skip it.

## What the self-test checks

For each `*.json` of `FixtureDir`, sorted by name, or for the three compiled-in copies when the directory is empty
or has none, the self-test does four things:

1. It rebuilds the job from `request`. Choice criteria give the option keys in order, score criteria give the levels
   `"0".."n-1"`, and yes/no criteria give `true` and `false`.
2. For a 200, it reads `response` with `SystemOneWire::ParseResponse` (engine calibrated, uncalibrated confidence
   0.3), the same way a live engine does. It then compares the error, the answers, the usage and the model with
   `expect`.
3. For any other status, it classifies the response with `SystemOneWire::ClassifyStatus` and compares the class and
   the detail.
4. It builds the request again from the rebuilt job with `SystemOneWire::BuildRequest`. That request must equal
   `request`: the same model, the same state, the same questions in the same order, and for each question the same
   type, instructions and criteria (keys, order and texts). It must also have no extra member.

The output is one line per fixture, `PASS <fixture>` or `FAIL <fixture>: <first difference>`.

## Recording real fixtures

Record OpenRouter first. Record TypeSafe once a key is granted.

1. Set `AiPlayerbot.CoaBrain.Probe.RecordDir` to an absolute directory the worldserver can write.
2. Give the key through the environment of the worldserver, not the conf file:
   `AC_AI_PLAYERBOT_COA_BRAIN_ENGINE_OPENROUTER_API_KEY` (TypeSafe:
   `AC_AI_PLAYERBOT_COA_BRAIN_ENGINE_TYPESAFE_API_KEY`). `Engine.<name>.ApiKeyEnv` can name another variable
   instead.
3. The probe only knows the engines listed in `AiPlayerbot.CoaBrain.Decision.Chain`, so the engine must be there
   (for example `openrouter,typesafe`). Then run `.reload config`. If `AiPlayerbot.CoaBrain.Enable = 0`, also run
   `.playerbots brain on`, which lasts until the next `.reload config`. `.playerbots brain status` shows each
   engine, its masked key and where the key came from.
4. Run `.playerbots brain probe openrouter record`. It asks the canned probe cases (`fr-greet-1`, `en-noise-1`...;
   no player's words) and prints a report when the answers are in.
5. Read the report. The OpenRouter Decisions path is `/api/alpha/decisions` (the default,
   which answered on 2026-09-30). Check the configured URL and model when the probe reports HTTP 404.
6. Each call is saved as `<RecordDir>/<engine>-<case id>-<yyyymmddThhmmssZ>.json`, with `"source": "recorded"` and
   an empty `expect`. Only bodies are written: no header and no key. Still, read each file before you commit it.
7. Fill in its `expect` block from what the response says, following the format above. Add `"fixture"` and
   `"dialect"` if they are missing.
8. Rename the file `<dialect>-<case>.json` (for example `openrouter-fr-greet-1.json`) and commit it here.
9. Set `AiPlayerbot.CoaBrain.FixtureDir` to this directory, `.reload config`, and run
   `.playerbots brain test selftest`. Every fixture must PASS.

A recorded fixture may contradict a documentation one: a field name, a type name, a missing `usage.cost`, or the
URL. In that case:

1. Correct the dialect table in `src/Coa/Brain/Net/SystemOneDialects.cpp`. For a quick check before a rebuild, a
   `AiPlayerbot.CoaBrain.DialectFile` override is enough. The self-test itself always reads the fixtures with the
   compiled-in table.
2. Update or delete the documentation fixtures that were wrong, and their compiled-in copies.
3. Run the self-test again.

## Running the mock engine from these fixtures

```
AiPlayerbot.CoaBrain.Enable = 1
AiPlayerbot.CoaBrain.Decision.Chain = mock
AiPlayerbot.CoaBrain.FixtureDir = /absolute/path/to/mod-playerbots/data/coa/brain/fixtures
AiPlayerbot.CoaBrain.Engine.mock.LatencyMs = 150
AiPlayerbot.CoaBrain.Engine.mock.ErrorPercent = 0
AiPlayerbot.CoaBrain.Engine.mock.FailStatus = 0
AiPlayerbot.CoaBrain.Engine.mock.UsdPerCall = 0
```

For each job, the mock proceeds as follows:

- **Picking a fixture.** It takes the 200 fixture whose request asks the most of the same questions (same name and
  same type). On a tie, it takes the first by file name.
- **Building the answers.** It keeps each of that fixture's choices that is one of the job's options, and draws the
  others at random. A question the fixture does not answer gets a random valid answer.
- **Reading back.** It reads the result through the real parser. `usage.cost` is `UsdPerCall`.
- **Failing.** `ErrorPercent` of the calls fail with `FailStatus` (500 when 0). The body is that of the first
  fixture with this status, so `FailStatus = 402` replays `openrouter-error-402.json`.

With `FixtureDir` empty, the mock answers entirely at random. The mock never opens a socket, and needs no key.

## Text generator (M6)

The decision brain chooses typed answers. The optional text generator writes one short line. A failed,
late, filtered or rejected line uses the prepared template once. All M6 switches default to off.
Enable `AiPlayerbot.CoaBrain.Enable = 1`, select a provider, then set `AiPlayerbot.CoaVoice.TextLlm = 1`
for whispers and group/guild chat, or `2` for all supported chat. An empty `Decision.Chain` runs the
text generator without decision engines.

For an already installed Ollama model (no model is downloaded by these settings):

```ini
AiPlayerbot.CoaBrain.Text.Provider = openai
AiPlayerbot.CoaBrain.Text.Url = http://127.0.0.1:11434/v1/chat/completions
AiPlayerbot.CoaBrain.Text.Model = your-installed-model
```

For an OpenRouter text model:

```ini
AiPlayerbot.CoaBrain.Text.Provider = openai
AiPlayerbot.CoaBrain.Text.Url = https://openrouter.ai/api/v1/chat/completions
AiPlayerbot.CoaBrain.Text.Model = your-text-model
```

For the Anthropic Messages API:

```ini
AiPlayerbot.CoaBrain.Text.Provider = anthropic
AiPlayerbot.CoaBrain.Text.Url = https://api.anthropic.com/v1/messages
AiPlayerbot.CoaBrain.Text.Model = claude-haiku-4-5
```

Give hosted credentials through `AC_AI_PLAYERBOT_COA_BRAIN_TEXT_API_KEY`, then `.reload config`.
`.playerbots brain text status` shows the provider, masked key, budgets, breaker and counters.
`.playerbots brain text test <online-bot> <situation>` reports a simulated whisper to a GM or console;
`.playerbots brain text probe` checks six canned situations without recording prompts or responses.
Do not put credentials in versioned files.

Loopback text receives player words by default. Hosted player words require both
`AiPlayerbot.CoaBrain.Text.SendPlayerText = 1` and `AiPlayerbot.CoaBrain.Privacy.SendPlayerText = 1`.
The prompt uses `Joueur1`; character names are filled in locally after generation. Feature-only memory
may be included; message text and generated lines are not stored. `.playerbots optout ai` stops model
calls for the account, and `.playerbots optout voice` makes bots ignore the player.

Text calls share the global brain budget and also have `Text.MaxUsdPerDay` and
`Text.MaxRequestsPerDay`. The `text` usage row survives restarts. Player replies may use the full text
cap, probes 80%, and ambient banter 50%. `CoaVoice.TextPerSenderPerDay` limits an account's generated
lines. Provider fallback attempts are billed too; enable them only within an appropriate budget.

`AiPlayerbot.CoaVoice.Banter.Enable = 1` allows capped zone exchanges where an eligible real player is
present. It needs voice enabled; answers also need `BotToBot = 1`. Generated openers need `TextLlm = 2`.
`AiPlayerbot.CoaBrain.Use.RpgNext = 1` shares advice by zone, level band and role, blending it with the
existing activity weights. `Rpg.Influence = 0` preserves those weights.

The handoff workspace's `tools/fuzz/build.sh` and `tools/fuzz/run.sh` build and run offline verification:
a sanitizer corpus, 10,000 deterministic mutations, local provider-shaped HTTP faults, budget floods
and exactly-once fallback tests. They use loopback mock servers and no real credentials. The harness
needs the module's static libraries from the CoA core build. In-game chat and banter still need a client.
