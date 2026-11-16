# Phase 2: Foundations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the protocol layer, a host simulator of hub + remote, and an HA integration skeleton, so the whole system is developable and testable with no Harmony hardware attached.

**Architecture:** One versioned message schema (`proto/schema.yaml`) generates both C and Python codecs, so hub firmware and the HA integration can never drift. The hub daemon is real C compiled for the host (`sim` build) with hardware I/O behind injectable sink interfaces — the same source later cross-compiles for MIPS. The HA custom integration talks to the sim hub over WebSocket exactly as it will to real hardware.

**Tech Stack:** C11 (CMake 3.28, GCC host; cross toolchains deferred to Phase 3), vendored cJSON, hand-rolled minimal RFC6455 WebSocket server, Python 3.13 via `uv`, pytest + pytest-xdist + pytest-asyncio + pytest-homeassistant-custom-component, ruff.

**Spec:** `docs/design/2026-11-16-ha-harmony-design.md`

## Global Constraints

- Hub/remote code is **C11**, no C++, no dynamic allocation in RF or IR hot paths; must build with `-Wall -Wextra -Werror -std=c11`.
- Hub target is Linux 2.6.31 / uClibc, big-endian MIPS: **no** `getrandom`, `epoll_pwait2`, `statx`, `renameat2`, `O_TMPFILE`, or glibc-only APIs. Host sim must not depend on them either.
- All multi-byte wire fields are **big-endian**.
- RF payload budget: **hub→remote 30 bytes total frame, remote→hub 32 bytes total frame**, minus a 4-byte header = 26 / 28 bytes payload per fragment.
- Exactly one thread/process may hold the RF transport open (`/dev/rfspi` hazard); the sim transport enforces the same single-owner rule so the constraint is tested on the host.
- Protocol version constant `HP_PROTO_VERSION = 1`; every envelope carries it and a mismatched version is rejected, never coerced.
- HA integration domain is `ha_harmony`; mDNS service type `_haharmony._tcp.local.`; hub WebSocket port **8099** (8088/8089 are left to stock and other tooling).
- Python tests run `pytest -n auto` and must be xdist-safe: per-worker temp dirs and per-worker TCP ports, no shared global state.
- No file in this repo names or credits the reference project.

## Review Focus

1. **Oversized payload** — a page push or mapping table larger than the RF fragment budget must fail with a clear error, not silently truncate. (Task 2)
2. **Fragment reassembly abuse** — out-of-order, duplicate, missing, or mismatched-total fragments, and a fragment stream interleaved with a new message, must not corrupt state or over-read the buffer. (Task 2)
3. **Malformed WebSocket/JSON input** — oversized frames, truncated UTF-8, unknown message types, missing fields, wrong types, and absent auth token must get a protocol error and keep the connection sane. (Task 3)
4. **HA reconnect and hub restart** — the integration must survive hub restart, token rotation, and HA restart without duplicate entities or stuck `unavailable`. (Task 6)
5. **Version skew** — hub and integration on different `HP_PROTO_VERSION` must refuse to pair with an explicit message rather than half-working. (Tasks 3, 6)

---

## File Structure

| Path | Responsibility |
|---|---|
| `proto/schema.yaml` | Single source of truth: message types, fields, wire types, transport (ws/rf/both) |
| `proto/codegen.py` | Generator: schema → C header/source + Python module |
| `proto/gen/harmony_proto.h`, `.c` | Generated C type enums + JSON encode/decode + RF pack/unpack |
| `proto/gen/harmony_proto.py` | Generated Python mirror |
| `hub/rf/framing.c`, `.h` | RF envelope: header, fragmentation, reassembly (hardware-agnostic) |
| `hub/net/ws.c`, `.h` | Minimal RFC6455 server: handshake, text frames, ping/pong, close |
| `hub/net/jsonrpc.c`, `.h` | Envelope validation, auth token check, dispatch table |
| `hub/engine/mapping.c`, `.h` | Compiled button→action tables, lookup, activity state machine |
| `hub/io/sinks.h` | Injectable `hp_ir_sink`, `hp_bt_sink`, `hp_rf_transport` vtables |
| `hub/io/sim_sinks.c` | Host implementations: record calls, replay scripted RF input |
| `hub/main.c` | Daemon wiring; `--sim` flag selects sim sinks |
| `remote/sim/remote_sim.c` | Headless remote: RF client, key injection, holds pushed page state |
| `custom_components/ha_harmony/` | `manifest.json`, `config_flow.py`, `coordinator.py`, `remote.py`, `select.py`, `event.py`, `const.py` |
| `tests/c/` | C unit tests, one executable per module, run via CTest |
| `tests/py/` | pytest: codegen golden tests, cross-language codec tests, integration tests |
| `vendor/cjson/` | Vendored cJSON (single .c/.h pair) |

---

### Task 1: Protocol schema and code generation

**Files:**
- Create: `proto/schema.yaml`, `proto/codegen.py`, `proto/__init__.py`
- Create: `tests/py/test_codegen.py`
- Create: `pyproject.toml` (requires-python `>=3.13`, deps: pytest, pytest-xdist, pytest-asyncio, ruff, pyyaml)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `proto.codegen.generate(schema: Path, out_dir: Path) -> None` writing `harmony_proto.h`, `harmony_proto.c`, `harmony_proto.py`.
  - `proto.codegen.load_schema(path: Path) -> Schema` where `Schema` is a frozen dataclass with `version: int` and `messages: tuple[Message, ...]`; `Message` has `name: str`, `code: int`, `transport: Literal["ws","rf","both"]`, `fields: tuple[Field, ...]`; `Field` has `name: str`, `type: str`, `optional: bool`.
  - Generated Python: `MsgType` IntEnum (one member per message, value = `code`), `PROTO_VERSION: int`, `encode_ws(MsgType, dict) -> dict`, `decode_ws(dict) -> tuple[MsgType, dict]`, `ProtoError`.
  - Generated C: `hp_msg_type_t` enum (`HP_MSG_<NAME>`), `HP_PROTO_VERSION`, `const hp_msg_desc_t *hp_msg_desc(hp_msg_type_t)` exposing field names/types for the dispatcher.

Wire types permitted in the schema: `u8 u16 u32 i32 bool str bytes enum:<name>`. `str`/`bytes` are length-prefixed `u8` on RF.

Message set for Phase 2 (hub↔HA unless noted):
`hello`, `hello_ack`, `error`, `ping`, `pong`, `set_config`, `config_ack`, `command`, `command_result`, `key_event` (rf+ws), `state_update` (both), `page_push` (rf), `battery` (rf).

- [ ] **Step 1: Write the failing tests**

```python
# tests/py/test_codegen.py
def test_load_schema_parses_messages(tmp_path): ...        # name/code/transport/fields round-trip
def test_duplicate_code_rejected(tmp_path): ...            # two messages, same code -> SchemaError
def test_unknown_wire_type_rejected(tmp_path): ...         # type "u64" -> SchemaError
def test_generate_emits_three_files(tmp_path): ...         # .h, .c, .py exist and are non-empty
def test_generated_python_round_trips(tmp_path): ...        # exec() the module, encode_ws then decode_ws
def test_generated_python_rejects_version_mismatch(tmp_path): ...  # envelope v=99 -> ProtoError
def test_generated_c_header_declares_every_message(tmp_path): ...  # HP_MSG_<NAME> present for each
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `uv run pytest tests/py/test_codegen.py -v`
Expected: FAIL — `ModuleNotFoundError: proto.codegen`

- [ ] **Step 3: Write `proto/schema.yaml` with the message set above, then implement `proto/codegen.py`**

Generator emits plain string-templated code; no Jinja. The C emitter writes a static descriptor table (name, code, field count, field name/type arrays) rather than per-message functions, so the dispatcher stays table-driven.

- [ ] **Step 4: Run tests to verify they pass**

Run: `uv run pytest tests/py/test_codegen.py -v` → PASS
Run: `uv run ruff check proto tests` → clean

- [ ] **Step 5: Commit**

```bash
git add pyproject.toml proto tests/py/test_codegen.py
git commit -m "feat(proto): message schema and C/Python code generator"
```

---

### Task 2: RF framing — header, fragmentation, reassembly

**Files:**
- Create: `hub/rf/framing.h`, `hub/rf/framing.c`
- Create: `tests/c/test_framing.c`, `tests/c/ctest_assert.h`
- Create: `CMakeLists.txt`, `tests/c/CMakeLists.txt`
- Create: `proto/rf_codec.py`, `tests/py/test_rf_codec.py`

**Interfaces:**
- Consumes: `hp_msg_type_t` from Task 1.
- Produces:
  - C: `int hp_rf_split(hp_msg_type_t t, uint8_t seq, const uint8_t *payload, size_t len, size_t frame_cap, hp_rf_frame_t *out, size_t out_cap, size_t *out_n)` returning `0`/`-HP_E_TOOBIG`/`-HP_E_INVAL`.
  - C: `hp_rf_asm_t` + `void hp_rf_asm_init(hp_rf_asm_t *)` and `int hp_rf_asm_push(hp_rf_asm_t *, const uint8_t *frame, size_t len, hp_rf_msg_t *out)` returning `HP_ASM_INCOMPLETE`, `HP_ASM_COMPLETE`, or a negative error.
  - Python mirror: `proto.rf_codec.split(msg_type: int, seq: int, payload: bytes, frame_cap: int) -> list[bytes]` and `class Reassembler` with `push(frame: bytes) -> tuple[int, bytes] | None`.

Header layout (4 bytes, big-endian): `type u8`, `seq u8`, `frag u8` (`index << 4 | total`), `len u8` (payload bytes in *this* fragment). Max 15 fragments.

- [ ] **Step 1: Write the failing tests**

C (`tests/c/test_framing.c`): single-fragment round trip; exact-boundary payload (26 and 28 bytes); multi-fragment round trip; payload needing 16 fragments → `-HP_E_TOOBIG`; duplicate fragment ignored; out-of-order fragments reassemble; mismatched `total` across fragments → error; new `seq` mid-stream discards the partial; `len` larger than the frame → error, no over-read.

Python (`tests/py/test_rf_codec.py`): the same cases, plus `test_python_matches_c_vectors` which reads `tests/vectors/rf_frames.json` (committed golden vectors produced from the C implementation) and asserts `split` output matches byte-for-byte.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure`
Expected: FAIL — `hub/rf/framing.c` does not compile / tests absent
Run: `uv run pytest tests/py/test_rf_codec.py -v` → FAIL

- [ ] **Step 3: Implement `hp_rf_split` / `hp_rf_asm_*` in C and the Python mirror**

Reassembler state is a fixed-size struct: current `type`, `seq`, `total`, a received-index bitmask, a `uint8_t buf[15 * 28]`, and per-index lengths. No allocation. Validate `index < total` and `total >= 1` before any write, and bounds-check `len` against both the frame and the slot.

- [ ] **Step 4: Run tests to verify they pass**

Run: `cmake --build build && ctest --test-dir build --output-on-failure` → all pass
Run: `uv run pytest tests/py/test_rf_codec.py -v` → PASS

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt hub/rf tests/c tests/vectors proto/rf_codec.py tests/py/test_rf_codec.py
git commit -m "feat(rf): framing with fragmentation and bounds-checked reassembly"
```

---

### Task 3: WebSocket server and envelope dispatch

**Files:**
- Create: `hub/net/ws.h`, `hub/net/ws.c`, `hub/net/jsonrpc.h`, `hub/net/jsonrpc.c`
- Create: `vendor/cjson/cJSON.c`, `vendor/cjson/cJSON.h`
- Create: `tests/c/test_ws.c`, `tests/c/test_jsonrpc.c`
- Create: `tests/py/test_ws_server.py`

**Interfaces:**
- Consumes: `hp_msg_desc` (Task 1).
- Produces:
  - `hp_ws_server_t *hp_ws_listen(uint16_t port, const char *token, const hp_ws_cb_t *cb, void *user)`, `int hp_ws_poll(hp_ws_server_t *, int timeout_ms)`, `int hp_ws_send(hp_ws_conn_t *, const char *text, size_t len)`, `void hp_ws_close(hp_ws_server_t *)`.
  - `hp_ws_cb_t` holds `void (*on_text)(hp_ws_conn_t *, const char *, size_t, void *)` and `void (*on_close)(hp_ws_conn_t *, void *)`.
  - `int hp_dispatch(const char *json, size_t len, const hp_handler_tbl_t *tbl, void *user, char *reply, size_t reply_cap, size_t *reply_len)`.

Envelope: `{"v":1,"id":<u32>,"t":"<message name>","d":{...}}`. Auth: the client sends `hello` carrying `token`; any other message before a successful `hello` gets `error` with code `unauthorized` and the connection closes. Use `poll(2)` (not epoll) for portability to the 2.6.31 kernel.

- [ ] **Step 1: Write the failing tests**

C: SHA1 and base64 of the RFC6455 example key produce the documented accept value; handshake rejects a missing `Sec-WebSocket-Key`; a text frame with a 126-byte extended length parses; a frame over `HP_WS_MAX_FRAME` is rejected with close code 1009; an unmasked client frame is rejected; ping gets a pong with the same payload. Dispatch: unknown `t` → `error`/`unknown_type`; `v` mismatch → `error`/`version`; missing required field → `error`/`bad_request`; wrong field type → `error`/`bad_request`; valid `hello` with the right token → `hello_ack`; any message before `hello` → `error`/`unauthorized`.

Python (`tests/py/test_ws_server.py`): launch the built `hub` binary with `--sim --port <per-worker port>`, connect with a real WebSocket client, and assert the `hello` → `hello_ack` handshake, a bad token's rejection, and that an oversized frame closes the connection rather than crashing the daemon.

- [ ] **Step 2: Run tests to verify they fail**

Run: `cmake --build build && ctest --test-dir build --output-on-failure` → FAIL
Run: `uv run pytest tests/py/test_ws_server.py -v` → FAIL

- [ ] **Step 3: Implement the WebSocket server, then the dispatcher**

`ws.c`: single-threaded `poll` loop, fixed per-connection read buffer `HP_WS_MAX_FRAME = 64 KiB`, text frames only, fragmented frames reassembled up to that cap, mask always applied, `Sec-WebSocket-Accept` from a local SHA1 + base64. `jsonrpc.c`: parse with cJSON, validate against `hp_msg_desc`, then call the handler from `hp_handler_tbl_t`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure` → all pass
Run: `uv run pytest tests/py/test_ws_server.py -n auto -v` → PASS

- [ ] **Step 5: Commit**

```bash
git add vendor hub/net tests/c/test_ws.c tests/c/test_jsonrpc.c tests/py/test_ws_server.py
git commit -m "feat(net): minimal WebSocket server and validating envelope dispatch"
```

---

### Task 4: Mapping engine, sinks, and the sim daemon

**Files:**
- Create: `hub/io/sinks.h`, `hub/io/sim_sinks.c`, `hub/engine/mapping.h`, `hub/engine/mapping.c`, `hub/main.c`
- Create: `tests/c/test_mapping.c`
- Modify: `CMakeLists.txt` (add the `hub` executable)

**Interfaces:**
- Consumes: Tasks 1–3.
- Produces:
  - `hp_ir_sink` vtable: `int (*send)(void *self, uint8_t port_mask, const uint16_t *pronto, size_t n)`.
  - `hp_bt_sink` vtable: `int (*key)(void *self, uint16_t usage, bool down)`.
  - `hp_rf_transport` vtable: `int (*send)(void *self, const uint8_t *frame, size_t len)`, `int (*recv)(void *self, uint8_t *frame, size_t cap, int timeout_ms)`, `int (*claim)(void *self)` returning `-HP_E_BUSY` on a second claim (the single-owner rule). Task 5 supplies the socketpair implementation; Task 4 ships only a scripted-input stub.
  - `hp_sinks_t`: a struct of `hp_ir_sink *ir`, `hp_bt_sink *bt`, `hp_rf_transport *rf`, passed to every engine call rather than reached through a global.
  - `hp_map_t` + `int hp_map_load(hp_map_t *, const char *json, size_t len)`, `int hp_map_lookup(const hp_map_t *, const char *activity, uint16_t key_code, hp_action_t *out)`, `int hp_map_apply(hp_map_t *, const hp_action_t *, const hp_sinks_t *)`.
  - `int hp_activity_set(hp_map_t *, const char *name, const hp_sinks_t *)` running the activity's ordered steps.
  - Persistence: `int hp_map_save_cache(const hp_map_t *, const char *path)` / `hp_map_load_cache(hp_map_t *, const char *path)` — the HA-offline fallback.

`hp_action_t` is a tagged union over `HP_ACT_IR`, `HP_ACT_BT`, `HP_ACT_HA_EVENT` (forwarded to HA only), `HP_ACT_NONE`.

- [ ] **Step 1: Write the failing tests**

`set_config` JSON loads into `hp_map_t`; lookup finds a key in the active activity and falls back to the global map; an unmapped key yields `HP_ACT_NONE` and still emits an HA event; `hp_map_apply` with an IR action calls the IR sink once with the right port mask and Pronto payload; a BT action calls the BT sink; `hp_activity_set` runs steps in order and records them on the sim sinks; a config whose JSON is malformed leaves the previous map intact; cache save/load round-trips and a corrupt cache file is rejected without clobbering the in-memory map.

- [ ] **Step 2: Run tests to verify they fail**

Run: `ctest --test-dir build -R mapping --output-on-failure` → FAIL

- [ ] **Step 3: Implement `mapping.c`, `sim_sinks.c`, and wire `hub/main.c`**

`main.c` with `--sim` installs the recording sinks and a scripted RF transport; without it, the real device sinks are a Phase 3 concern and `main` exits with a clear "no hardware backend built" error. Sim sinks append to an in-memory ring the tests read, and mirror each call as a line on stdout so Python tests can assert on it.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure` → all pass

- [ ] **Step 5: Commit**

```bash
git add hub/io hub/engine hub/main.c tests/c/test_mapping.c CMakeLists.txt
git commit -m "feat(engine): mapping tables, activity steps, injectable sinks, sim daemon"
```

---

### Task 5: Headless remote simulator

**Files:**
- Create: `remote/sim/remote_sim.c`
- Create: `hub/io/rf_pair.h`, `hub/io/rf_pair.c` (in-process socketpair RF transport shared by hub sim and remote sim)
- Create: `tests/py/test_sim_e2e.py`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–4, including the `hp_rf_transport` vtable from `hub/io/sinks.h`.
- Produces:
  - `hp_rf_pair_create(const char *path, hp_rf_transport **out)` — a socketpair-backed `hp_rf_transport` implementation shared by both simulators.
  - `remote_sim` CLI: `--rf <socket path>`, `--press <key code>`, `--dump-page` (prints the last `page_push` as JSON).

- [ ] **Step 1: Write the failing tests**

C: a second `claim()` on the same transport returns `-HP_E_BUSY`. Python (`tests/py/test_sim_e2e.py`): start `hub --sim` and `remote_sim` joined by a socketpair; push a config from a WebSocket client; `--press` a mapped key and assert the hub's IR sink recorded the expected Pronto payload *and* a `key_event` arrived on the WebSocket; press an unmapped key and assert only the `key_event` arrives; push a page and assert `--dump-page` returns it unchanged; push a page larger than the fragment budget and assert a clean `error` rather than a truncated page.

- [ ] **Step 2: Run tests to verify they fail**

Run: `uv run pytest tests/py/test_sim_e2e.py -v` → FAIL

- [ ] **Step 3: Implement `rf_pair.c` and `remote_sim.c`**

`remote_sim` is a single-threaded loop over the RF transport that reassembles with `hp_rf_asm_*`, stores the last page, and sends `key_event` on `--press`. No UI; LVGL rendering is Phase 3.

- [ ] **Step 4: Run tests to verify they pass**

Run: `ctest --test-dir build --output-on-failure` → all pass
Run: `uv run pytest tests/py -n auto -v` → PASS

- [ ] **Step 5: Commit**

```bash
git add remote/sim hub/io/rf_pair.* tests/py/test_sim_e2e.py CMakeLists.txt
git commit -m "feat(sim): headless remote simulator over in-process RF transport"
```

---

### Task 6: Home Assistant integration skeleton

**Files:**
- Create: `custom_components/ha_harmony/{__init__.py,manifest.json,const.py,api.py,coordinator.py,config_flow.py,remote.py,select.py,event.py,strings.json}`
- Create: `tests/py/conftest.py`, `tests/py/test_config_flow.py`, `tests/py/test_entities.py`
- Modify: `pyproject.toml` (add `pytest-homeassistant-custom-component`)

**Interfaces:**
- Consumes: `proto.gen.harmony_proto` (Task 1); a running `hub --sim` (Tasks 3–5).
- Produces:
  - `api.HarmonyHubClient(host: str, port: int, token: str, session: ClientSession)` with `async_connect()`, `async_send(MsgType, dict) -> dict`, `async_subscribe(Callable[[MsgType, dict], None]) -> Callable[[], None]`, `async_close()`; raises `HarmonyAuthError`, `HarmonyVersionError`, `HarmonyConnectionError`.
  - `coordinator.HarmonyCoordinator(hass, entry, client)` — a push coordinator holding `activities: list[str]`, `current_activity: str | None`, `devices: list[str]`, `available: bool`.
  - Entities: `remote.HarmonyRemote` (`turn_on`/`turn_off`/`send_command`), `select.HarmonyActivitySelect`, `event.HarmonyKeyEvent` (one per paired remote, `event_types` from the key map).

`manifest.json`: `"domain": "ha_harmony"`, `"iot_class": "local_push"`, `"zeroconf": ["_haharmony._tcp.local."]`, `"config_flow": true`, `"requirements": []`.

- [ ] **Step 1: Write the failing tests**

`test_config_flow.py`: user flow with a reachable sim hub creates the entry with host/port/token; a bad token shows `invalid_auth`; an unreachable host shows `cannot_connect`; a hub on a different `PROTO_VERSION` shows `unsupported_version`; zeroconf discovery prefills the host and aborts as `already_configured` on a duplicate unique id.
`test_entities.py`: setup creates exactly one remote, one activity select, and one event entity; a pushed `state_update` flips `select` to the new activity without polling; a `key_event` fires the HA event entity with the right `event_type`; hub disconnect marks entities `unavailable` and reconnect restores them with no duplicates.

- [ ] **Step 2: Run tests to verify they fail**

Run: `uv run pytest tests/py/test_config_flow.py tests/py/test_entities.py -v`
Expected: FAIL — `custom_components.ha_harmony` not importable

- [ ] **Step 3: Implement the client, coordinator, config flow, and the three platforms**

`api.py` wraps `aiohttp`'s WebSocket client, matching replies to requests by envelope `id` with a future map, and reconnects with capped exponential backoff. The coordinator pushes updates via `async_set_updated_data`; entities are `CoordinatorEntity` subclasses keyed off a hub id reported in `hello_ack`.

- [ ] **Step 4: Run tests to verify they pass**

Run: `uv run pytest tests/py -n auto -v` → PASS
Run: `uv run ruff check custom_components proto tests` → clean

- [ ] **Step 5: Commit**

```bash
git add custom_components tests/py/conftest.py tests/py/test_config_flow.py tests/py/test_entities.py pyproject.toml
git commit -m "feat(ha): integration skeleton with config flow, coordinator, and entities"
```

---

### Task 7: CI

**Files:**
- Create: `.github/workflows/ci.yml`
- Modify: `pyproject.toml` (add `pytest-shard`, which supplies `--shard-id`/`--num-shards`)

- [ ] **Step 1: Write the workflow**

Jobs: `c-tests` (configure, build with `-Werror`, `ctest`), `py-tests` (matrix `shard: [1,2,3]`, each `uv run pytest -n auto --shard-id=${{matrix.shard}} --num-shards=3`), `lint` (`ruff check`), and a single `ci` aggregator job that `needs` all of them and is the one required check.

- [ ] **Step 2: Verify locally**

Run: `cmake --build build && ctest --test-dir build && uv run pytest tests/py -n auto && uv run ruff check .`
Expected: all green

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: C and Python test matrix with single aggregator check"
```

---

## Verification (end of Phase 2)

1. `cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure` — all C unit tests pass.
2. `uv run pytest tests/py -n auto` — codegen, codec cross-language, WebSocket, and HA integration tests pass.
3. End-to-end on the host with no Harmony hardware: start `hub --sim` and `remote_sim`, add the integration against `127.0.0.1:8099` in a real HA dev instance, push a config, then `remote_sim --press` a mapped key and watch the activity select change and the event entity fire in HA.
4. `uv run ruff check .` — clean.
