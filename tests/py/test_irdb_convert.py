"""Archive command -> IR action. The exactness proof: every `proto` mapping is encoded with our
reference encoder (tests/py/ir_ref.py) and compared with the command's own pronto."""

import json
from pathlib import Path

import ir_ref
import pytest

from custom_components.ha_harmony.harmony_core.ir.model import IrCode
from harmony_irdb.convert import EXACT, Converted, Skipped, convert_command, exact_proto

FIX = Path(__file__).parent / "fixtures" / "irdb"
REAL = json.loads((FIX / "exactness.json").read_text())


def _frames(pronto: str):
    w = [int(x, 16) for x in pronto.split()]
    unit = w[1] * 0.241246
    d = [round(x * unit) for x in w[4:]]
    return 1e6 / unit, d[: 2 * w[2]], d[2 * w[2] :]


def _close(theirs, ours, hz_a, hz_b):
    """Carrier within 1%; every mark/space up to the last mark within max(40 us, 6%)."""
    if abs(hz_a - hz_b) / hz_b > 0.01:
        return False
    ours = ours[:-1] if len(ours) % 2 == 0 else ours  # trailing gap is padding, not signal
    theirs = theirs[: len(ours)]
    return len(theirs) == len(ours) and all(
        abs(a - b) <= max(40, 0.06 * b) for a, b in zip(theirs, ours, strict=True)
    )


def test_every_exact_protocol_has_real_samples():
    by = {}
    for c in REAL:
        by.setdefault(c["protocol"], []).append(c)
    assert set(by) == set(EXACT), "a protocol in EXACT without proof samples (or vice versa)"
    assert all(len(v) >= 9 for v in by.values())


@pytest.mark.parametrize("cmd", REAL, ids=lambda c: f"{c['protocol']}-{c['keycode'][-24:]}")
def test_proto_mapping_is_exact(cmd):
    res = convert_command(cmd)
    assert isinstance(res, Converted) and res.form == "proto", res
    hz, once, rep = ir_ref.encode(res.action)
    t_hz, t_once, t_rep = _frames(cmd["pronto"])
    assert _close(t_once, once, t_hz, hz), (res.action, t_once[:8], once[:8])
    if t_rep:  # the archive's separate repeat frame must match ours too
        assert _close(t_rep, rep, t_hz, hz), (res.action, t_rep, rep)
    IrCode(**res.action)  # accepted by the hub-side model (ranges)


def test_proto_outputs_cover_both_nec_forms():
    protos = {convert_command(c).action["proto"] for c in REAL}
    assert protos == {"sirc12", "sirc15", "sirc20", "nec", "necext"}


def test_known_values():
    # Sony 12 bit 0x911 is wire order (command first, LSB first): bit-reversed it is 0x889
    assert exact_proto("Sony 12 Bit", "G:Sony 12 Bit:()(0x911)():3") == {
        "proto": "sirc12",
        "addr": 0x11,
        "cmd": 0x09,
    }
    toshiba = "G:Toshiba 32 Bit:(0x02FD00FF)(Repeat)():3"  # classic NEC: complement bytes
    assert exact_proto("Toshiba 32 Bit", toshiba) == {"proto": "nec", "addr": 0x40, "cmd": 0}
    ext = exact_proto("Toshiba 32 Bit", "G:Toshiba 32 Bit:(0x18EF18E7)(Repeat)():3")
    assert ext["proto"] == "necext"


@pytest.mark.parametrize(
    "protocol,keycode",
    [
        ("Sony 12 Bit", "G:Sony 12 Bit:()(0x91)():3"),  # wrong digit width
        ("Sony 12 Bit", "G:Sony 12 Bit:()(0x0911)():3"),
        ("Sony 12 Bit", "G:Sony 12 Bit:(0x911)()():3"),  # unexpected group layout
        ("Sony 12 Bit", "G:Sony 15 Bit:()(0x911)():3"),  # protocol/keycode disagree
        ("Sony 12 Bit", "G:Sony 12 Bit:()(0xZZZ)():3"),
        ("Sony 12 Bit", None),
        ("NEC 32 Bit", "G:NEC 32 Bit:(0x18EF18E7)()():3"),  # no Repeat marker
        ("NEC 32 Bit", "G:NEC 32 Bit:(0x18EF18E)(Repeat)():3"),
        ("NEC 32 Bit", "garbage"),
        ("Toshiba 32 Bit", "G:Toshiba 32 Bit:(0x02FD00FF)(Repeat)(x):3"),
        ("Samsung 32 Bit", "G:Samsung 32 Bit:(0xB4B422DD)(Repeat)():3"),  # not exact: see EXACT
        ("Philips RC5 13 Bit Toggle", "G:Philips RC5 13 Bit Toggle:()(0x00F8)():3"),  # toggle bit
        ("Philips RC6", "G:Philips RC6:()(Start_0x10_Space_1x04A_Trailer)():3"),
        (None, "G:x:()()():3"),
    ],
)
def test_conservative_no_proto(protocol, keycode):
    assert exact_proto(protocol, keycode) is None


# ---- pronto path
WORDS = "0000 006D 0002 0001 0156 00AB 0016 0900 0156 0055"  # once 2 pairs, repeat 1 pair


def test_pronto_form_for_non_exact_protocol():
    cs = json.loads((FIX / "codesets/36/362eb927ae6329c2.json").read_text())["commands"]
    sam = next(c for c in cs if c["protocol"] == "Samsung 32 Bit")
    res = convert_command(sam)
    assert res.form == "pronto" and "prontoRepeat" in sam
    ref = _frames(sam["pronto"])
    out = _frames(res.action["pronto"])
    assert out[0] == ref[0]
    for o, r in ((out[1], ref[1]), (out[2], ref[2])):  # once AND repeat survive; gaps clamped
        assert len(o) == len(r)
        assert all(a == b or (a == 65529 and b > 65535) for a, b in zip(o, r, strict=True))
    assert out[2]
    IrCode(pronto=res.action["pronto"]).check_size()


def test_prontorepeat_equal_to_embedded_repeat_is_a_no_op():
    cmd = {
        "protocol": "X 32 Bit",
        "keycode": "G:X 32 Bit:()(0x1)():3",
        "pronto": WORDS,
        "prontoRepeat": "0000 006D 0000 0001 0156 0055",
    }
    res = convert_command(cmd)
    assert res.action["pronto"] == WORDS and res.notes == ()


def test_prontorepeat_that_differs_replaces_the_embedded_repeat():
    cmd = {"protocol": "X", "keycode": "k", "pronto": WORDS}
    cmd["prontoRepeat"] = "0000 006D 0000 0001 0156 0040"
    res = convert_command(cmd)
    assert res.action["pronto"].endswith("0156 0040")
    assert res.action["pronto"].split()[2:4] == ["0002", "0001"]


def test_prontorepeat_folded_when_pronto_has_no_repeat_part():
    cmd = {
        "protocol": "X 32 Bit",
        "keycode": "G:X 32 Bit:()(0x1)():3",
        "pronto": "0000 006D 0002 0000 0156 00AB 0016 0900",
        "prontoRepeat": "0000 006D 0000 0002 0156 0055 0016 0900",
    }
    res = convert_command(cmd)
    assert res.action["pronto"] == "0000 006D 0002 0002 0156 00AB 0016 0900 0156 0055 0016 0900"
    _, once, rep = _frames(res.action["pronto"])
    assert once[1] != rep[1]  # a distinct repeat frame, held in the pronto's repeat part
    assert any("prontoRepeat" in n for n in res.notes)


def test_prontorepeat_with_other_carrier_is_ignored():
    cmd = {
        "protocol": "X",
        "keycode": "k",
        "pronto": "0000 006D 0001 0000 0156 00AB",
        "prontoRepeat": "0000 0070 0000 0001 0156 0055",
    }
    res = convert_command(cmd)
    assert res.action["pronto"] == "0000 006D 0001 0000 0156 00AB"
    assert any("carrier" in n for n in res.notes)


def test_long_gaps_are_clamped_for_the_hub():
    cmd = {"protocol": "X", "keycode": "k", "pronto": "0000 006D 0001 0000 0156 0E46"}  # 96 ms gap
    res = convert_command(cmd)
    _, once, _ = _frames(res.action["pronto"])
    assert once[1] <= 65535 and once[1] > 65000
    ir_ref.pronto([int(w, 16) for w in res.action["pronto"].split()])  # the hub's rule: no error
    assert any("clamped" in n for n in res.notes)
    with pytest.raises(ValueError):  # and the raw archive pronto would have been rejected
        ir_ref.pronto([int(w, 16) for w in cmd["pronto"].split()])


@pytest.mark.parametrize(
    "cmd,why",
    [
        ({"protocol": "Roku IP", "keycode": "G:Roku IP:(Key)(Back):3"}, "no pronto"),
        ({"protocol": "X", "keycode": "k", "pronto": ""}, "no pronto"),
        ({"protocol": "X", "keycode": "k", "pronto": "zzzz"}, "malformed"),
        ({"protocol": "X", "keycode": "k", "pronto": "0000 006D 0001 0000 0156"}, "malformed"),
        ({"protocol": "X", "keycode": "k", "pronto": "0000 006D 0005 0000 0156 00AB"}, "structure"),
        ({"protocol": "X", "keycode": "k", "pronto": "0100 006D 0001 0000 0156 00AB"}, "structure"),
        ({"protocol": "X", "keycode": "k", "pronto": 5}, "malformed"),
        (
            {"protocol": "X", "keycode": "k", "pronto": "0000 006D 0101 0000 " + "0010 " * 514},
            "too long",
        ),
        ("not a dict", "not a command"),
    ],
)
def test_skipped(cmd, why):
    res = convert_command(cmd)
    assert isinstance(res, Skipped) and why in res.reason


def test_fixture_codesets_convert_and_pass_hub_limits():
    seen = {"proto": 0, "pronto": 0, "skipped": 0}
    for p in FIX.glob("codesets/*/*.json"):
        for c in json.loads(p.read_text())["commands"]:
            res = convert_command(c)
            if isinstance(res, Skipped):
                seen["skipped"] += 1
                continue
            seen[res.form] += 1
            if res.form == "pronto":
                IrCode(pronto=res.action["pronto"]).check_size()
                ir_ref.encode(res.action)
            else:
                IrCode(**res.action)
    assert seen["proto"] and seen["pronto"] and seen["skipped"] == 1  # Roku IP
