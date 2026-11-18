"""Every limit and rule the editor must handle, driven through the add-on's own preview.

Each case breaks the large synthetic fixture in one way and records the error the backend
answers. The result is the table the frontend test (addon/frontend/test/rules.test.ts) checks
against its UI handling; regenerate it with `UPDATE_RULES=1 uv run pytest tests/py/test_editor_rules.py`.
"""

import copy
import json
import os
from pathlib import Path

import pytest

from harmony_addon.app import _preview

ROOT = Path(__file__).parent
BASE = json.loads((ROOT / "fixtures" / "large_config.json").read_text())
TABLE = ROOT.parents[1] / "addon" / "frontend" / "test" / "fixtures" / "rule-errors.json"
HA = {"service": "light.toggle", "target": {"entity_id": "light.lamp"}}
IR = {"proto": "nec", "addr": 1, "cmd": 2}


def widget(page, **kw):
    return {
        "type": "button",
        "id": "x_new",
        "x": 3,
        "y": 3,
        "label": "X",
        "action": {"device": "tv", "command": "up"},
        **kw,
    }


def pg(c, i=4):
    return c["pages"][i]


def _set(path, value):
    def m(c):
        cur = c
        for k in path[:-1]:
            cur = cur[k]
        cur[path[-1]] = value

    return m


def _dev(c, **kw):
    c["devices"][0].update(kw)


def _many_pages(c):
    c["pages"] += [{"id": f"p{n}", "cols": 1, "rows": 1, "widgets": []} for n in range(20)]


def _many_widgets(c):
    p = pg(c)
    p["cols"], p["rows"] = 16, 16
    p["widgets"] = [{"type": "label", "x": n % 16, "y": n // 16, "label": "L"} for n in range(33)]


def _many_soft(c):
    c["pages"] += [
        {
            "id": f"s{n}",
            "cols": 4,
            "rows": 4,
            "widgets": [widget(None, id=f"s{n}_{k}", x=k % 4, y=k // 4) for k in range(16)],
        }
        for n in range(2)
    ]


def _dup_widget(c):
    pg(c)["widgets"][1]["id"] = pg(c)["widgets"][0]["id"]


def _nav_next_last(c):
    c["pages"][-1]["widgets"][0] = {
        "type": "button",
        "id": "nv",
        "x": 0,
        "y": 0,
        "label": "N",
        "nav": "next",
    }


def _nav_home_missing(c):
    c["pages"][0].pop("kind")
    pg(c)["widgets"][0] = {
        "type": "button",
        "id": "nv",
        "x": 0,
        "y": 0,
        "label": "N",
        "nav": "home",
    }


def _second_home(c):
    c["pages"][1]["kind"] = "home"


def _list_xy(c):
    c["pages"][2]["widgets"][0]["x"] = 0


def _long_label(c):
    pg(c)["widgets"][0]["label"] = "L" * 25


def _over_budget(c):
    p = pg(c)
    p["cols"], p["rows"] = 16, 16
    p["widgets"] = [
        {"type": "label", "x": n % 16, "y": n // 16, "label": "twenty-three bytes long.."}
        for n in range(32)
    ]


def _devices_cap(c):
    c["devices"] += [{"id": f"d{n}", "commands": {}} for n in range(14)]


def _cmd_cap(c):
    c["devices"][0]["commands"].update({f"c{n}": IR for n in range(200)})


def _ha_dev_bad(c):
    c["devices"][4]["commands"]["x"] = IR


def _ha_power(c):
    c["devices"][4]["power"].pop("off")


def _editor_big(c):
    c["editor"] = {"k": "x" * 1900, "l": ["y" * 1900] * 40}


def _alias_dup(c):
    c["devices"][0]["aliases"] = ["avr"]


def _dup_dev(c):
    c["devices"][1]["id"] = "tv"


def _step_unknown(c):
    c["activities"][0]["steps"] = [{"device": "tv", "command": "nope"}]


def _step_activity(c):
    c["activities"][0]["steps"] = [{"activity": "stream"}]


def _too_many_steps(c):
    c["activities"][0]["steps"] = [{"device": "tv", "command": "up"}] * 40


def _ha_entities(c):
    c["keys"]["play"] = {
        "ha": {
            "service": "light.toggle",
            "target": {"entity_id": [f"light.l{n}" for n in range(9)]},
        }
    }


def _ha_template(c):
    c["keys"]["play"] = {"ha": {**HA, "data": {"brightness": "{{ 5 }}"}}}


def _ha_deep(c):
    c["keys"]["play"] = {"ha": {**HA, "data": {"a": {"b": {"c": {"d": {"e": 1}}}}}}}


def _ha_big(c):
    c["keys"]["play"] = {"ha": {**HA, "data": {f"k{n}": "y" * 100 for n in range(12)}}}


def _tile_sensor_toggle(c):
    pg(c)["widgets"][2]["tap"] = "toggle"


CASES: list[tuple[str, object, list]] = [
    ("device id pattern", _set(["devices", 0, "id"], "Bad Id"), ["devices", 0, "id"]),
    ("device name 31 bytes", _set(["devices", 0, "name"], "n" * 32), ["devices", 0, "name"]),
    (
        "command name 31 bytes",
        lambda c: c["devices"][0]["commands"].update({"é" * 16: IR}),
        ["devices", 0, "commands"],
    ),
    (
        "reserved command power_on",
        lambda c: c["devices"][0]["commands"].update({"power_on": IR}),
        ["devices", 0, "commands"],
    ),
    (
        "reserved command input_x",
        lambda c: c["devices"][0]["commands"].update({"input_x": IR}),
        ["devices", 0, "commands"],
    ),
    (
        "power needs both or toggle",
        lambda c: c["devices"][0]["power"].pop("off"),
        ["devices", 0, "power"],
    ),
    ("settle_ms 0..60000", _set(["devices", 0, "settle_ms"], 70000), ["devices", 0, "settle_ms"]),
    (
        "port mask 1..255",
        lambda c: c["devices"][0]["commands"]["up"].update(port_mask=300),
        ["devices", 0, "commands", "up"],
    ),
    (
        "repeats 1..8",
        lambda c: c["devices"][0]["commands"]["up"].update(repeats=9),
        ["devices", 0, "commands", "up"],
    ),
    (
        "IR form exactly one",
        lambda c: c["devices"][0]["commands"].update({"up": {"proto": "nec", "pronto": "0000"}}),
        ["devices", 0, "commands", "up"],
    ),
    ("alias is another device id", _alias_dup, ["devices", 0, "aliases"]),
    ("duplicate device id", _dup_dev, ["devices", 1, "id"]),
    ("max 16 devices", _devices_cap, []),
    ("max 256 commands", _cmd_cap, []),
    ("HA device takes only ha actions", _ha_dev_bad, ["devices", 4, "commands"]),
    ("HA device power needs on and off", _ha_power, ["devices", 4, "power"]),
    (
        "HA device entity pattern",
        _set(["devices", 4, "entity"], "Bad Entity"),
        ["devices", 4, "entity"],
    ),
    ("activity id `off` reserved", _set(["activities", 0, "id"], "off"), ["activities", 0, "id"]),
    (
        "activity unknown device",
        lambda c: c["activities"][0]["devices"].append({"device": "ghost"}),
        ["activities", 0, "devices"],
    ),
    (
        "activity unknown input",
        _set(["activities", 0, "devices", 0, "input"], "nope"),
        ["activities", 0, "devices", 0, "input"],
    ),
    ("step unknown command", _step_unknown, ["activities", 0, "steps", 0]),
    ("step may not start an activity", _step_activity, ["activities", 0, "steps", 0]),
    ("max steps per activity", _too_many_steps, []),
    ("key name", lambda c: c["keys"].update({"nokey": IR}), ["keys"]),
    (
        "key action unknown device",
        lambda c: c["keys"].update({"play": {"device": "ghost", "command": "x"}}),
        ["keys", "play"],
    ),
    ("bt usage 0..65535", lambda c: c["keys"].update({"play": {"bt": 70000}}), ["keys", "play"]),
    (
        "ha service domain allow-list",
        lambda c: c["keys"].update({"play": {"ha": {**HA, "service": "shell_command.run"}}}),
        ["keys", "play"],
    ),
    ("ha max 8 entities", _ha_entities, ["keys", "play"]),
    (
        "ha entity id pattern",
        lambda c: c["keys"].update({"play": {"ha": {**HA, "target": {"entity_id": "Light.Lamp"}}}}),
        ["keys", "play"],
    ),
    ("ha no templates", _ha_template, ["keys", "play"]),
    ("ha data depth 4", _ha_deep, ["keys", "play"]),
    ("ha data 1024 bytes", _ha_big, ["keys", "play"]),
    (
        "ha repeat is boolean",
        lambda c: c["keys"].update({"play": {"ha": {**HA, "repeat": "yes"}}}),
        ["keys", "play"],
    ),
    ("page id pattern", _set(["pages", 4, "id"], "Bad Id"), ["pages", 4, "id"]),
    ("page title 24 bytes", _set(["pages", 4, "title"], "t" * 25), ["pages", 4, "title"]),
    ("page cols 1..16", _set(["pages", 4, "cols"], 17), ["pages", 4, "cols"]),
    ("duplicate page id", lambda c: c["pages"][5].update(id="watch"), ["pages", 5, "id"]),
    (
        "unknown activity on a page",
        _set(["pages", 4, "activity"], "ghost"),
        ["pages", 4, "activity"],
    ),
    ("max 32 widgets per page", _many_widgets, ["pages", 4]),
    ("max 32 pages", _many_pages, []),
    ("max 128 soft keys", _many_soft, []),
    ("widget label 24 bytes", _long_label, ["pages", 4]),
    ("page 251 bytes", _over_budget, ["pages", 4]),
    (
        "widget position 0..15",
        _set(["pages", 4, "widgets", 0, "x"], 16),
        ["pages", 4, "widgets", 0, "x"],
    ),
    (
        "widget id pattern",
        _set(["pages", 4, "widgets", 0, "id"], "Bad Id"),
        ["pages", 4, "widgets", 0, "id"],
    ),
    ("duplicate widget id", _dup_widget, ["pages", 4, "widgets", 1, "id"]),
    (
        "button needs action",
        lambda c: pg(c)["widgets"][0].pop("action"),
        ["pages", 4, "widgets", 0],
    ),
    ("tile needs entity", lambda c: pg(c)["widgets"][2].pop("entity"), ["pages", 4, "widgets", 2]),
    (
        "tile entity pattern",
        _set(["pages", 4, "widgets", 2, "entity"], "Sensor Bad"),
        ["pages", 4, "widgets", 2, "entity"],
    ),
    ("tap toggle needs a toggleable entity", _tile_sensor_toggle, ["pages", 4, "widgets", 2]),
    (
        "unknown icon",
        _set(["pages", 4, "widgets", 0, "icon"], "nope"),
        ["pages", 4, "widgets", 0, "icon"],
    ),
    (
        "role values",
        _set(["pages", 4, "widgets", 0, "role"], "huge"),
        ["pages", 4, "widgets", 0, "role"],
    ),
    (
        "shape values",
        _set(["pages", 4, "widgets", 0, "shape"], "blob"),
        ["pages", 4, "widgets", 0, "shape"],
    ),
    ("nav next on the last page", _nav_next_last, ["pages"]),
    ("nav home needs a home page", _nav_home_missing, ["pages"]),
    (
        "nav to unknown page",
        _set(["pages", 4, "widgets", 4, "nav"], "ghost"),
        ["pages", 4, "widgets", 4, "nav"],
    ),
    ("one home page", _second_home, ["pages", 1, "kind"]),
    ("list page has no x/y", _list_xy, ["pages", 2, "widgets", 0]),
    ("list rows 1..16", _set(["pages", 2, "list", "rows"], 17), ["pages", 2]),
    ("editor size", _editor_big, ["editor"]),
]


def run(mutate):
    c = copy.deepcopy(BASE)
    mutate(c)
    return _preview(c)["errors"]


@pytest.mark.parametrize("name,mutate,prefix", CASES, ids=[c[0] for c in CASES])
def test_the_backend_rejects_it_with_an_error_at_the_expected_place(name, mutate, prefix):
    errors = run(mutate)
    assert errors, f"{name}: the backend accepted it"
    assert any(e["path"][: len(prefix)] == prefix for e in errors), (
        name,
        [e["path"] for e in errors][:4],
    )


def test_the_table_the_frontend_checks_is_current():
    table = []
    for name, mutate, prefix in CASES:
        errs = [e for e in run(mutate) if e["path"][: len(prefix)] == prefix] or run(mutate)
        e = errs[0]
        table.append({"rule": name, "path": e["path"], "msg": e["msg"][:160]})
    text = json.dumps(table, indent=1) + "\n"
    if os.environ.get("UPDATE_RULES"):
        TABLE.write_text(text)
    assert TABLE.read_text() == text, (
        "run UPDATE_RULES=1 uv run pytest tests/py/test_editor_rules.py"
    )
