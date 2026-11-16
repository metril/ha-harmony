"""mDNS responder: real hub binary on loopback (unicast, ephemeral ports, no multicast),
queries built with the zeroconf package's DNS classes, replies parsed with DNSIncoming."""

import re
import socket
import time

import pytest
from conftest import HUB_ID
from homeassistant import config_entries
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
from zeroconf import (
    DNSAddress,
    DNSIncoming,
    DNSOutgoing,
    DNSPointer,
    DNSQuestion,
    DNSService,
    DNSText,
    IPVersion,
    ServiceInfo,
    const,
)

from custom_components.ha_harmony.const import DOMAIN

SVC = "_haharmony._tcp.local."
INST = f"{HUB_ID}.{SVC}"
HOST = f"{HUB_ID}.local."
ENUM = "_services._dns-sd._udp.local."


class MdnsHub:
    def __init__(self, hub, port, sink):
        self.hub, self.port, self.sink = hub, port, sink

    def ask(self, name, qtype, qu=False, timeout=3.0):
        """Send one query from an ephemeral port; return the parsed reply."""
        out = DNSOutgoing(0)
        out.add_question(
            DNSQuestion(name, qtype, const._CLASS_IN | (const._CLASS_UNIQUE if qu else 0))
        )
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(timeout)
            s.sendto(out.packets()[0], ("127.0.0.1", self.port))
            data, _ = s.recvfrom(9000)
        return DNSIncoming(data)

    def announcement(self, timeout=3.0):
        self.sink.settimeout(timeout)
        data, _ = self.sink.recvfrom(9000)
        return time.monotonic(), DNSIncoming(data)


@pytest.fixture
def mdns(start_hub):
    sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sink.bind(("127.0.0.1", 0))
    dest = f"127.0.0.1:{sink.getsockname()[1]}"
    hub = start_hub("--mdns", "--mdns-addr", "127.0.0.1", "--mdns-port", "0", "--mdns-dest", dest)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        m = re.search(rb"mdns port=(\d+)\n", hub.out_path.read_bytes())
        if m:
            break
        time.sleep(0.01)
    else:
        raise AssertionError("hub did not report an mdns port")
    yield MdnsHub(hub, int(m.group(1)), sink)
    sink.close()


def by_type(msg, cls):
    return [r for r in msg.answers() if isinstance(r, cls)]


def test_announcements_then_goodbye(mdns):
    t1, first = mdns.announcement()
    t2, second = mdns.announcement()
    assert 0.8 <= t2 - t1 < 2.0  # two sends, ~1 s apart
    for msg in (first, second):
        recs = msg.answers()
        assert {type(r) for r in recs} == {DNSPointer, DNSService, DNSText, DNSAddress}
        assert len(recs) == 5  # incl. the services-enumeration PTR
        ptr = next(r for r in by_type(msg, DNSPointer) if r.name == SVC)
        assert ptr.alias == INST and ptr.ttl == 4500 and not ptr.unique
        srv = by_type(msg, DNSService)[0]
        assert srv.unique and srv.ttl == 120 and srv.port == mdns.hub.port
        assert by_type(msg, DNSText)[0].ttl == 4500
        addr = by_type(msg, DNSAddress)[0]
        assert addr.address == bytes([127, 0, 0, 1]) and addr.ttl == 120
    with pytest.raises(TimeoutError):  # nothing further unsolicited
        mdns.announcement(timeout=0.3)
    mdns.hub.stop()
    _, bye = mdns.announcement()
    assert len(bye.answers()) == 5 and all(r.ttl == 0 for r in bye.answers())


def test_ptr_query_answers_full_set(mdns):
    msg = mdns.ask(SVC, const._TYPE_PTR)
    assert msg.is_response()
    ptr = by_type(msg, DNSPointer)[0]
    assert ptr.name == SVC and ptr.alias == INST
    assert {type(r) for r in msg.answers()} == {DNSPointer, DNSService, DNSText, DNSAddress}
    assert all(0 < r.ttl <= 10 for r in msg.answers())  # legacy unicast (src port != 5353): TTL cap


def test_enum_srv_txt_a_queries(mdns):
    enum = mdns.ask(ENUM, const._TYPE_PTR)
    assert [(r.name, r.alias) for r in by_type(enum, DNSPointer)] == [(ENUM, SVC)]
    srv = mdns.ask(INST, const._TYPE_SRV)
    s = by_type(srv, DNSService)[0]
    assert s.server == HOST and s.port == mdns.hub.port and by_type(srv, DNSAddress)
    txt = by_type(mdns.ask(INST, const._TYPE_TXT), DNSText)[0]
    assert txt.text == b"\x0ehub_id=sim-hub\x03v=1"
    a = by_type(mdns.ask(HOST, const._TYPE_A), DNSAddress)[0]
    assert a.name == HOST and a.address == bytes([127, 0, 0, 1])


def test_unrelated_query_gets_no_reply(mdns):
    with pytest.raises(TimeoutError):
        mdns.ask("other.local.", const._TYPE_A, timeout=0.3)
    # still alive and answering afterwards
    assert by_type(mdns.ask(SVC, const._TYPE_PTR), DNSPointer)


def test_garbage_does_not_kill_hub(mdns):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        for pkt in (
            b"",
            b"\x00" * 5,
            b"\xff" * 300,
            b"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\xc0\x0c\x00\x01\x00\x01",
        ):
            s.sendto(pkt, ("127.0.0.1", mdns.port))
    assert by_type(mdns.ask(SVC, const._TYPE_PTR), DNSPointer)
    assert mdns.hub.alive()


def test_default_is_off_under_sim(start_hub):
    hub = start_hub()
    assert b"mdns port" not in hub.out_path.read_bytes() and hub.alive()


def test_bind_failure_is_soft(start_hub):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as blocker:  # no SO_REUSEADDR: bind fails
        blocker.bind(("127.0.0.1", 0))
        port = blocker.getsockname()[1]
        hub = start_hub("--mdns", "--mdns-addr", "127.0.0.1", "--mdns-port", str(port))
        assert hub.alive()
        deadline = time.monotonic() + 3
        while b"mdns disabled" not in hub.err_path.read_bytes() and time.monotonic() < deadline:
            time.sleep(0.02)
        err = hub.err_path.read_text()
        assert err.count("mdns disabled") == 1 and b"mdns port" not in hub.out_path.read_bytes()


def _service_info(msg) -> ServiceInfo:
    srv, txt, addr = (by_type(msg, c)[0] for c in (DNSService, DNSText, DNSAddress))
    return ServiceInfo(
        SVC,
        srv.name,
        port=srv.port,
        server=srv.server,
        properties=txt.text,
        addresses=[addr.address],
    )


@pytest.mark.usefixtures("enable_custom_integrations")
async def test_discovery_feeds_zeroconf_step(hass, mdns):
    """Hub's replies, shaped like HA's zeroconf listener does, drive the real config flow."""
    info = _service_info(mdns.ask(SVC, const._TYPE_PTR))
    host = info.parsed_addresses()[0]
    discovery = ZeroconfServiceInfo(
        ip_address=host,
        ip_addresses=info.ip_addresses_by_version(IPVersion.V4Only),
        hostname=info.server,
        name=info.name,
        port=info.port,
        properties={k.decode(): v.decode() for k, v in info.properties.items()},
        type=SVC,
    )
    assert discovery.name == INST and discovery.properties == {"hub_id": HUB_ID, "v": "1"}
    res = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": config_entries.SOURCE_ZEROCONF}, data=discovery
    )
    assert res["type"] is FlowResultType.FORM and res["step_id"] == "user"
    schema = {str(k): k.default() for k in res["data_schema"].schema if callable(k.default)}
    assert schema["host"] == "127.0.0.1" and schema["port"] == mdns.hub.port
