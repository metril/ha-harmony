"""WebSocket client for the Harmony hub: request/reply matching, push fan-out, reconnect."""

from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import random
import time
from collections.abc import Awaitable, Callable
from typing import Any

import aiohttp

from .harmony_core.harmony_proto import (
    PROTO_VERSION,
    MsgType,
    ProtoError,
    decode_ws_full,
    encode_ws,
)
from .harmony_core.hub_limits import HubLimits

_LOGGER = logging.getLogger(__name__)

SET_CONFIG_SINGLE_MAX = 40000  # bytes of compact config text sent as one set_config frame (the hub's frame cap is 64 KiB)
CHUNK_FALLBACK = 12000  # config_chunk text bytes per chunk (the hub takes up to its `chunk`; JSON escaping can double it)

PushCallback = Callable[[MsgType, dict[str, Any]], None]
# connected, error. error is set when the client gave up for good (auth/version).
StateCallback = Callable[[bool, "HarmonyError | None"], None]


class HarmonyError(Exception):
    """Base class for hub client errors."""


class HarmonyAuthError(HarmonyError):
    """The hub rejected the token."""


class HarmonyVersionError(HarmonyError):
    """Hub and client speak different protocol versions."""


class HarmonyConnectionError(HarmonyError):
    """Transport failure or timeout."""


class HarmonyCommandError(HarmonyError):
    """The hub answered a request with an error message."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class HarmonyHubClient:
    """One hub connection that heals itself until closed or permanently refused."""

    def __init__(
        self,
        host: str,
        port: int,
        token: str,
        session: aiohttp.ClientSession,
        *,
        backoff_min: float = 1.0,
        backoff_max: float = 30.0,
        heartbeat_interval: float = 30.0,
        pong_timeout: float = 10.0,
        request_timeout: float = 10.0,
        stable_after: float = 30.0,
        clock: Callable[[], float] = time.monotonic,
        rand: Callable[[], float] = random.random,
        sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
    ) -> None:
        self._url = f"ws://{host}:{port}/"
        self._token = token
        self._session = session
        self._backoff = (backoff_min, backoff_max)
        self._hb_interval = heartbeat_interval
        self._pong_timeout = pong_timeout
        self._req_timeout = request_timeout
        self._stable_after = stable_after  # a link must stay up this long to reset the backoff
        self._clock, self._rand, self._sleep = clock, rand, sleep
        self._ws: aiohttp.ClientWebSocketResponse | None = None
        self._task: asyncio.Task[None] | None = None
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._next_id = 0
        self._subs: list[PushCallback] = []
        self._state_subs: list[StateCallback] = []
        self._closed = False
        self.hub_id: str | None = None
        self.hub_name: str = ""  # hello_ack.name: the display name set from Home Assistant ("" = none / old firmware)
        self.hw_id: str | None = None  # hello_ack.hw_id: the hub's stable hardware id (survives renames)
        self.can_rename = False  # hello_ack.rename: the hub understands set_hub_id
        self.id_pinned = False  # hello_ack.id_pinned: hub.conf pins the id on the device
        self.fw_version: str | None = None
        self.activities: list[str] = []
        self.fw_build: int | None = None  # installed firmware build id (absent: hub without OTA)
        self.ota_key: str | None = None  # "dev" | "release" | None
        self.config_hash: str | None = None  # hub's CRC-32 hex of its loaded config text
        self.limits: HubLimits | None = None  # hello_ack.limits (LEGACY for a hub that sends none); None before the first hello

    @property
    def connected(self) -> bool:
        return self._ws is not None and not self._ws.closed

    # ------------------------------------------------------------ subscriptions
    def async_subscribe(self, cb: PushCallback) -> Callable[[], None]:
        """Receive unsolicited hub messages. Returns an unsubscribe function."""
        self._subs.append(cb)
        return lambda: self._subs.remove(cb) if cb in self._subs else None

    def async_subscribe_state(self, cb: StateCallback) -> Callable[[], None]:
        """Receive connection up/down notifications (and a terminal auth/version error)."""
        self._state_subs.append(cb)
        return lambda: self._state_subs.remove(cb) if cb in self._state_subs else None

    # ------------------------------------------------------------ lifecycle
    async def async_connect(self) -> None:
        """Connect and authenticate once (raises), then keep the link alive in the background."""
        if self._task is not None:
            raise HarmonyConnectionError("already connected")
        self._closed = False
        self._ws = await self._open()
        self._task = asyncio.create_task(self._run(self._ws), name="ha_harmony client")

    async def async_close(self) -> None:
        self._closed = True
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        ws, self._ws = self._ws, None
        if ws is not None and not ws.closed:
            with contextlib.suppress(aiohttp.ClientError, OSError, TimeoutError):
                await ws.close()
        self._fail_pending(HarmonyConnectionError("client closed"))

    async def async_send(self, msg_type: MsgType, data: dict[str, Any]) -> dict[str, Any]:
        """Send a request and return the reply's `d` (error replies raise)."""
        return await self._request(msg_type, data)

    async def async_set_identity(self, hub_id: str, name: str | None = None) -> dict[str, Any]:
        """Rename the hub (`set_hub_id`; the current id keeps it, `name` None keeps the stored name, "" clears
        it). Returns the hub_id_ack `d`; a refusal raises HarmonyCommandError (bad_id, bad_name, id_pinned, io)."""
        data: dict[str, Any] = {"id": hub_id}
        if name is not None:
            data["name"] = name
        ack = await self._request(MsgType.SET_HUB_ID, data)
        self.hub_id = ack["hub_id"]
        self.hub_name = ack.get("name") or ""
        return ack

    async def async_set_config(self, config: dict[str, Any]) -> dict[str, Any]:
        """Push a compiled config; returns the final config_ack `d`. A big config travels as
        config_chunk messages when the hub supports them (its hello_ack.limits.chunk)."""
        text = json.dumps(config, separators=(",", ":"), ensure_ascii=False)
        raw = text.encode()
        lim = self.limits
        if len(raw) <= SET_CONFIG_SINGLE_MAX or lim is None or not lim.chunk:
            return await self._request(MsgType.SET_CONFIG, {"config": config})
        step = min(CHUNK_FALLBACK, lim.chunk)
        # a load can take seconds on the hub's CPU: allow time in proportion to the text
        load_timeout = self._req_timeout + len(raw) / 20000
        seq, start, ack = 0, 0, {"ok": False}
        while start < len(text):
            end = min(len(text), start + step)  # characters; bytes may be more with non-ASCII
            while len(text[start:end].encode()) > step:
                end -= max(1, (len(text[start:end].encode()) - step) // 4)
            last = end >= len(text)
            ack = await self._request(
                MsgType.CONFIG_CHUNK,
                {"seq": seq, "total": len(raw), "data": text[start:end]},
                load_timeout if last else None,
            )
            if not ack.get("ok") or last:
                return ack
            seq, start = seq + 1, end
        return ack

    # ------------------------------------------------------------ internals
    async def _open(self) -> aiohttp.ClientWebSocketResponse:
        try:
            ws = await self._session.ws_connect(
                self._url, timeout=aiohttp.ClientWSTimeout(ws_close=5), autoping=True
            )
        except (aiohttp.ClientError, OSError, TimeoutError) as exc:
            raise HarmonyConnectionError(f"cannot connect to {self._url}: {exc}") from exc
        try:
            hello = {"token": self._token, "client": "home-assistant"}
            await ws.send_json(encode_ws(MsgType.HELLO, hello, 1))
            msg = await ws.receive(timeout=self._req_timeout)
            if msg.type != aiohttp.WSMsgType.TEXT:
                raise HarmonyConnectionError("hub closed during handshake")
            env = msg.json()
            self._check_version(env)
            try:
                _id, mtype, d = decode_ws_full(env)
            except ProtoError as exc:
                raise HarmonyConnectionError(f"bad handshake reply: {exc}") from exc
            if mtype is MsgType.ERROR:
                code = d.get("code")
                if code == "unauthorized":
                    raise HarmonyAuthError(d.get("message", "unauthorized"))
                if code == "version":
                    raise HarmonyVersionError(
                        f"protocol version mismatch (client {PROTO_VERSION}): {d.get('message')}"
                    )
                raise HarmonyConnectionError(f"hub refused hello: {code}")
            if mtype is not MsgType.HELLO_ACK:
                raise HarmonyConnectionError(f"unexpected handshake reply {mtype.name}")
            self.hub_id = d["hub_id"]
            self.hub_name = d.get("name") or ""
            self.hw_id = d.get("hw_id") or None
            self.can_rename = d.get("rename") is True
            self.id_pinned = d.get("id_pinned") is True
            self.fw_version = d.get("fw_version")
            self.fw_build = d.get("fw_build")
            self.ota_key = d.get("ota_key")
            self.config_hash = d.get("config_hash")
            self.limits = HubLimits.from_hello(d.get("limits"))
            acts = d.get("activities")
            self.activities = (
                [a for a in acts if isinstance(a, str)] if isinstance(acts, list) else []
            )
            self._next_id = 1
            return ws
        except (aiohttp.ClientError, OSError, TimeoutError, ValueError) as exc:
            await ws.close()
            raise HarmonyConnectionError(f"handshake failed: {exc}") from exc
        except BaseException:
            await ws.close()
            raise

    @staticmethod
    def _check_version(env: Any) -> None:
        if isinstance(env, dict) and env.get("v") != PROTO_VERSION:
            raise HarmonyVersionError(
                f"hub speaks protocol version {env.get('v')!r}, integration speaks {PROTO_VERSION}"
            )

    def _notify_state(self, connected: bool, err: HarmonyError | None = None) -> None:
        for cb in list(self._state_subs):
            try:
                cb(connected, err)
            except Exception:
                _LOGGER.exception("state callback failed")

    def _jitter(self, delay: float) -> float:
        """+-20 % so many clients (or restarts) never retry in lockstep."""
        return delay * (0.8 + 0.4 * self._rand())

    async def _run(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        """Serve, and when the link drops wait (exponential, jittered, capped), reconnect. The wait
        only restarts from its minimum after a link stayed up `stable_after` seconds: a hub that
        accepts the handshake and drops again is not a healthy hub."""
        delay = self._backoff[0]
        up_since = self._clock()
        while not self._closed:
            try:
                await self._serve(ws)
            except HarmonyVersionError as exc:
                await self._drop(ws)
                self._notify_state(False, exc)
                return
            except Exception:  # a bug must look like a dropped link, never a silent dead task
                _LOGGER.exception("hub connection handler failed; reconnecting")
            await self._drop(ws)
            self._notify_state(False)
            if self._clock() - up_since >= self._stable_after:
                delay = self._backoff[0]
            while True:
                await self._sleep(self._jitter(delay))
                try:
                    ws = await self._open()
                except (HarmonyAuthError, HarmonyVersionError) as exc:
                    self._notify_state(False, exc)
                    return
                except HarmonyConnectionError as exc:
                    _LOGGER.debug("reconnect failed: %s", exc)
                    delay = min(delay * 2, self._backoff[1])
                    continue
                except Exception:
                    _LOGGER.exception("unexpected error while reconnecting")
                    delay = min(delay * 2, self._backoff[1])
                    continue
                self._ws = ws
                up_since = self._clock()
                # the wait before the next attempt doubles unless this link proves stable
                delay = min(delay * 2, self._backoff[1])
                self._notify_state(True)
                break

    async def _drop(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        if self._ws is ws:
            self._ws = None
        self._fail_pending(HarmonyConnectionError("connection lost"))
        with contextlib.suppress(aiohttp.ClientError, OSError, TimeoutError):
            await ws.close()

    async def _serve(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        beat = asyncio.create_task(self._heartbeat(ws), name="ha_harmony heartbeat")
        try:
            async for msg in ws:
                if msg.type == aiohttp.WSMsgType.TEXT:
                    try:
                        env = msg.json()
                    except ValueError:
                        continue
                    self._check_version(env)
                    self._dispatch(env)
        finally:
            beat.cancel()
            await asyncio.gather(beat, return_exceptions=True)

    def _dispatch(self, env: Any) -> None:
        if isinstance(env, dict) and env.get("t") == "error" and isinstance(env.get("d"), dict):
            # Tolerate error replies missing fields: still fail the waiting request.
            rid = env.get("id")
            valid = isinstance(rid, int) and not isinstance(rid, bool) and rid
            fut = self._pending.pop(rid, None) if valid else None
            if fut is not None:
                if not fut.done():
                    d = env["d"]
                    fut.set_exception(
                        HarmonyCommandError(str(d.get("code", "error")), str(d.get("message", "")))
                    )
                return
        try:
            msg_id, mtype, data = decode_ws_full(env)
        except ProtoError as exc:
            _LOGGER.debug("dropping malformed message: %s", exc)
            return
        fut = self._pending.pop(msg_id, None) if msg_id else None
        if fut is not None:
            if fut.done():
                return
            if mtype is MsgType.ERROR:
                fut.set_exception(HarmonyCommandError(data["code"], data["message"]))
            else:
                fut.set_result(data)
            return
        for cb in list(self._subs):
            try:
                cb(mtype, data)
            except Exception:
                _LOGGER.exception("push callback failed")

    async def _heartbeat(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        while True:
            await asyncio.sleep(self._hb_interval)
            try:
                await asyncio.wait_for(self._request(MsgType.PING, {}), self._pong_timeout)
            except TimeoutError, HarmonyError:
                _LOGGER.debug("no pong from hub; dropping connection")
                await ws.close()
                return

    async def _request(
        self, msg_type: MsgType, data: dict[str, Any], timeout: float | None = None
    ) -> dict[str, Any]:
        ws = self._ws
        if ws is None or ws.closed:
            raise HarmonyConnectionError("not connected")
        self._next_id = self._next_id % 0xFFFFFFFF + 1
        msg_id = self._next_id
        envelope = encode_ws(msg_type, data, msg_id)
        fut: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
        self._pending[msg_id] = fut
        try:
            await ws.send_json(envelope)
            return await asyncio.wait_for(fut, timeout or self._req_timeout)
        except (aiohttp.ClientError, OSError) as exc:
            raise HarmonyConnectionError(str(exc)) from exc
        except TimeoutError as exc:
            raise HarmonyConnectionError("request timed out") from exc
        finally:
            self._pending.pop(msg_id, None)

    def _fail_pending(self, exc: Exception) -> None:
        pending, self._pending = self._pending, {}
        for fut in pending.values():
            if not fut.done():
                fut.set_exception(exc)
