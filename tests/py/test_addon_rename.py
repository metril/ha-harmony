"""A renamed hub: the add-on moves its store (draft, applied revisions, history, ack) to the new id when the
integration polls with the previous id and/or the stable hardware id. Never overwrites an existing store."""

import pytest
from addon_ws import connect, drain
from cfgapi import put_apply
from test_addon import CONFIG
from test_addon_linked import make

HW_A, HW_B = "aabbccddee01", "aabbccddee02"


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    return await aiohttp_client(make(tmp_path))


async def poll(c, hub, hw=None, prev=None, name=None):
    """Connect, say hello (with what the integration knows), disconnect. -> {adopt, frame}: the `adopt` answer
    (None when the add-on sent none) and the first other frame (`bundle` / `nothing_applied`)."""
    f = {k: v for k, v in (("hw", hw), ("prev", prev), ("name", name)) if v is not None}
    ws = await connect(c, hub, **f)
    try:
        frames = await drain(ws)
    finally:
        await ws.close()
    adopt = next((x["result"] for x in frames if x["t"] == "adopt"), None)
    return {"adopt": adopt, "frame": next(x for x in frames if x["t"] != "adopt")}


async def apply(c, hub, config=CONFIG):
    assert (await put_apply(c, config, hub)).status == 200


async def hubs(c):
    return await (await c.get("/api/v1/hubs")).json()


def ids(body):
    return sorted(h["id"] for h in body["hubs"])


async def two_applies(c, hub):
    """A store with applied and history."""
    await apply(c, hub)
    await apply(c, hub, {**CONFIG, "keys": {}})  # a second apply: the first goes to history


async def full(c, hub):
    return await (await c.get(f"/api/v1/hubs/{hub}")).json()


async def test_store_moves_with_the_previous_id_and_hardware_id(c):
    assert (await poll(c, "old-id", hw=HW_A))["frame"]["t"] == "nothing_applied"
    await two_applies(c, "old-id")
    before = await full(c, "old-id")
    assert before["applied"] and before["history"]
    r = await poll(c, "new-id", hw=HW_A, prev="old-id", name="Living room é")
    assert r["adopt"] == "moved" and r["frame"]["t"] == "bundle"
    assert r["frame"]["body"]["hub_id"] == "new-id"
    assert ids(await hubs(c)) == ["new-id"]
    after = await full(c, "new-id")
    assert (after["applied"], after["history"]) == (
        before["applied"],
        before["history"],
    )
    linked = (await hubs(c))["linked"]
    assert [(h["id"], h.get("name")) for h in linked] == [
        ("new-id", "Living room é")
    ]  # the old id is no longer listed


async def test_idempotent_and_the_header_is_only_on_renames(c):
    await apply(c, "old-id")
    await poll(c, "old-id", hw=HW_A)
    r = await poll(c, "new-id", hw=HW_A, prev="old-id")
    assert r["adopt"] == "moved"
    again = await poll(
        c, "new-id", hw=HW_A, prev="old-id"
    )  # the integration repeats it until it has an answer
    assert again["adopt"] == "none" and again["frame"]["t"] == "bundle"
    assert ids(await hubs(c)) == ["new-id"]
    assert (await poll(c, "new-id"))["adopt"] is None  # an ordinary hello says nothing about it


async def test_hardware_id_alone_finds_the_old_store(c):
    """No previous id (e.g. the hub was renamed on the device): the same hardware id is enough."""
    await poll(c, "old-id", hw=HW_A)
    await apply(c, "old-id")
    r = await poll(c, "new-id", hw=HW_A)
    assert r["adopt"] == "moved" and ids(await hubs(c)) == ["new-id"]


async def test_never_overwrites_an_existing_store(c):
    await poll(c, "old-id", hw=HW_A)
    await apply(c, "old-id")
    await apply(c, "new-id", {**CONFIG, "keys": {}})
    mine = await full(c, "old-id")
    theirs = await full(c, "new-id")
    r = await poll(c, "new-id", hw=HW_A, prev="old-id")
    assert r["adopt"] == "exists" and r["frame"]["t"] == "bundle"
    assert ids(await hubs(c)) == ["new-id", "old-id"]  # both stay
    assert await full(c, "old-id") == mine and await full(c, "new-id") == theirs


async def test_two_hubs_each_move_only_their_own_store(c):
    for hub, hw in (("hub-a", HW_A), ("hub-b", HW_B)):
        await poll(c, hub, hw=hw)
        await apply(c, hub)
    a_before, b_before = await full(c, "hub-a"), await full(c, "hub-b")
    # hub A renames; a claim of B's id as the previous one is refused by the hardware id
    r = await poll(c, "hub-a2", hw=HW_A, prev="hub-b")
    assert r["adopt"] == "moved"  # found hub-a through the hardware id, not hub-b through prev
    assert ids(await hubs(c)) == ["hub-a2", "hub-b"]
    assert await full(c, "hub-a2") == {**a_before, "hub": "hub-a2"}
    assert await full(c, "hub-b") == b_before
    r = await poll(
        c, "hub-b2", hw=HW_B, prev="hub-a2"
    )  # B renames but names A as previous: still moves B's
    assert r["adopt"] == "moved" and ids(await hubs(c)) == ["hub-a2", "hub-b2"]
    assert {h["id"] for h in (await hubs(c))["linked"]} == {"hub-a2", "hub-b2"}


async def test_a_previous_id_that_is_another_hub_is_not_moved(c):
    await poll(c, "hub-a", hw=HW_A)
    await apply(c, "hub-a")
    r = await poll(
        c, "hub-x", hw=HW_B, prev="hub-a"
    )  # hub-a is known to be a different piece of hardware
    assert r["adopt"] == "none" and ids(await hubs(c)) == ["hub-a"]


async def test_previous_id_without_a_known_hardware_id_is_accepted(c):
    """The integration was updated before the hub: the add-on never saw a hardware id for the old id."""
    await poll(c, "old-id")
    await apply(c, "old-id")
    r = await poll(c, "new-id", hw=HW_A, prev="old-id")
    assert r["adopt"] == "moved" and ids(await hubs(c)) == ["new-id"]


async def test_moving_back_restores_the_old_id(c):
    """The integration's rollback is the same call the other way round."""
    await poll(c, "old-id", hw=HW_A)
    await apply(c, "old-id")
    await poll(c, "new-id", hw=HW_A, prev="old-id")
    r = await poll(c, "old-id", hw=HW_A, prev="new-id")
    assert r["adopt"] == "moved" and ids(await hubs(c)) == ["old-id"]


async def test_rename_without_a_store_just_forgets_the_old_id(c):
    await poll(c, "old-id", hw=HW_A)
    assert [h["id"] for h in (await hubs(c))["linked"]] == ["old-id"]
    r = await poll(c, "new-id", hw=HW_A, prev="old-id")
    assert r["frame"]["t"] == "nothing_applied" and r["adopt"] == "none"
    assert [h["id"] for h in (await hubs(c))["linked"]] == ["new-id"]


async def test_garbage_identity_fields_are_ignored(c):
    await apply(c, "old-id")
    for bad in ({"hw": "not valid!"}, {"hw": "x" * 80}, {"prev": "../etc"}, {"prev": "default"}):
        r = await poll(c, "new-id", **bad)
        assert r["frame"]["t"] in ("bundle", "nothing_applied") and ids(await hubs(c)) == ["old-id"]
    r = await poll(
        c, "new-id", name="x" * 49
    )  # over 48 bytes: the name is dropped, the hello works
    assert [h.get("name") for h in (await hubs(c))["linked"]] == [None]
    assert (await poll(c, "new-id", name="tab\t"))["frame"]


async def test_names_and_hardware_ids_survive_a_restart(aiohttp_client, tmp_path):
    c1 = await aiohttp_client(make(tmp_path))
    await poll(c1, "old-id", hw=HW_A, name="Den")
    await apply(c1, "old-id")
    c2 = await aiohttp_client(make(tmp_path))  # same data dir
    assert [(h["id"], h.get("name")) for h in (await hubs(c2))["linked"]] == [("old-id", "Den")]
    r = await poll(c2, "new-id", hw=HW_A)  # the hardware id was remembered
    assert r["adopt"] == "moved"
    c3 = await aiohttp_client(make(tmp_path))
    assert [h["id"] for h in (await hubs(c3))["linked"]] == ["new-id"]


async def test_clearing_the_name(c):
    await poll(c, "h", name="Den")
    assert (await hubs(c))["linked"][0]["name"] == "Den"
    await poll(c, "h", name="")
    assert "name" not in (await hubs(c))["linked"][0]


async def test_adopt_pushes_the_configs_list(c):
    from addon_ws import live, recv

    await apply(c, "old-id")
    lv = await live(c, "configs")
    await recv(lv, "snapshot")
    r = await poll(c, "new-id", prev="old-id")
    assert r["adopt"] == "moved"
    msg = await recv(lv, "configs")
    assert "new-id" in str(msg) and "old-id" not in str([h["id"] for h in msg.get("hubs", [])])
    await lv.close()
