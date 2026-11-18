"""IR archive client: search, caching, fallback, offline, and hostile-input handling."""

import re
import threading

import pytest
from irdb_fake import FakeSession, jbytes

from harmony_irdb.archive import (
    BASES,
    ArchiveError,
    Brand,
    IrdbArchive,
    Model,
    OfflineError,
    load_manufacturers,
    safe_codeset_path,
    safe_segment,
    search_brands,
    search_models,
)

P, F = BASES  # primary (jsDelivr), fallback (GitHub raw)


class Clock:
    t = 1_000_000.0

    def __call__(self):
        return self.t


def make(tmp_path, session=None, **kw):
    return IrdbArchive(session or FakeSession(), tmp_path / "cache", **kw), kw


# ---- search
BRANDS = [
    Brand("Citroën Audio", "Citroen_Audio", 3),
    Brand("Sony", "Sony", 20109),
    Brand("Sonya Labs", "Sonya", 2),
    Brand("Ericsson", "Ericsson", 9),
    Brand("Mr. Sony Fan", "Mr_Sony_Fan", 1),
    Brand("Harman Kardon", "Harman_Kardon", 400),
]


def test_brand_search_ranking_and_folding():
    names = [b.name for b in search_brands(BRANDS, "sony")]
    assert names == ["Sony", "Sonya Labs", "Mr. Sony Fan"]  # exact, prefix, then word-prefix
    assert [b.name for b in search_brands(BRANDS, "CITROEN")] == ["Citroën Audio"]
    assert [b.name for b in search_brands(BRANDS, "citroën")] == ["Citroën Audio"]
    assert [b.name for b in search_brands(BRANDS, "kard")] == ["Harman Kardon"]  # word prefix
    assert [b.name for b in search_brands(BRANDS, "son")][0] == "Sony"  # prefix: bigger first
    assert [b.name for b in search_brands(BRANDS, "harmankardon")] == []  # no fuzzy joining
    assert [b.name for b in search_brands(BRANDS, "ricsson")] == ["Ericsson"]  # substring
    assert search_brands(BRANDS, "")[0].name == "Sony"
    assert len(search_brands(BRANDS, "", limit=2)) == 2


def test_bundled_manufacturer_list():
    brands = load_manufacturers()
    assert len(brands) > 7000
    sony = next(b for b in brands if b.name == "Sony")
    assert (sony.slug, sony.count > 1000) == ("Sony", True)
    odd = next(b for b in brands if b.name == "1&1")
    assert odd.slug == "1_1"  # folder name differs from the display name
    for b in brands:
        safe_segment(b.slug)


def test_model_search():
    ms = [Model("a.json", "KDL-46V2501"), Model("b.json", "BDP-S590"), Model("c.json", "KDL 40")]
    assert {m.file for m in search_models(ms, "kdl")} == {"a.json", "c.json"}
    assert [m.file for m in search_models(ms, "bdp s590")] == ["b.json"]
    assert len(search_models(ms, "")) == 3


# ---- typed accessors on the fixture slice
async def test_models_devices_codesets(tmp_path):
    arc, _ = make(tmp_path)
    models = await arc.models("Sony")
    names = {m.file: m.name for m in models}
    assert names["BDP-S590-231542.json"] == "BDP-S590"  # padding stripped
    assert names["device-234951.json"] == "device-234951"  # null model falls back to the file
    dev = await arc.device("Sony", "KV-32V42.json")
    cs = await arc.codeset(dev["codeset"])
    assert any(c["name"] == "PowerOn" for c in cs["commands"])


async def test_missing_file_is_a_clean_error(tmp_path):
    arc, _ = make(tmp_path)
    with pytest.raises(ArchiveError, match="404") as e:
        await arc.device("Sony", "device-234951.json")
    assert not isinstance(e.value, OfflineError)


# ---- hosts, cache, ETag, TTL
async def test_primary_host_first_and_cache_hit_needs_no_network(tmp_path):
    s = FakeSession()
    arc, _ = make(tmp_path, s)
    await arc.models("Sony")
    assert s.urls() == [P + "devices/Sony/index.json"]
    await arc.models("Sony")
    assert len(s.calls) == 1


async def test_fallback_host_when_primary_fails(tmp_path):
    s = FakeSession()
    s.down.add(P)
    arc, _ = make(tmp_path, s)
    assert await arc.models("Toshiba")
    assert s.urls() == [P + "devices/Toshiba/index.json", F + "devices/Toshiba/index.json"]
    s2 = FakeSession()
    s2.status[P] = 503
    arc2 = IrdbArchive(s2, tmp_path / "c2")
    assert await arc2.models("Toshiba")
    assert len(s2.calls) == 2


async def test_etag_revalidation_after_ttl(tmp_path):
    s, clock = FakeSession(), Clock()
    arc = IrdbArchive(s, tmp_path / "c", ttl=100, now=clock)
    first = await arc.models("Toshiba")
    clock.t += 50
    await arc.models("Toshiba")
    assert len(s.calls) == 1  # still fresh
    clock.t += 100  # expired: revalidate with the stored ETag
    again = await arc.models("Toshiba")
    assert len(s.calls) == 2 and again == first
    assert s.calls[1][1]["If-None-Match"].startswith('"')
    clock.t += 50  # the 304 renewed the TTL
    await arc.models("Toshiba")
    assert len(s.calls) == 2
    # content changed upstream: the old ETag no longer matches, new data is stored
    s.files["devices/Toshiba/index.json"] = jbytes([{"f": "x.json", "id": 1, "m": "X"}])
    clock.t += 500
    assert [m.file for m in await arc.models("Toshiba")] == ["x.json"]


async def test_ttl_expired_and_network_down_serves_stale(tmp_path):
    s, clock = FakeSession(), Clock()
    arc = IrdbArchive(s, tmp_path / "c", ttl=10, now=clock)
    await arc.models("Toshiba")
    clock.t += 1000
    s.down.update(BASES)
    assert await arc.models("Toshiba")  # stale beats nothing


async def test_offline_without_cache_is_a_clear_error(tmp_path):
    s = FakeSession()
    arc = IrdbArchive(s, tmp_path / "c", offline=True)
    with pytest.raises(OfflineError, match="offline"):
        await arc.models("Sony")
    assert s.calls == []  # offline never touches the network
    s2 = FakeSession()
    s2.down.update(BASES)
    with pytest.raises(OfflineError, match="cannot reach"):
        await IrdbArchive(s2, tmp_path / "c2").models("Sony")


async def test_offline_with_cache_ignores_ttl(tmp_path):
    clock = Clock()
    online = IrdbArchive(FakeSession(), tmp_path / "c", ttl=1, now=clock)
    await online.models("Sony")
    clock.t += 10**7
    s = FakeSession()
    arc = IrdbArchive(s, tmp_path / "c", ttl=1, now=clock, offline=True)
    assert await arc.models("Sony")
    assert s.calls == []


async def test_disk_io_runs_off_the_event_loop_thread(tmp_path, monkeypatch):
    main = threading.get_ident()
    seen = []
    real_w, real_r = IrdbArchive._write, IrdbArchive._read
    monkeypatch.setattr(
        IrdbArchive,
        "_write",
        lambda self, *a: (seen.append(threading.get_ident()), real_w(self, *a))[1],
    )
    monkeypatch.setattr(
        IrdbArchive,
        "_read",
        lambda self, *a: (seen.append(threading.get_ident()), real_r(self, *a))[1],
    )
    arc, _ = make(tmp_path)
    await arc.models("Sony")
    assert seen and all(t != main for t in seen)


# ---- hostile input
@pytest.mark.parametrize(
    "bad",
    [
        "..",
        ".",
        "",
        "/etc/passwd",
        "a/b",
        "a\\b",
        "x\x00y",
        "a\nb",
        "a\x7fb",
        "../x",
        "x" * 201,
        None,
        5,
    ],
)
def test_safe_segment_rejects(bad):
    with pytest.raises(ArchiveError):
        safe_segment(bad)


def test_safe_segment_accepts_real_names():
    for ok in ["Sony", "1_1", "BDP-S590-231542.json", "Satellite X205-S9349.json", "a..b.json"]:
        assert safe_segment(ok) == ok


@pytest.mark.parametrize(
    "bad",
    [
        "/codesets/ed/x.json",
        "codesets/../../etc/passwd",
        "codesets/ed/../x.json",
        "codesets/ed/x.json/extra",
        "devices/Sony/index.json",
        "codesets/e d/x.json",
        "codesets/ed/x.txt",
        "codesets//x.json",
        "https://evil.example/codesets/ed/x.json",
        None,
    ],
)
def test_codeset_path_validation(bad):
    with pytest.raises(ArchiveError):
        safe_codeset_path(bad)


async def test_unsafe_names_never_reach_the_network(tmp_path):
    s = FakeSession()
    arc, _ = make(tmp_path, s)
    for slug, f in [
        ("..", "x.json"),
        ("Sony", "../../index.json"),
        ("a/b", "x.json"),
        ("Sony", "a\\b"),
    ]:
        with pytest.raises(ArchiveError):
            await arc.device(slug, f)
    with pytest.raises(ArchiveError):
        await arc.codeset("../../index.json")
    with pytest.raises(ArchiveError):
        await arc.models("../Sony")
    assert s.calls == []


async def test_bad_model_index_entries_are_dropped(tmp_path):
    index = [
        {"f": "ok.json", "id": 1, "m": "OK"},
        {"f": "../evil.json", "id": 2, "m": "Evil"},
        {"f": "a/b.json", "m": "Slash"},
        {"f": "c\x00.json", "m": "Nul"},
        {"f": "notjson.txt", "m": "Txt"},
        {"f": "index.json", "m": "Self"},
        {"f": 5, "m": "Num"},
        "string",
        None,
        {"f": "m.json", "m": ["not", "a", "string"], "id": "7", "extra": {"x": 1}},
    ]
    arc, _ = make(tmp_path, FakeSession({"devices/B/index.json": jbytes(index)}))
    assert [(m.file, m.name, m.id) for m in await arc.models("B")] == [
        ("ok.json", "OK", 1),
        ("m.json", "m", None),
    ]


async def test_only_fixed_hosts_and_no_redirects(tmp_path):
    s = FakeSession()
    s.status[P] = 302  # a redirect must be treated as a failure, never followed
    arc, _ = make(tmp_path, s)
    await arc.models("Sony")
    assert all(u.startswith(BASES) for u in s.urls())
    assert all(c[2]["allow_redirects"] is False for c in s.calls)


async def test_cache_names_are_hashes_and_stay_inside(tmp_path):
    s = FakeSession({"devices/..x/index.json": jbytes([])})
    arc, _ = make(tmp_path, s)
    await arc.models("..x")  # legal name that merely contains dots
    files = list((tmp_path / "cache").iterdir())
    assert files and all(re.fullmatch(r"[0-9a-f]{40}\.json", f.name) for f in files)
    assert {p for p in tmp_path.iterdir()} == {tmp_path / "cache"}


async def test_response_size_cap(tmp_path):
    big = jbytes([{"f": "a.json", "m": "x" * 5000}])
    s = FakeSession({"devices/B/index.json": big})
    arc = IrdbArchive(s, tmp_path / "c", max_bytes=1000)
    with pytest.raises(ArchiveError, match="larger"):  # streamed past the cap, no header
        await arc.models("B")
    s.content_length = 10**9  # declared too large: refused before reading
    with pytest.raises(ArchiveError, match="larger"):
        await IrdbArchive(s, tmp_path / "c2", max_bytes=10**6).models("B")
    assert not list((tmp_path / "c").glob("*.json"))  # nothing oversized is cached


@pytest.mark.parametrize(
    "body",
    [b"not json", b"", b"\xff\xfe", b"[" * 100000, b'{"a": ' * 5000],
)
async def test_malformed_json_fails_cleanly_and_is_not_cached(tmp_path, body):
    arc, _ = make(tmp_path, FakeSession({"devices/B/index.json": body}))
    with pytest.raises(ArchiveError):
        await arc.models("B")
    assert not list((tmp_path / "cache").glob("*.json")) if (tmp_path / "cache").exists() else True


@pytest.mark.parametrize(
    "files,call",
    [
        ({"devices/B/index.json": b'{"f": 1}'}, lambda a: a.models("B")),
        ({"devices/B/x.json": b"[1,2]"}, lambda a: a.device("B", "x.json")),
        (
            {"codesets/aa/b.json": b'{"commands": "nope"}'},
            lambda a: a.codeset("codesets/aa/b.json"),
        ),
        ({"codesets/aa/b.json": b'{"other": []}'}, lambda a: a.codeset("codesets/aa/b.json")),
    ],
)
async def test_wrong_shapes_are_archive_errors(tmp_path, files, call):
    arc, _ = make(tmp_path, FakeSession(files))
    with pytest.raises(ArchiveError):
        await call(arc)


async def test_corrupt_cache_entry_is_refetched(tmp_path):
    s = FakeSession()
    arc, _ = make(tmp_path, s)
    await arc.models("Toshiba")
    for f in (tmp_path / "cache").iterdir():
        f.write_text("{broken")
    assert await arc.models("Toshiba")
    assert len(s.calls) == 2
