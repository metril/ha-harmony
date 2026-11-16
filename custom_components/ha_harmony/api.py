"""WebSocket client for the Harmony hub: request/reply matching, push fan-out, reconnect."""

from __future__ import annotations

import asyncio
import contextlib
import logging
from collections.abc import Callable
from typing import Any

import aiohttp

from .harmony_proto import PROTO_VERSION, MsgType, ProtoError, decode_ws_full, encode_ws

_LOGGER = logging.getLogger(__name__)

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
    ) -> None:
        self._url = f"ws://{host}:{port}/"
        self._token = token
        self._session = session
        self._backoff = (backoff_min, backoff_max)
        self._hb_interval = heartbeat_interval
        self._pong_timeout = pong_timeout
        self._req_timeout = request_timeout
        self._ws: aiohttp.ClientWebSocketResponse | None = None
        self._task: asyncio.Task[None] | None = None
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._next_id = 0
        self._subs: list[PushCallback] = []
        self._state_subs: list[StateCallback] = []
        self._closed = False
        self.hub_id: str | None = None
        self.fw_version: str | None = None
        self.activities: list[str] = []

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
            self.fw_version = d.get("fw_version")
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

    async def _run(self, ws: aiohttp.ClientWebSocketResponse) -> None:
        delay = self._backoff[0]
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
            while True:
                await asyncio.sleep(delay)
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
                delay = self._backoff[0]
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

    async def _request(self, msg_type: MsgType, data: dict[str, Any]) -> dict[str, Any]:
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
            return await asyncio.wait_for(fut, self._req_timeout)
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
