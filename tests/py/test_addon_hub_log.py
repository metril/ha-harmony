"""The add-on prints the hub's relayed log lines to its own log, tagged with the hub id. The integration is played by raw
channel frames; no hub, no Home Assistant."""

import logging

import pytest
from addon_ws import TOKEN, connect, drain, recv

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth

HUB = "LivingRoom"


@pytest.fixture
async def c(aiohttp_client, tmp_path):
    app = create_app(
        tmp_path, auth=NoAuth(), bundle_token=TOKEN, bundle_networks=None, hello_timeout=0.3
    )
    return await aiohttp_client(app)


async def test_hub_lines_reach_the_add_on_log_tagged_and_at_info(c, caplog):
    sock = await connect(c, HUB)
    await recv(sock, "nothing_applied")
    await drain(sock)
    with caplog.at_level(logging.INFO, logger="hub"):
        await sock.send_json({"t": "hub_log", "level": "info", "msg": "hub: build 42", "ts": 5})
        await sock.send_json(
            {
                "t": "hub_log",
                "level": "warning",
                "msg": "hub: RF send failed",
                "ts": 6,
                "dropped": 3,
            }
        )
        await drain(sock)  # everything sent before the junk frame has been handled
    recs = [r for r in caplog.records if r.name == f"hub.{HUB}"]
    assert [(r.levelno, r.getMessage()) for r in recs] == [
        (logging.INFO, "hub: build 42"),
        (logging.WARNING, "3 hub log line(s) were lost before this one (hub ring full)"),
        (logging.WARNING, "hub: RF send failed"),
    ]
    assert (
        logging.getLogger("hub").level == logging.INFO
    )  # the add-on's log_level option cannot hide them
    await sock.close()


async def test_bad_hub_log_frames_are_refused(c):
    sock = await connect(c, HUB)
    await recv(sock, "nothing_applied")
    for bad in (
        {"t": "hub_log", "level": "debug", "msg": "x"},
        {"t": "hub_log", "level": "info"},
        {"t": "hub_log", "level": "info", "msg": "x" * 401},
    ):
        await sock.send_json(bad)
        assert (await recv(sock))["code"] == "bad_frame", bad
    await sock.close()
