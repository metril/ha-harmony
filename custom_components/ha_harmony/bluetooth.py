"""The hub's Bluetooth feature: the latest `bt_state`, commands, remote-key events and the editor relay.

Only a hub whose `hello_ack` carried `bt` has any of it. `bt_pair` / `bt_forget` replies are snapshots (the outcome
arrives as a later `bt_state` push), so nothing here waits for an outcome. The add-on is not trusted: its commands
are re-validated with the hub's limits (`harmony_core.bt_relay.check_command`) before anything is sent.
"""

from __future__ import annotations

import asyncio
import collections
import logging
import re
import time
from typing import TYPE_CHECKING, Any

from homeassistant.exceptions import HomeAssistantError

from . import const
from .addon_link import AddonClient, AddonError, AddonRejected
from .api import HarmonyCommandError, HarmonyConnectionError
from .errors import invalid
from .harmony_core.bt_relay import (
    MAX_COMMANDS,
    RelayError,
    addr_key,
    check_command,
    clean_limits,
    clean_state,
    newer_gen,
    norm_addr,
)
from .harmony_core.harmony_proto import MsgType

if TYPE_CHECKING:
    from .coordinator import HarmonyCoordinator

_LOGGER = logging.getLogger(__name__)

TOPIC_BT = "bt"  # a new bt_state (or the hub's Bluetooth support changed)
_ID = re.compile(r"^[A-Za-z0-9_-]{1,16}$")
RESULTS_MAX = 32
EVENTS_MAX = 64
LEASE_MAX = 300
EVENT_RATE = 20  # remote-key events per second fired on the HA bus (burst of the same size)

HUB_ERRORS = {  # the hub's error codes (docs/proto/bluetooth.md), as a sentence
    "bad_addr": "that is not a Bluetooth address",
    "no_candidate": "that device is not in the list any more; scan again",
    "unsupported": "this hub does not support that",
    "bad_secs": "the pairing time must be 1 to 300 seconds",
    "bad_transport": "unknown transport",
    "bad_mode": "unknown pairing mode",
    "bad_kind": "unknown kind",
    "bad_page": "unsupported HID usage page",
    "bad_action": "unknown key action",
    "no_le": "this hub has no Bluetooth LE",
    "no_slot": "nothing is paired in that slot",
    "not_connected": "that device is not connected",
    "bt_not_ready": "the Bluetooth adapter is not ready yet",
    "bt_busy": "the Bluetooth helper is busy; try again",
}


class BtError(HomeAssistantError):
    """A Bluetooth command the hub (or the link to it) refused. `code` is the hub's word, `msg` a sentence."""

    def __init__(self, key: str, code: str, msg: str) -> None:
        super().__init__(
            translation_domain=const.DOMAIN, translation_key=key, translation_placeholders={"error": msg}
        )
        self.code, self.msg = _code(code), str(msg)


def _code(raw: object) -> str:
    """A hub error code as the add-on accepts it (`[a-z_]{1,32}`); anything else is "failed"."""
    return re.sub(r"[^a-z_]", "_", str(raw or "").lower())[:32] or "failed"


class BtManager:
    def __init__(self, c: HarmonyCoordinator) -> None:
        self.c = c
        self.state: dict[str, Any] | None = None
        self.gen: int | None = None
        self._supported = c.client.bt is not None
        self._hub_listen = False  # the hub has this socket opted in (reset by every hello)
        self._leases: dict[str, float] = {}  # who asked to listen -> monotonic deadline
        self._timer: asyncio.TimerHandle | None = None
        self.addon_listen = False  # the editor wants key events (add-on answer)
        self._events: collections.deque[tuple[int, dict[str, Any]]] = collections.deque(maxlen=EVENTS_MAX)
        self._eseq = 0  # seq of the last queued key event: reports are acknowledged by it
        self.events_dropped = 0  # queued key events pushed out of a full ring before the add-on took them
        self.events_rate_dropped = 0  # key events not fired on the HA bus (rate limit)
        self._tokens, self._token_t = float(EVENT_RATE), time.monotonic()
        self._rejected_logged = False
        self._listen_lock = asyncio.Lock()
        self._fast_hint = False
        self._results: collections.deque[dict[str, Any]] = collections.deque(maxlen=RESULTS_MAX)
        self.exchanges = 0  # relay round trips made (observable by tests)
        if c.client.bt_state is not None:
            self.on_state(c.client.bt_state)

    # ------------------------------------------------------------ facts
    @property
    def supported(self) -> bool:
        return self.c.client.bt is not None

    @property
    def limits(self) -> dict[str, Any] | None:
        return clean_limits(self.c.client.bt)

    @property
    def window(self) -> str | None:
        return self.state.get("window") if self.state else None

    def listening(self) -> bool:
        now = time.monotonic()
        self._leases = {k: v for k, v in self._leases.items() if v > now}
        return bool(self._leases) or self.addon_listen

    def peers(self) -> list[tuple[str, str, str | None]]:
        """(kind, address, name) of every Bluetooth device and remote the loaded config lists, in config order.
        Bonds the config does not list are never entities."""
        hub = self.c.compiled.hub_json
        out: list[tuple[str, str, str | None]] = []
        for kind, key in (("host", "bt_hosts"), ("remote", "bt_remotes")):
            seen: set[str] = set()
            for row in hub.get(key, ()):
                a = norm_addr(row.get("addr"))
                if a is not None and a not in seen:
                    seen.add(a)
                    out.append((kind, a, row.get("name") if isinstance(row.get("name"), str) else None))
        return out

    def expected_unique_ids(self) -> set[str]:
        if not self.supported:
            return set()
        h = self.c.hub_id
        return {f"{h}_bt"} | {f"{h}_btconn_{kind}_{addr_key(a)}" for kind, a, _ in self.peers()}

    def entry(self, kind: str, addr: str) -> dict[str, Any] | None:
        """The latest bt_state entry (bond) of a configured peer, or None when it is not bonded."""
        rows = (self.state or {}).get("hosts" if kind == "host" else "remotes", ())
        return next((e for e in rows if e["addr"] == addr), None)

    def find(self, addr: str) -> list[tuple[str, dict[str, Any]]]:
        """(kind, entry) for every bond in the latest state with this address."""
        a = norm_addr(addr)
        out: list[tuple[str, dict[str, Any]]] = []
        if a and self.state:
            out += [("host", e) for e in self.state["hosts"] if e["addr"] == a]
            out += [("remote", e) for e in self.state["remotes"] if e["addr"] == a]
        return out

    # ------------------------------------------------------------ hub -> here
    def on_state(self, raw: Any) -> None:
        """A bt_state (push or reply). One with an older `gen` than the one held is ignored."""
        st = clean_state(raw)
        if st is None:
            return
        gen = st.get("gen")
        if gen is not None and self.gen is not None and not newer_gen(gen, self.gen):
            return
        if gen is not None:
            self.gen = gen
        if st != self.state:
            self.state = st
            self.c._notify(TOPIC_BT)

    def on_connect(self) -> None:
        """The hub link (re)started: its helper state is whatever it pushes next, and the key listen was reset by hello."""
        had = self._supported
        self._supported = self.supported
        self.state, self.gen, self._hub_listen = None, None, False
        if self.c.client.bt_state is not None:
            self.on_state(self.c.client.bt_state)
        if had != self._supported:
            for cb in list(self.c._config_listeners):  # entities appear / go
                cb()
        self.c._notify(TOPIC_BT)
        if self.listening():
            self.c.config_entry.async_create_background_task(
                self.c.hass, self.sync_listen(), f"{const.DOMAIN} bt listen"
            )

    def on_key(self, data: dict[str, Any]) -> None:
        """A key from a bonded remote. Delivered only while something asked to listen."""
        if not self.listening():
            return
        slot, page, usage, down = data.get("slot"), data.get("page"), data.get("usage"), data.get("down")
        if (
            type(slot) is not int
            or type(page) is not int
            or type(usage) is not int
            or type(down) is not bool
            or not 0 <= slot <= 255
            or not 0 <= page <= 255
            or not 0 <= usage <= 0xFFFF
        ):
            return  # the add-on would refuse the whole report over it
        event = {"slot": slot, "page": page, "usage": usage, "down": down}
        if self.addon_listen:
            if len(self._events) == EVENTS_MAX:
                self.events_dropped += 1  # the oldest unsent one goes
            self._eseq += 1
            self._events.append((self._eseq, event))
        now = time.monotonic()  # token bucket: a stuck or chattering remote must not flood the event bus
        self._tokens = min(EVENT_RATE, self._tokens + (now - self._token_t) * EVENT_RATE)
        self._token_t = now
        if self._tokens < 1:
            self.events_rate_dropped += 1
            return
        self._tokens -= 1
        remote = next((e for e in (self.state or {}).get("remotes", ()) if e["slot"] == slot), None)
        self.c.hass.bus.async_fire(
            const.EVENT_BT_REMOTE_KEY,
            {
                "hub_id": self.c.hub_id,
                **event,
                "address": remote["addr"] if remote else None,
                "name": remote.get("name") if remote else None,
            },
        )

    # ------------------------------------------------------------ listening
    async def sync_listen(self) -> None:
        """Make the hub's opt-in match what is wanted; never raises."""
        async with self._listen_lock:  # one at a time: a second call sees what the first achieved
            want = self.listening() and self.supported
            if want == self._hub_listen or not self.c.available:
                return
            try:
                await self.c.client.async_send(MsgType.BT_LISTEN, {"on": want})
                self._hub_listen = want
            except (HarmonyCommandError, HarmonyConnectionError) as exc:
                _LOGGER.debug("bt_listen failed: %s", exc)

    def _arm_timer(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None
        if self._leases:
            loop = self.c.hass.loop
            delay = max(0.0, min(self._leases.values()) - time.monotonic())
            self._timer = loop.call_later(
                delay + 0.01,
                lambda: self.c.config_entry.async_create_background_task(
                    self.c.hass, self._expired(), f"{const.DOMAIN} bt listen end"
                ),
            )

    async def _expired(self) -> None:
        self.listening()  # drops the lapsed leases, so the next timer is for one that is still running
        self._arm_timer()
        await self.sync_listen()

    async def async_listen(self, on: bool, secs: int = const.BT_LISTEN_DEFAULT, source: str = "service") -> None:
        """Ask the hub for `bt_remote_key` for `secs` (the service's lease) or stop. The HA event fires only meanwhile."""
        self._need_bt()
        if on:
            if type(secs) is not int or not 1 <= secs <= LEASE_MAX:
                raise invalid("bt_invalid", error=f"seconds are 1 to {LEASE_MAX}")
            self._leases[source] = time.monotonic() + secs
        else:
            self._leases.pop(source, None)
        self._arm_timer()
        await self.sync_listen()

    def shutdown(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None

    # ------------------------------------------------------------ commands
    def _need_bt(self) -> None:
        if not self.supported:
            raise BtError("bt_unsupported", "unsupported", "this hub has no Bluetooth")

    async def _send(self, mtype: MsgType, payload: dict[str, Any]) -> None:
        try:
            res = await self.c.client.async_send(mtype, payload)
        except HarmonyCommandError as exc:
            raise BtError("bt_failed", exc.code, HUB_ERRORS.get(exc.code, exc.message or exc.code)) from exc
        except HarmonyConnectionError as exc:
            raise BtError("bt_failed", "offline", "the hub is not connected") from exc
        if mtype is not MsgType.BT_KEY:
            self.on_state(res)  # the snapshot taken when the hub accepted the request

    async def execute(self, raw: dict[str, Any]) -> None:
        """Validate (with the hub's limits) and run one command `{op, ...}`: pair, pair_stop, pick, forget, key."""
        self._need_bt()
        try:
            cmd = check_command(raw, self.limits)
        except RelayError as exc:
            raise invalid("bt_invalid", error=exc.msg) from exc
        await self._do(cmd)

    async def _do(self, cmd: dict[str, Any]) -> None:
        op = cmd["op"]
        if op == "pair":
            await self._send(
                MsgType.BT_PAIR,
                {"on": True, "secs": cmd["secs"], "transport": cmd["transport"], "mode": cmd["mode"]},
            )
        elif op == "pair_stop":
            await self._send(MsgType.BT_PAIR, {"on": False})
        elif op == "pick":
            await self._send(MsgType.BT_PAIR, {"on": True, "mode": "remote", "addr": cmd["addr"]})
        elif op == "forget":
            await self._send(MsgType.BT_FORGET, {"slot": cmd["slot"], "kind": cmd["kind"]})
        else:  # key
            await self._send(MsgType.BT_KEY, {k: cmd[k] for k in ("slot", "page", "usage", "action")})

    async def pair(
        self,
        on: bool,
        secs: int | None = None,
        mode: str = "host",
        transport: str = "any",
        addr: str | None = None,
    ) -> None:
        """Open or close a pairing window; with `addr` (remote mode) bond that scanned candidate."""
        if not on:
            await self.execute({"op": "pair_stop"})
        elif addr is not None:
            if mode != "remote":
                raise invalid("bt_invalid", error="an address is only for pairing a remote")
            await self.execute({"op": "pick", "addr": addr})
        else:
            await self.execute(
                {"op": "pair", "mode": mode, "transport": transport, **({} if secs is None else {"secs": secs})}
            )

    async def forget(self, kind: str, slot: int) -> None:
        await self.execute({"op": "forget", "kind": kind, "slot": slot})

    async def key(self, slot: int, page: int, usage: int, action: str) -> None:
        await self.execute({"op": "key", "slot": slot, "page": page, "usage": usage, "action": action})

    async def forget_address(self, addr: str) -> None:
        self._need_bt()
        a = norm_addr(addr)
        if a is None:
            raise invalid("bt_invalid", error="an address looks like AA:BB:CC:DD:EE:FF")
        found = self.find(a)
        if not found:
            raise invalid("bt_unknown_address", address=a)
        if len(found) > 1:
            raise invalid("bt_ambiguous_address", address=a)
        kind, entry = found[0]
        await self.execute({"op": "forget", "kind": kind, "slot": entry["slot"]})

    # ------------------------------------------------------------ add-on relay
    def start_relay(self, client: AddonClient) -> None:
        self.c.config_entry.async_create_background_task(
            self.c.hass, self._relay_loop(client), f"{const.DOMAIN} bt relay {self.c.hub_id}"
        )

    def _report(self) -> tuple[dict[str, Any], int, int]:
        """The report body, how many results it carries and the highest event seq in it."""
        events = list(self._events)
        body: dict[str, Any] = {
            "hub": self.c.hub_id,
            "supported": self.supported,
            "online": self.c.available,
            "results": list(self._results),
            "events": [e for _, e in events],
        }
        if self.supported:
            body["state"] = self.state
            body["limits"] = self.limits
        return body, len(body["results"]), events[-1][0] if events else 0

    def _sent(self, results: int, event_seq: int) -> None:
        """Forget what the add-on has taken: that many results, the events up to `event_seq` (by seq, so a
        newer one that arrived meanwhile stays)."""
        for _ in range(min(results, len(self._results))):
            self._results.popleft()
        while self._events and self._events[0][0] <= event_seq:
            self._events.popleft()

    async def _run_command(self, raw: Any) -> bool:
        """One command from the add-on: validated again here, its outcome queued for the next report.
        False when it had no usable id (nobody can be told)."""
        cid = raw.get("id") if isinstance(raw, dict) else None
        if not isinstance(cid, str) or not _ID.fullmatch(cid):
            return False
        res: dict[str, Any] = {"id": cid, "ok": True}
        try:
            self._need_bt()
            await self._do(check_command(raw, self.limits))
        except RelayError as exc:
            res |= {"ok": False, "code": _code(exc.code), "msg": exc.msg[:200]}
        except BtError as exc:
            res |= {"ok": False, "code": _code(exc.code), "msg": exc.msg[:200]}
        except HomeAssistantError as exc:
            res |= {"ok": False, "code": "invalid", "msg": str(getattr(exc, "translation_placeholders", {}).get("error", "refused"))[:200]}
        except Exception:  # a bug must not stop the relay
            _LOGGER.exception("Bluetooth relay command failed")
            res |= {"ok": False, "code": "failed", "msg": "internal error"}
        self._results.append(res)
        return True

    async def _swap(self, client: AddonClient) -> list[Any]:
        """One report; what the add-on asked for comes back as a list of commands. A 400 from the add-on drops what
        was sent (one bad item must not be resent for ever)."""
        body, n_results, seq = self._report()
        try:
            data = await client.bt_exchange(body)
        except AddonRejected:
            self._sent(n_results, seq)
            if not self._rejected_logged:
                self._rejected_logged = True
                _LOGGER.warning("The add-on refused a Bluetooth report; dropped it")
            raise
        self._sent(n_results, seq)
        self.exchanges += 1
        want = data.get("listen") is True
        if want != self.addon_listen:
            self.addon_listen = want
            if not want:
                self._events.clear()
            await self.sync_listen()
        self._fast_hint = data.get("fast") is True
        cmds = data.get("commands")
        return cmds[:MAX_COMMANDS] if isinstance(cmds, list) else []

    async def exchange_once(self, client: AddonClient) -> bool:
        """One report/collect round trip, and the commands it brought. Each result is reported as soon as its
        command finishes (a slow one does not hold back the others). True when a command with a usable id ran."""
        cmds = await self._swap(client)
        ran = False
        budget = 4 * MAX_COMMANDS  # commands per round, however many exchanges bring more
        while cmds and budget > 0:
            budget -= 1
            ran = await self._run_command(cmds.pop(0)) or ran
            if cmds and self._results:
                cmds += (await self._swap(client))[: max(0, budget - len(cmds))]
        return ran

    def fast(self) -> bool:
        return bool(self.window) or self.listening() or self._fast_hint or bool(self._results)

    def _addon_gone(self) -> None:
        """The add-on stopped answering: nobody is reading keys for it any more."""
        if self.addon_listen:
            self.addon_listen = False
            self.c.config_entry.async_create_background_task(
                self.c.hass, self.sync_listen(), f"{const.DOMAIN} bt listen off"
            )
        self._events.clear()

    async def _relay_loop(self, client: AddonClient) -> None:
        await self.c._first_apply.wait()  # the add-on knows this hub id once the bundle loop has polled it
        delay = const.LINK_BACKOFF_MIN
        while True:
            try:
                ran = await self.exchange_once(client)
                delay = const.LINK_BACKOFF_MIN
                self._rejected_logged = False
                # a valid command was run: report it at once; otherwise never faster than the fast cadence
                if not ran:
                    await asyncio.sleep(const.BT_POLL_FAST if self.fast() else const.BT_POLL_IDLE)
            except AddonRejected:
                await asyncio.sleep(const.BT_POLL_IDLE)
            except AddonError:
                self._addon_gone()
                await asyncio.sleep(delay)
                delay = min(delay * 2, 60.0)
            except Exception:
                _LOGGER.exception("Unexpected error in the Bluetooth relay; retrying")
                await asyncio.sleep(const.LINK_BACKOFF_MAX)
