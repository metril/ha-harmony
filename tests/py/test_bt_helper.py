"""`hub bt-helper` without an adapter: the IPC channel reports state no_adapter and the helper exits
non-zero (never crashes, never hangs). Uses HCI index 15 so a real adapter on the test machine is
never touched. The wire format is documented in hub/bt/bt_ipc.h."""

import socket
import subprocess

import pytest

BT_IPC_STATE = 0x82
ST_NAMES = {0: "starting", 1: "ready", 2: "no_adapter", 3: "error"}


def _frames(data: bytes):
    out = []
    while len(data) >= 3:
        n = (data[1] << 8) | data[2]
        assert len(data) >= 3 + n, "truncated frame"
        out.append((data[0], data[3 : 3 + n]))
        data = data[3 + n :]
    assert not data, "trailing bytes"
    return out


def test_no_adapter_reports_state_and_exits(built_binaries, tmp_path):
    parent, child = socket.socketpair()
    fd = child.fileno()
    proc = subprocess.Popen(
        [
            str(built_binaries / "hub"),
            "bt-helper",
            "--hci",
            "15",
            "--fd",
            str(fd),
            "--keys",
            str(tmp_path / "bt" / "keys.tlv"),
        ],
        pass_fds=(fd,),
        stderr=subprocess.PIPE,
    )
    child.close()
    try:
        rc = proc.wait(timeout=10)
    finally:
        if proc.poll() is None:
            proc.kill()
    assert rc == 1, proc.stderr.read()
    parent.settimeout(2)
    data = b""
    while chunk := parent.recv(4096):  # EOF: the helper closed its end on exit
        data += chunk
    states = [ST_NAMES[p[0]] for t, p in _frames(data) if t == BT_IPC_STATE]
    assert states and states[-1] == "no_adapter", states
    last = [p for t, p in _frames(data) if t == BT_IPC_STATE][-1]
    assert last[2] == 0  # le = 0
    assert (last[3], last[4], last[5]) == (255, 4, 32)  # hosts_max, conn_max, remotes_max
    assert b"Segmentation" not in proc.stderr.read()


def test_bad_arguments_exit_2(built_binaries):
    res = subprocess.run(
        [str(built_binaries / "hub"), "bt-helper", "--bogus"],
        check=False,
        capture_output=True,
        timeout=10,
    )
    assert res.returncode == 2
    assert b"usage: hub bt-helper" in res.stderr


@pytest.mark.parametrize("hci", ["16", "-1"])
def test_bad_hci_index_exit_2(built_binaries, hci):
    res = subprocess.run(
        [str(built_binaries / "hub"), "bt-helper", "--hci", hci],
        check=False,
        capture_output=True,
        timeout=10,
    )
    assert res.returncode == 2
