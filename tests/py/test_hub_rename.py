"""set_hub_id against the real `hub --sim` daemon: persistence across restarts, precedence of the pin, hello_ack."""

import aiohttp
import pytest
from conftest import TOKEN

HW = "aabbccddeeff"


@pytest.fixture
def ident_dir(tmp_path):
    d = tmp_path / "ident"
    d.mkdir()
    return d


async def call(hub, t, d, *, expect_ack=True):
    """hello, then one request; returns (hello_ack d, reply envelope)."""
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "hello", "d": {"token": TOKEN}})
        ack = await ws.receive_json()
        assert ack["t"] == "hello_ack"
        await ws.receive_json()  # state_update
        await ws.send_json({"v": 1, "id": 2, "t": t, "d": d})
        return ack["d"], await ws.receive_json()


async def hello(hub):
    ack, _ = await call(hub, "ping", {})
    return ack


async def test_hello_ack_reports_identity_support(start_hub, ident_dir):
    hub = start_hub("--ident-dir", str(ident_dir), "--hw-id", HW)
    ack = await hello(hub)
    assert ack["hub_id"] == "sim-hub" and ack["hw_id"] == HW and ack["rename"] is True
    assert "name" not in ack and "id_pinned" not in ack


async def test_set_hub_id_applies_at_once_and_survives_a_restart(start_hub, ident_dir):
    hub = start_hub("--ident-dir", str(ident_dir), "--hw-id", HW)
    _, rep = await call(hub, "set_hub_id", {"id": "living-room", "name": "Living room é"})
    assert rep["t"] == "hub_id_ack" and rep["id"] == 2
    assert rep["d"] == {"hub_id": "living-room", "name": "Living room é"}
    ack = await hello(hub)
    assert (ack["hub_id"], ack["name"], ack["hw_id"]) == ("living-room", "Living room é", HW)
    assert (ident_dir / "hub_id").read_text() == "living-room"
    assert not list(ident_dir.glob("*.tmp*"))  # atomic: no temp file left behind
    hub.stop()
    again = start_hub(
        "--ident-dir", str(ident_dir), "--hw-id", HW
    )  # the daemon's own file wins over the default
    ack = await hello(again)
    assert (ack["hub_id"], ack["name"]) == ("living-room", "Living room é")


async def test_name_only_and_clearing(start_hub, ident_dir):
    hub = start_hub("--ident-dir", str(ident_dir))
    _, rep = await call(hub, "set_hub_id", {"id": "sim-hub", "name": "Den"})
    assert rep["d"] == {"hub_id": "sim-hub", "name": "Den"}
    _, rep = await call(hub, "set_hub_id", {"id": "sim-hub"})  # name omitted: kept
    assert rep["d"]["name"] == "Den"
    _, rep = await call(hub, "set_hub_id", {"id": "sim-hub", "name": ""})  # cleared
    assert rep["d"] == {"hub_id": "sim-hub"}
    assert "name" not in await hello(hub)


@pytest.mark.parametrize(
    ("data", "code"),
    [
        ({"id": "default"}, "bad_id"),
        ({"id": "has space"}, "bad_id"),
        ({"id": "a.b"}, "bad_id"),
        ({"id": "-leading"}, "bad_id"),
        ({"id": "x" * 64}, "bad_id"),
        ({"id": "ok", "name": "n" * 49}, "bad_name"),
        ({"id": "ok", "name": "tab\t"}, "bad_name"),
    ],
)
async def test_invalid_input_is_refused_and_changes_nothing(start_hub, ident_dir, data, code):
    hub = start_hub("--ident-dir", str(ident_dir))
    _, rep = await call(hub, "set_hub_id", data)
    assert rep["t"] == "error" and rep["d"]["code"] == code and rep["id"] == 2
    assert (await hello(hub))["hub_id"] == "sim-hub"
    assert not (ident_dir / "hub_id").exists()


async def test_hub_conf_pin_refuses_the_rename(start_hub, ident_dir):
    hub = start_hub(
        "--ident-dir", str(ident_dir), "--hub-id", "pinned-hub"
    )  # hub.conf `hub_id` -> --hub-id
    ack = await hello(hub)
    assert ack["hub_id"] == "pinned-hub" and ack["id_pinned"] is True
    _, rep = await call(hub, "set_hub_id", {"id": "other"})
    assert (
        rep["t"] == "error"
        and rep["d"]["code"] == "id_pinned"
        and "hub.conf" in rep["d"]["message"]
    )
    assert (await hello(hub))["hub_id"] == "pinned-hub"
    assert not (ident_dir / "hub_id").exists()
    _, rep = await call(
        hub, "set_hub_id", {"id": "pinned-hub", "name": "Pinned"}
    )  # same id + name is fine
    assert rep["t"] == "hub_id_ack"


async def test_the_flag_beats_a_persisted_file(start_hub, ident_dir):
    (ident_dir / "hub_id").write_text("from-file\n")
    assert (await hello(start_hub("--ident-dir", str(ident_dir))))["hub_id"] == "from-file"
    flagged = start_hub("--ident-dir", str(ident_dir), "--hub-id", "from-flag")
    assert (await hello(flagged))["hub_id"] == "from-flag"
    assert (ident_dir / "hub_id").read_text() == "from-file\n"  # never rewritten


async def test_requires_authentication(start_hub, ident_dir):
    hub = start_hub("--ident-dir", str(ident_dir))
    async with aiohttp.ClientSession() as s, s.ws_connect(hub.url) as ws:
        await ws.send_json({"v": 1, "id": 1, "t": "set_hub_id", "d": {"id": "x"}})
        rep = await ws.receive_json()
        assert rep["t"] == "error" and rep["d"]["code"] == "unauthorized"
