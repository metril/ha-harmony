"""Test helpers for the config library routes (docs/addon-api.md "Configs")."""

H = {"X-HH-CSRF": "1"}


async def ensure(c, cid="main"):
    """Create library config `cid` (its name slugs to the id) unless it exists."""
    if (await c.get(f"/api/v1/configs/{cid}")).status == 404:
        r = await c.post("/api/v1/configs", json={"name": cid}, headers=H)
        assert r.status == 201, await r.text()


async def put_draft(c, config, cid="main", **kw):
    await ensure(c, cid)
    return await c.put(f"/api/v1/configs/{cid}/draft", json={"config": config}, headers=H, **kw)


async def apply_draft(c, hub="default", cid="main", rev=None):
    body = {"config_id": cid, **({} if rev is None else {"rev": rev})}
    return await c.post(f"/api/v1/hubs/{hub}/apply", json=body, headers=H)


async def put_apply(c, config, hub="default", cid="main"):
    """Store `config` as the draft of `cid` and apply it to `hub`; the apply response."""
    r = await put_draft(c, config, cid)
    assert r.status == 200, await r.text()
    return await apply_draft(c, hub, cid)
