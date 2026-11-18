"""docs/addon-api.md must list every route the add-on serves (and nothing it does not)."""

import re
import tempfile
from pathlib import Path

from test_addon import router_routes

from harmony_addon.app import create_app
from harmony_addon.auth import NoAuth

DOC = Path(__file__).resolve().parents[2] / "docs" / "addon-api.md"
ROW = re.compile(
    r"^\| (GET|POST|PUT|PATCH|DELETE) \| `(/[^`]*)` \| (ingress|bearer) \|", re.MULTILINE
)


def test_every_route_is_in_the_api_doc_with_the_right_auth_class():
    with tempfile.TemporaryDirectory() as d:
        served = router_routes(create_app(d, auth=NoAuth()))
    rows = ROW.findall(DOC.read_text())
    documented = {(m, p) for m, p, _ in rows}
    assert len(documented) == len(rows), "a route is listed twice"
    assert set(served) == documented, (
        f"missing from the doc: {sorted(set(served) - documented)}; "
        f"documented but not served: {sorted(documented - set(served))}"
    )
    for method, path, auth in rows:
        assert auth == ("bearer" if path.startswith("/api/v1/bundle") else "ingress"), path
