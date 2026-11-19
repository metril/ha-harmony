"""The validators shared by the integration and the add-on (`harmony_core/bt_relay.py`): pure, nothing is trusted."""

import pytest

from harmony_core import bt_relay

LIMITS = {"hosts_max": 4, "conn_max": 2, "remotes_max": 2, "le_bonds_max": 8, "pages": [7, 12], "le": True}


def test_clean_state_bounds_and_drops_junk():
    raw = {
        "state": "ready",
        "hosts": [{"slot": i, "addr": f"AA:BB:CC:DD:{i // 256:02X}:{i % 256:02X}"} for i in range(300)],
        "candidates": [{"addr": f"AA:BB:CC:00:00:{i:02X}", "rssi": -40} for i in range(40)],
    }
    out = bt_relay.clean_state(raw)
    assert len(out["hosts"]) == 255 and len(out["candidates"]) == 16
    assert bt_relay.clean_state({"state": "nope"}) is None and bt_relay.clean_state([]) is None
    assert bt_relay.clean_state({"state": "ready", "hosts": "x", "remotes": [1, None]})["remotes"] == []
    named = bt_relay.clean_state({"state": "ready", "hosts": [{"slot": 0, "addr": "aa:bb:cc:dd:ee:01", "name": "a\x07"}]})
    assert named["hosts"][0]["addr"] == "AA:BB:CC:DD:EE:01" and "name" not in named["hosts"][0]


def test_clean_limits():
    assert bt_relay.clean_limits({"hosts_max": 999, "remotes_max": "x", "pages": [7, 9], "le": 1}) == {
        "hosts_max": 0, "conn_max": 0, "remotes_max": 0, "le_bonds_max": 0, "pages": [7], "le": False
    }
    assert bt_relay.clean_limits(None) is None


def test_gen_ordering_is_serial_arithmetic():
    assert bt_relay.newer_gen(5, 4) and bt_relay.newer_gen(5, 5) and not bt_relay.newer_gen(4, 5)
    assert bt_relay.newer_gen(1, 0xFFFFFFFF) and not bt_relay.newer_gen(0xFFFFFFFF, 1)


@pytest.mark.parametrize(
    "cmd",
    [
        {"op": "nope"},
        {"op": "pair", "secs": 301},
        {"op": "pair", "secs": True},
        {"op": "pair", "mode": "x"},
        {"op": "pair", "transport": "le", "evil": 1},
        {"op": "pick", "addr": "zz"},
        {"op": "forget", "kind": "host", "slot": 4},  # hosts_max 4
        {"op": "forget", "kind": "remote", "slot": 2},  # remotes_max 2
        {"op": "forget", "slot": -1},
        {"op": "key", "slot": 0, "page": 9, "usage": 1, "action": "press"},
        {"op": "key", "slot": 0, "page": 7, "usage": 70000, "action": "press"},
        {"op": "key", "slot": 9, "page": 7, "usage": 4, "action": "press"},
        {"op": "key", "slot": 0, "page": 7, "usage": 4, "action": "tap"},
        {"op": "listen", "on": True},
        "pair",
        None,
    ],
)
def test_check_command_refuses(cmd):
    with pytest.raises(bt_relay.RelayError):
        bt_relay.check_command(cmd, LIMITS)


def test_check_command_normalises():
    assert bt_relay.check_command({"op": "pick", "addr": "aa:bb:cc:dd:ee:ff"}, LIMITS) == {
        "op": "pick",
        "addr": "AA:BB:CC:DD:EE:FF",
    }
    assert bt_relay.check_command({"op": "pair"})["secs"] == 120
    assert bt_relay.check_command({"op": "forget", "slot": 3}, LIMITS) == {"op": "forget", "kind": "host", "slot": 3}
