"""App rings on the remote: each app button shows whether its app is open, playing or paused.

Push only: a state change of an activity's `media.apps_entity` (an androidtv player) recomputes the
status of the buttons bound to it and sends the ones that changed as `tile_update` with `state`
(addressed to the button's widget id, text empty). There is no timer anywhere.

Status of a button: its app_id equals the player's `app_id` attribute -> playing 2, paused 3, any
other state 1 (open); else 0. An app_id the table does not know and no button binds shows no ring
and is logged once.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Callable, Iterable
from typing import Any

from homeassistant.core import Event, HomeAssistant, State, callback
from homeassistant.helpers.event import EventStateChangedData, async_track_state_change_event

from .api import HarmonyCommandError, HarmonyConnectionError
from .const import CMD_TILE_UPDATE
from .harmony_core.config_model import KNOWN_APP_IDS
from .harmony_core.harmony_proto import MsgType
from .harmony_core.page_build import AppBinding

_LOGGER = logging.getLogger(__name__)

HUB_DEVICE = "hub"
NONE, OPEN, PLAYING, PAUSED = 0, 1, 2, 3
_OFF_STATES = ("off", "unavailable", "unknown", "standby")


def status(binding: AppBinding, player: State | None, running: str | None = None) -> int:
    """The ring of one button for the player's current state, while its activity runs."""
    if binding.activity is not None and running != binding.activity:
        return NONE
    if player is None or player.state in _OFF_STATES:
        return NONE
    if player.attributes.get("app_id") not in binding.app_ids:
        return NONE
    return {"playing": PLAYING, "paused": PAUSED}.get(player.state, OPEN)


class AppStatusFeed:
    """Keeps the app rings of the attached remotes in step with the apps_entity players."""

    def __init__(
        self,
        hass: HomeAssistant,
        client: Any,
        remotes: Callable[[], Iterable[int]],
        ready: Callable[[int], bool],
        spawn: Callable[[Awaitable[None], str], asyncio.Task[None]],
        resolve: Callable[[str], str] = lambda e: e,
        running: Callable[[], str | None] = lambda: None,
    ) -> None:
        self.hass, self.client = hass, client
        self._remotes, self._ready, self._spawn, self._resolve = remotes, ready, spawn, resolve
        self._running = running
        self._bindings: tuple[AppBinding, ...] = ()
        self._sent: dict[
            tuple[int, int, int], int
        ] = {}  # (remote, page, widget) -> status on the remote
        self._unsub: Callable[[], None] | None = None
        self._task: asyncio.Task[None] | None = None
        self._dirty = False
        self._unknown: set[str] = set()
        self.sent = 0

    # -- lifecycle (events only: nothing here schedules a timer)
    def configure(self, bindings: Iterable[AppBinding]) -> None:
        """A new config went live: track exactly its apps_entity players; every ring is resent."""
        self.stop()
        self._bindings = tuple(bindings)
        self._sent.clear()
        entities = sorted({self._resolve(b.entity_id) for b in self._bindings})
        if entities:
            self._unsub = async_track_state_change_event(self.hass, entities, self._on_event)
            self._on_event(None)  # type: ignore[arg-type]  # every remote gets every ring, 0 included

    def stop(self) -> None:
        if self._unsub:
            self._unsub()
            self._unsub = None
        if self._task is not None and not self._task.done():
            self._task.cancel()
        self._task = None

    @callback
    def _on_event(self, _event: Event[EventStateChangedData]) -> None:
        self._dirty = True
        if self._task is None or self._task.done():
            self._task = self._spawn(self._run(), "ha_harmony app status")

    def refresh(self) -> None:
        """The running activity changed: rings of the other activities clear."""
        if self._bindings:
            self._on_event(None)  # type: ignore[arg-type]

    def on_remote_attached(self, remote: int) -> None:
        """A remote (re)joined or got its pages again: it holds no ring, send them all."""
        for key in [k for k in self._sent if k[0] == remote]:
            del self._sent[key]
        if self._bindings:
            self._spawn(self._push((remote,)), "ha_harmony app status attach")

    # -- work
    async def _run(self) -> None:
        while self._dirty:
            self._dirty = False
            await self._push(tuple(self._remotes()))

    def _note_unknown(self, player: State | None) -> None:
        app = player.attributes.get("app_id") if player is not None else None
        known = KNOWN_APP_IDS.union(*(b.app_ids for b in self._bindings))
        if isinstance(app, str) and app and app not in known and app not in self._unknown:
            self._unknown.add(app)
            _LOGGER.info("app %s is not in the app table: no status ring for it", app)

    async def _push(self, remotes: tuple[int, ...]) -> None:
        running = self._running()
        for b in self._bindings:
            self._note_unknown(self.hass.states.get(self._resolve(b.entity_id)))
        for remote in remotes:
            if not self._ready(remote) or not self.client.connected:
                continue
            for b in self._bindings:
                want = status(b, self.hass.states.get(self._resolve(b.entity_id)), running)
                key = (remote, b.page, b.widget)
                if self._sent.get(key) == want:
                    continue
                args = {
                    "remote": remote,
                    "page": b.page,
                    "tile": b.widget,
                    "text": "",
                    "state": want,
                }
                try:
                    await self.client.async_send(
                        MsgType.COMMAND,
                        {"device": HUB_DEVICE, "command": CMD_TILE_UPDATE, "args": args},
                    )
                except (HarmonyCommandError, HarmonyConnectionError) as exc:
                    _LOGGER.debug("app status for %s not delivered: %s", b.entity_id, exc)
                    break
                self._sent[key] = want
                self.sent += 1
