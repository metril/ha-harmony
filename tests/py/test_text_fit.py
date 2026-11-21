"""Shared fixture (tests/vectors/text_fit.json): addon/frontend/test/text-fit.test.ts runs the same cases."""

import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from harmony_core.key_names import abbreviate, display_name
from harmony_core.text_fit import clip, fit_label, label_budget, text_px

FIX = json.loads((Path(__file__).resolve().parents[1] / "vectors" / "text_fit.json").read_text())


@pytest.mark.parametrize("c", FIX["cases"], ids=lambda c: c["name"])
def test_budget(c):
    page, w = SimpleNamespace(cols=c["cols"], rows=c["rows"]), SimpleNamespace(**c["widget"])
    b = label_budget(page, w)
    e = c["expect"]
    if e is None:
        assert b is None
        return
    assert b == (e["font"], e["px"])
    assert text_px(w.label, b[0]) == e["text_px"]
    assert clip(w.label, *b) == (e["shown"], e["over"])
    f = fit_label(page, w)
    assert (f is None) == e["fits"]
    if f:
        assert f[2] == e["over"]


@pytest.mark.parametrize("c", FIX["clip"], ids=lambda c: c["s"])
def test_clip(c):
    assert clip(c["s"], c["font"], c["px"]) == (c["shown"], c["over"])


KEYS = json.loads((Path(__file__).resolve().parents[1] / "vectors" / "key_names.json").read_text())


@pytest.mark.parametrize(("raw", "want"), KEYS["display"])
def test_display_name(raw, want):
    assert display_name(raw) == want


@pytest.mark.parametrize(("raw", "want"), KEYS["abbreviate"])
def test_abbreviate(raw, want):
    assert abbreviate(raw) == want


def test_unknown_glyph_counts_as_question_mark():
    assert text_px("中", "b13") == text_px("?", "b13")
