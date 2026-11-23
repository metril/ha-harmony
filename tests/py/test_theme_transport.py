"""The custom theme on its way to the remote: theme_push before the pages of its generation, acked by the remote's
remote_info, redone when it does not show up, and the pairings with an older hub, remote or integration."""

import asyncio
import base64

import pytest
from test_ha_runtime import YAML, clock, rig, settle  # noqa: F401  (fixtures)

from custom_components.ha_harmony import const
from harmony_core.page_codec import decode_page

pytestmark = pytest.mark.usefixtures("enable_custom_integrations", "fast_client")

DUSK = "themes:\n  dusk: {extends: midnight, tokens: {ACC: '#FF8A3D'}}\ntheme: dusk\n"
USER = YAML + DUSK
USER2 = YAML + DUSK.replace("#FF8A3D", "#3366CC")
FULL = {
    "remote": 1,
    "page_fmt": 2,
    "caps": 15,
    "icons": 50,
    "gen": 0,
    "pages": 0,
    "boot": 1,
}  # all caps, empty store
MIDNIGHT = 1  # the base preset of the user theme


def info_of(c, caps=15):
    rp = c.compiled.for_caps(caps)
    return {**FULL, "caps": caps, "gen": rp.gen, "pages": len(rp.pages_v2)}


def sent(hub):
    """The order things reached the hub: ("theme", gen) / ("page", gen)."""
    out = []
    for t, d in hub.got:
        if t == "theme_push":
            out.append(("theme", d["gen"]))
        elif t == "page_push":
            out.append(("page", decode_page(base64.b64decode(d["data"])).gen))
    return out


async def idle(hass, c):
    await settle(hass, lambda: all(lk.task is None or lk.task.done() for lk in c.links.values()))


async def start(hass, rig, yaml=USER, info=FULL):  # noqa: F811
    hub, c = await rig(yaml, pages=False)
    await hub.push("remote_attached", {})
    await hub.push("remote_info", info)
    return hub, c


async def test_theme_goes_before_its_pages_and_the_remote_acks(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig)
    rp = c.compiled.for_caps(15)
    n = len(rp.pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    assert sent(hub) == [("theme", rp.gen)] + [("page", rp.gen)] * n
    assert hub.themes() == [(rp.gen, c.compiled.theme_wire)] and len(c.compiled.theme_wire) == 99
    assert decode_page(c.compiled.pages_v2[0]).theme == 7 == rp.models[0].theme
    st = c.theme_status(1)
    assert st == {
        "name": "dusk",
        "id": 7,
        "sent": True,
        "acked": False,
    }  # the hub took it; the remote has not said
    await hub.push("remote_info", info_of(c))  # complete set held: pages and theme
    await idle(hass, c)
    assert c.links[1].pushed is not None and c.theme_status(1)["acked"] is True
    assert len(hub.themes()) == 1


async def test_a_theme_the_remote_never_confirms_is_sent_once_more(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig)
    n = len(c.compiled.for_caps(15).pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    await settle(hass, clock.active)
    clock.elapse()  # THEME_ACK_WAIT passes with no remote_info: the hub lost it
    await settle(hass, lambda: len(sent(hub)) == 2 * (1 + n))
    assert [k for k, _ in sent(hub)] == (["theme"] + ["page"] * n) * 2
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    assert c.links[1].pushed is not None and len(hub.themes()) == 2


async def test_a_newer_config_cancels_a_running_theme_push(hass, rig, clock, fake_addon):  # noqa: F811
    hub, c = await rig(USER, pages=False)
    hub.gate = asyncio.Event()  # replies are held: the theme is in flight
    await hub.push("remote_attached", {})
    await hub.push("remote_info", FULL)
    await settle(hass, lambda: len(sent(hub)) == 1)
    old = sent(hub)[0][1]
    fake_addon.apply_yaml(USER2)
    await c.async_refresh_from_addon()
    hub.gate.set()
    rp = c.compiled.for_caps(15)
    assert rp.gen != old
    await settle(hass, lambda: ("page", rp.gen) in sent(hub))
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    order = sent(hub)
    first_new = order.index(("theme", rp.gen))
    assert all(g == rp.gen for _, g in order[first_new:]), order  # the old set never resumes
    assert order[first_new + 1 :] == [("page", rp.gen)] * len(
        rp.pages_v2
    )  # the new theme first, then its pages
    assert hub.themes()[-1] == (rp.gen, c.compiled.theme_wire)


async def test_old_remote_without_bit_3_gets_no_theme(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig, info={**FULL, "caps": 7})
    n = len(c.compiled.for_caps(7).pages_v2)
    await settle(hass, lambda: len(hub.pages()) == n)
    assert hub.themes() == [] and {p.theme for p in hub.pages()} == {MIDNIGHT}
    assert c.theme_status(1) == {"name": "dusk", "id": 7, "sent": False, "acked": False}


async def test_a_preset_set_needs_no_theme_push(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig, yaml=YAML + "theme: midnight\n")
    n = len(c.compiled.for_caps(15).pages_v2)
    await settle(hass, lambda: len(hub.pages()) == n)
    assert hub.themes() == [] and c.theme_status(1)["sent"] is False
    await idle(hass, c)


async def test_a_remote_that_holds_the_set_is_sent_nothing(hass, rig, clock):  # noqa: F811
    hub, c = await rig(USER, pages=False)
    await hub.push("remote_attached", {})
    await hub.push(
        "remote_info", info_of(c)
    )  # a hub restart: the remote still holds pages and theme
    await idle(hass, c)
    await asyncio.sleep(0.05)
    assert sent(hub) == [] and c.theme_status(1)["acked"] is True


async def test_reattach_sends_the_theme_again(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig)
    n = len(c.compiled.for_caps(15).pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    await hub.push("remote_attached", {})  # the remote rebooted: its slot and store are empty
    await hub.push("remote_info", FULL)
    await settle(hass, lambda: len(sent(hub)) == 2 * (1 + n))
    assert [k for k, _ in sent(hub)] == (["theme"] + ["page"] * n) * 2
    assert len(hub.themes()) == 2 and hub.themes()[0] == hub.themes()[1]
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)


async def test_a_caps_change_that_adds_bit_3_sends_the_theme(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig, info={**FULL, "caps": 7})
    n7 = len(c.compiled.for_caps(7).pages_v2)
    await settle(hass, lambda: len(hub.pages()) == n7)
    await idle(hass, c)
    await hub.push("remote_info", {**FULL, "caps": 15})  # firmware update
    rp = c.compiled.for_caps(15)
    await settle(hass, lambda: ("theme", rp.gen) in sent(hub))
    await settle(hass, lambda: len(hub.pages()) == n7 + len(rp.pages_v2))
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    assert c.theme_status(1)["acked"] is True


# ------------------------------------------------------------------ a set the remote never confirms
async def drain(hass, clk, until):
    """Release the manual clk until `until()` holds (a signal, never a real sleep)."""
    for _ in range(40):
        if until():
            return
        await settle(hass, lambda: clk.active() or until())
        if until():
            return
        clk.elapse()
    await settle(hass, until)


def retrying(link):
    return link.retry_task is not None and not link.retry_task.done()


async def test_an_unconfirmed_theme_set_is_not_marked_pushed_and_is_retried_with_backoff(
    hass,
    rig,  # noqa: F811
    clock,  # noqa: F811
):
    hub, c = await start(hass, rig)
    link = c.links[1]
    n = len(c.compiled.for_caps(15).pages_v2)
    one = 1 + n
    await settle(hass, lambda: len(sent(hub)) == one)
    await settle(hass, clock.active)
    clock.elapse()  # first ack wait over: sent once more
    await settle(hass, lambda: len(sent(hub)) == 2 * one)
    await settle(hass, clock.active)
    clock.elapse()  # second ack wait over, still nothing from the remote
    await settle(hass, lambda: retrying(link))
    assert link.pushed is None and link.pending is True  # never "pushed" without the remote's word
    await settle(hass, clock.active)
    assert clock.waits[-1] == const.PUSH_RETRY_BASE
    clock.elapse()  # the retry is due
    await settle(hass, lambda: len(sent(hub)) == 3 * one)
    await settle(hass, clock.active)
    clock.elapse()  # the retry's own ack wait
    await settle(hass, lambda: len(sent(hub)) == 4 * one)
    await settle(hass, clock.active)
    clock.elapse()
    await settle(hass, lambda: retrying(link))
    await settle(hass, clock.active)
    assert clock.waits[-1] == 2 * const.PUSH_RETRY_BASE  # backing off
    await hub.push("remote_info", info_of(c))  # the remote finally reports the whole set
    await settle(hass, lambda: link.pushed is not None and link.retries == 0)
    await idle(hass, c)
    assert c.theme_status(1)["acked"] is True


async def test_backoff_is_bounded(hass, rig, clock, monkeypatch):  # noqa: F811
    monkeypatch.setattr(const, "THEME_ACK_WAIT", 1.0)
    _hub, c = await start(hass, rig)
    link = c.links[1]
    for _ in range(12):  # many rounds with a remote that never confirms
        await drain(hass, clock, lambda: retrying(link))
        await settle(hass, clock.active)
        clock.elapse()
    assert const.PUSH_RETRY_MAX in clock.waits
    assert max(w for w in clock.waits if w != 1.0 and w != const.INFO_WAIT) <= const.PUSH_RETRY_MAX


async def test_the_hub_giving_up_on_the_theme_while_the_remote_stays_attached(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig)
    link = c.links[1]
    rp = c.compiled.for_caps(15)
    n = len(rp.pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    # the hub dropped the theme (resends exhausted): the remote holds the pages but no theme and no
    # link loss is ever seen
    await hub.push("remote_info", {**FULL, "gen": rp.gen, "pages": n - 1})
    await drain(hass, clock, lambda: len(sent(hub)) >= 3 * (1 + n))
    await hub.push("remote_info", info_of(c))
    await settle(hass, lambda: link.pushed is not None)
    await idle(hass, c)
    assert c.theme_status(1)["acked"] is True


async def test_a_remote_reboot_the_hub_never_saw_is_pushed_again(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig)
    link = c.links[1]
    n = len(c.compiled.for_caps(15).pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    assert link.pushed is not None
    await hub.push(
        "remote_info", FULL
    )  # it rebooted and says its store is empty; no remote_attached
    await settle(hass, lambda: len(sent(hub)) == 2 * (1 + n))  # at once the first time
    assert [k for k, _ in sent(hub)] == (["theme"] + ["page"] * n) * 2
    await hub.push("remote_info", info_of(c))
    await settle(hass, lambda: link.pushed is not None and link.retries == 0)
    await idle(hass, c)


async def test_repeated_reboots_back_off_when_nothing_confirms_the_set(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig, yaml=YAML + "theme: midnight\n", info={**FULL, "caps": 7})
    link = c.links[1]
    n = len(c.compiled.for_caps(7).pages_v2)
    await settle(hass, lambda: link.pushed is not None and len(hub.pages()) == n)
    await idle(hass, c)
    await hub.push("remote_info", {**FULL, "caps": 7})  # store empty although we pushed: at once
    await settle(hass, lambda: len(hub.pages()) == 2 * n)
    await idle(hass, c)
    await hub.push("remote_info", {**FULL, "caps": 7})  # still nothing held: now it waits
    await settle(hass, lambda: retrying(link))
    await settle(hass, clock.active)
    assert len(hub.pages()) == 2 * n and clock.waits[-1] == 2 * const.PUSH_RETRY_BASE
    clock.elapse()
    await settle(hass, lambda: len(hub.pages()) == 3 * n)


async def test_a_refused_theme_holds_the_pages_back_and_recovers(hass, rig, clock):  # noqa: F811
    hub, c = await rig(USER, pages=False)
    hub.refuse["theme_push"] = 1  # the hub's link queue was full
    await hub.push("remote_attached", {})
    await hub.push("remote_info", FULL)
    link = c.links[1]
    await settle(hass, lambda: retrying(link))
    assert hub.pages() == []  # no page without its theme
    assert link.pushed is None and link.pending is True
    await settle(hass, clock.active)
    clock.elapse()
    rp = c.compiled.for_caps(15)
    await settle(hass, lambda: len(hub.pages()) == len(rp.pages_v2))
    assert [k for k, _ in sent(hub)] == ["theme", "theme"] + ["page"] * len(
        rp.pages_v2
    )  # the refused one counts
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    assert link.pushed is not None and c.theme_status(1)["acked"] is True


async def test_a_refused_page_is_retried_not_marked_pushed(hass, rig, clock):  # noqa: F811
    hub, c = await rig(YAML + "theme: midnight\n", pages=False)
    hub.refuse["page_push"] = 1
    await hub.push("remote_attached", {})
    await hub.push("remote_info", {**FULL, "caps": 7})
    link = c.links[1]
    await settle(hass, lambda: retrying(link))
    assert link.pushed is None
    await settle(hass, clock.active)
    clock.elapse()
    await settle(hass, lambda: link.pushed is not None)


async def test_unload_cancels_a_pending_retry(hass, rig, clock):  # noqa: F811
    hub, c = await rig(USER, pages=False)
    hub.refuse["theme_push"] = 1
    await hub.push("remote_attached", {})
    await hub.push("remote_info", FULL)
    link = c.links[1]
    await settle(hass, lambda: retrying(link))
    task = link.retry_task
    await c.async_shutdown()
    await settle(hass, task.done)
    assert task.cancelled()


async def test_tiles_are_resent_after_the_pages_of_an_ack_timeout_resend(hass, rig, clock):  # noqa: F811
    hass.states.async_set("sensor.t", "21", {"unit_of_measurement": "C"})
    hub, c = await start(hass, rig)
    n = len(c.compiled.for_caps(15).pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    hass.states.async_set("sensor.t", "22", {"unit_of_measurement": "C"})  # a live update mid-push
    await settle(hass, clock.active)
    clock.elapse()  # the re-send: the remote's page store drops the tile values it had
    await settle(hass, lambda: len(sent(hub)) == 2 * (1 + n))
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    last_page = max(i for i, (t, _) in enumerate(hub.got) if t == "page_push")
    after = [
        d["args"]
        for t, d in hub.got[last_page + 1 :]
        if t == "command" and d["command"] == "tile_update"
    ]
    assert any(a["text"] == "22 C" for a in after)  # every tile value again, after the final page


async def test_a_slow_delivery_that_keeps_progressing_is_never_pushed_again(hass, rig, clock):  # noqa: F811
    hub, c = await start(hass, rig)
    rp = c.compiled.for_caps(15)
    n = len(rp.pages_v2)
    await settle(hass, lambda: len(sent(hub)) == 1 + n)
    await settle(hass, clock.active)
    want = const.THEME_ACK_WAIT + const.THEME_ACK_PER_PAGE * n
    assert clock.waits[-1] == want  # the window scales with the set
    for k in range(1, n):  # the remote stores page after page; each remote_info restarts the window
        await hub.push("remote_info", {**FULL, "caps": 15, "gen": rp.gen, "pages": k})
        await settle(hass, lambda k=k: clock.waits.count(want) >= 1 + k)
    assert len(sent(hub)) == 1 + n  # nothing was sent a second time
    await hub.push("remote_info", info_of(c))
    await idle(hass, c)
    assert c.links[1].pushed is not None and len(sent(hub)) == 1 + n
