"""IR database routes against the fixture slice (fake session, no network)."""

import json

import pytest
from irdb_fake import FakeSession, fixture_files, jbytes
from test_addon import H

from harmony_addon import ir_routes
from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth
from harmony_irdb.archive import BASES, IrdbArchive


def archive(tmp_path, session=None, **kw):
    return IrdbArchive(session or FakeSession(), tmp_path / "irdb-cache", **kw)


@pytest.fixture
async def ir(aiohttp_client, tmp_path):
    session = FakeSession()
    app = create_app(
        tmp_path / "d", auth=NoAuth(), irdb=archive(tmp_path, session), clock=lambda: 0
    )
    c = await aiohttp_client(app)
    c.fake = session
    return c


async def get(c, path):
    r = await c.get(f"/api/v1/ir{path}")
    return r.status, await r.json()


# ---------------------------------------------------------------- brands
async def test_brands_ranking_and_cap(ir):
    st, body = await get(ir, "/brands?q=sony")
    assert st == 200 and body["brands"][0] == {"name": "Sony", "slug": "Sony", "count": 20109}
    st, body = await get(ir, "/brands")
    assert len(body["brands"]) == 50  # capped; most models first
    assert body["brands"][0]["count"] >= body["brands"][1]["count"]
    st, body = await get(ir, "/brands?q=zzzzzzzzqq")
    assert body == {"brands": []}
    assert (await get(ir, "/brands?q=" + "x" * 65))[0] == 400
    assert (await get(ir, "/brands?bogus=1"))[0] == 400


# ---------------------------------------------------------------- models
async def test_models_paging_and_search(ir):
    st, body = await get(ir, "/models?brand=Sony")
    assert st == 200 and body["total"] == 5 and body["brand"]["slug"] == "Sony"
    assert body["models"][0] == {"file": "KV-32V42.json", "name": "KV-32V42", "id": 1520}
    st, page = await get(ir, "/models?brand=Sony&offset=3&limit=1")
    assert page["total"] == 5 and len(page["models"]) == 1 and page["offset"] == 3
    st, found = await get(ir, "/models?brand=Sony&q=kdl")
    assert [m["file"] for m in found["models"]] == ["KDL-46V2501.json"] and found["total"] == 1
    st, body = await get(ir, "/models?brand=Sony&offset=99")
    assert body["models"] == [] and body["total"] == 5


async def test_models_never_returns_more_than_200(aiohttp_client, tmp_path):
    files = fixture_files()
    files["devices/Sony/index.json"] = jbytes(
        [{"f": f"m{i}.json", "id": i, "m": f"Model {i}"} for i in range(1000)]
    )
    s = FakeSession(files)
    c = await aiohttp_client(create_app(tmp_path / "d", auth=NoAuth(), irdb=archive(tmp_path, s)))
    st, body = await get(c, "/models?brand=Sony&limit=200")
    assert st == 200 and len(body["models"]) == 200 and body["total"] == 1000
    assert (await get(c, "/models?brand=Sony&limit=201"))[0] == 400
    assert (await get(c, "/models?brand=Sony&limit=0"))[0] == 400
    assert (await get(c, "/models?brand=Sony&limit=-1"))[0] == 400
    assert (await get(c, "/models?brand=Sony&offset=abc"))[0] == 400
    st, body = await get(c, "/models?brand=Sony&q=model%2099&limit=200")
    assert len(body["models"]) <= 200


@pytest.mark.parametrize("brand", ["../etc", "a/b", "a%5Cb", "..", "%00", "Nonexistent", "x" * 201])
async def test_brand_parameter_is_sanitised_and_must_be_known(ir, brand):
    st, _ = await get(ir, f"/models?brand={brand}")
    assert st in (400, 404), brand
    assert not ir.fake.calls  # nothing was requested from the network


async def test_missing_params_are_400(ir):
    for path in ("/models", "/device", "/device?brand=Sony", "/candidates?brand=Sony"):
        assert (await get(ir, path))[0] == 400, path


# ---------------------------------------------------------------- device
async def test_device_block(ir):
    st, body = await get(ir, "/device?brand=Sony&model=KV-32V42.json")
    assert st == 200
    d = body["device"]
    assert d["id"] == "sony_kv_32v42" and d["power"]["on"]["proto"] == "sirc12"
    assert body["forms"] and set(body["forms"].values()) <= {"proto", "pronto"}
    assert body["source"]["kind"] == "harmony_archive" and body["source"]["mfr"] == "Sony"
    assert body["source"]["imported_at"] == "1970-01-01T00:00:00Z"
    assert body["device_type"] == {"id": 1, "name": "Television"}
    assert isinstance(body["skipped"], list) and isinstance(body["notes"], list)
    # the returned block is a valid config device
    from harmony_core.config_model import parse_model

    parse_model(json.dumps({"devices": [d]}))


@pytest.mark.parametrize("model", ["../x.json", "a/b.json", "x", "index.json", "..", "%00.json"])
async def test_device_model_parameter_is_sanitised(ir, model):
    st, _ = await get(ir, f"/device?brand=Sony&model={model}")
    assert st == 400
    assert not ir.fake.calls


async def test_unknown_model_is_a_stable_502(ir):
    st, body = await get(ir, "/device?brand=Sony&model=nothing.json")
    assert st == 502 and body["error"]["code"] == "archive_error"
    assert "http" not in json.dumps(body).lower()  # no URLs echoed


# ---------------------------------------------------------------- candidates
async def test_candidates(ir):
    st, body = await get(ir, "/candidates?brand=Sony&type=tv&limit=50")
    assert st == 200 and body["matched"] >= 1
    c = body["candidates"][0]
    assert set(c) == {"kind", "action", "form", "count", "models", "codesets"}
    counts = [x["count"] for x in body["candidates"]]
    assert counts == sorted(counts, reverse=True)
    assert (await get(ir, "/candidates?brand=Sony&type=blender"))[0] == 400
    assert (await get(ir, "/candidates?brand=Sony&type=tv&limit=201"))[0] == 400


# ---------------------------------------------------------------- archive failures
async def test_archive_down_is_504_and_cache_makes_it_work_offline(aiohttp_client, tmp_path):
    down = FakeSession()
    down.down = set(BASES)
    c = await aiohttp_client(
        create_app(tmp_path / "a", auth=NoAuth(), irdb=archive(tmp_path, down))
    )
    st, body = await get(c, "/models?brand=Sony")
    assert st == 504 and body["error"]["code"] == "archive_offline"
    # warm the cache online, then lose the network: the cache still answers
    warm = archive(tmp_path / "w", FakeSession(), ttl=0)
    c2 = await aiohttp_client(create_app(tmp_path / "b", auth=NoAuth(), irdb=warm))
    assert (await get(c2, "/models?brand=Sony"))[0] == 200
    offline = FakeSession()
    offline.down = set(BASES)
    cold = archive(tmp_path / "w", offline, ttl=0)  # same cache dir, entries all expired
    c3 = await aiohttp_client(create_app(tmp_path / "c", auth=NoAuth(), irdb=cold))
    st, body = await get(c3, "/models?brand=Sony")
    assert st == 200 and body["total"] == 5


async def test_bad_upstream_status_is_502(aiohttp_client, tmp_path):
    s = FakeSession()
    s.status = {b: 500 for b in BASES}
    c = await aiohttp_client(create_app(tmp_path / "a", auth=NoAuth(), irdb=archive(tmp_path, s)))
    st, body = await get(c, "/models?brand=Sony")
    assert st == 502 and body["error"]["code"] == "archive_error"


async def test_oversized_archive_answer_is_502(aiohttp_client, tmp_path):
    s = FakeSession()
    s.content_length = 10**9
    c = await aiohttp_client(create_app(tmp_path / "a", auth=NoAuth(), irdb=archive(tmp_path, s)))
    assert (await get(c, "/models?brand=Sony"))[0] == 502


async def test_default_archive_lives_under_the_data_dir(aiohttp_client, tmp_path):
    app = create_app(tmp_path / "data", auth=NoAuth())
    await aiohttp_client(app)
    assert str(app[ir_routes.IRDB_KEY]._dir) == str(tmp_path / "data" / "irdb")


# ---------------------------------------------------------------- parse
PRONTO = "0000 006D 0002 0000 0060 0018 0018 0018"
FLIPPER = """Filetype: IR signals file
Version: 1
#
name: Power
type: parsed
protocol: NEC
address: 04 00 00 00
command: 08 00 00 00
#
name: Raw
type: raw
frequency: 38000
duty_cycle: 0.330000
data: 9000 4500 560 560 560 1690
#
name: Weird
type: parsed
protocol: Kaseikyo
address: 01 00 00 00
command: 02 00 00 00
"""


async def parse(c, kind, text):
    r = await c.post("/api/v1/ir/parse", json={"kind": kind, "text": text}, headers=H)
    return r.status, await r.json()


async def test_parse_pronto_flipper_smartir(ir):
    st, body = await parse(ir, "pronto", PRONTO)
    assert st == 200 and body["actions"][0]["form"] == "pronto"
    assert body["actions"][0]["action"] == {"pronto": PRONTO}
    st, body = await parse(ir, "flipper", FLIPPER)
    assert st == 422  # an unsupported protocol is reported, never dropped silently
    good = FLIPPER[: FLIPPER.index("#\nname: Weird")]
    st, body = await parse(ir, "flipper", good)
    names = {a["name"]: a for a in body["actions"]}
    assert names["Power"]["action"] == {"proto": "nec", "addr": 4, "cmd": 8}
    assert names["Power"]["form"] == "proto" and names["Raw"]["form"] == "raw"
    smart = json.dumps({"commandsEncoding": "pronto", "commands": {"on": PRONTO}})
    st, body = await parse(ir, "smartir", smart)
    assert st == 200 and body["actions"][0]["name"] == "on"


@pytest.mark.parametrize(
    ("kind", "text"),
    [
        ("pronto", "not pronto"),
        ("pronto", "0000 006D 0002 0000 0060"),
        ("flipper", "garbage"),
        ("flipper", "Filetype: IR signals file\nVersion: 1\n"),
        ("smartir", "{not json"),
        ("smartir", "[]"),
        ("smartir", json.dumps({"commandsEncoding": "base64", "commands": {"a": "!!!"}})),
    ],
)
async def test_parse_garbage_is_422_with_path_msg(ir, kind, text):
    st, body = await parse(ir, kind, text)
    assert st == 422 and body["error"]["errors"][0]["msg"]
    assert isinstance(body["error"]["errors"][0]["path"], list)


async def test_parse_schema_and_caps(ir):
    for body in (
        {"kind": "xml", "text": "x"},
        {"kind": "pronto"},
        {"kind": "pronto", "text": ""},
        {"kind": "pronto", "text": "x" * (ir_routes.MAX_PARSE_TEXT + 1)},
        {"kind": "pronto", "text": "x", "extra": 1},
    ):
        r = await ir.post("/api/v1/ir/parse", json=body, headers=H)
        assert r.status == 400, body
    many = "Filetype: IR signals file\nVersion: 1\n" + "".join(
        f"#\nname: S{i}\ntype: parsed\nprotocol: NEC\naddress: 04 00 00 00\ncommand: {i % 256:02X} 00 00 00\n"
        for i in range(300)
    )
    st, body = await parse(ir, "flipper", many)
    assert st == 200 and len(body["actions"]) == ir_routes.MAX_PARSED and body["skipped"]


async def test_parse_too_long_signal_is_skipped_not_fatal(ir):
    raw = " ".join(["500"] * 600)
    text = (
        "Filetype: IR signals file\nVersion: 1\n#\nname: Long\ntype: raw\nfrequency: 38000\n"
        f"duty_cycle: 0.33\ndata: {raw}\n#\nname: Ok\ntype: parsed\nprotocol: NEC\n"
        "address: 04 00 00 00\ncommand: 08 00 00 00\n"
    )
    st, body = await parse(ir, "flipper", text)
    assert st == 200 and [a["name"] for a in body["actions"]] == ["Ok"]
    assert body["skipped"][0]["name"] == "Long"
