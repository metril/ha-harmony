#!/bin/sh
# Build the add-on image (amd64) and smoke-test it: health 200 from the ingress gateway IP, 403
# from any other peer, server running as a non-root user, draft write under /data works.
#   tools/addon_smoke.sh [image-tag]
set -eu
cd "$(dirname "$0")/.."
IMG="${1:-ha-harmony-editor:smoke}"
NET="hhsmoke$$"
CT="hhsmoke-srv-$$"
VOL="hhsmoke-data-$$"
BASE="$(sed -n 's/^  amd64: //p' addon/build.yaml)"

cleanup() {
  docker rm -f "$CT" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  docker volume rm "$VOL" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker build -q -f addon/Dockerfile --build-arg "BUILD_FROM=$BASE" -t "$IMG" .
docker network create --subnet 172.30.32.0/23 "$NET" >/dev/null
docker volume create "$VOL" >/dev/null
docker run -d --name "$CT" --network "$NET" --ip 172.30.32.10 -v "$VOL:/data" "$IMG" >/dev/null

probe() { # <client ip> <method> <path> [body] -> prints the HTTP status
  docker run --rm -e "NOUSER=${NOUSER:-}" --network "$NET" --ip "$1" --entrypoint python "$IMG" -c '
import os, sys, urllib.request, urllib.error
ip, method, path, body = sys.argv[1:5]
req = urllib.request.Request("http://172.30.32.10:8099" + path, method=method,
    data=body.encode() if body else None,
    headers={"Content-Type": "application/json", "X-HH-CSRF": "1", "X-Ingress-Path": "/x"})
if not os.environ.get("NOUSER"):
    req.add_header("X-Remote-User-Id", "u1")
try:
    print(urllib.request.urlopen(req, timeout=5).status)
except urllib.error.HTTPError as e:
    print(e.code)
' "$1" "$2" "$3" "${4:-}"
}

# wait for the server (probe from the ingress IP)
n=0
until [ "$(probe 172.30.32.2 GET /api/v1/health 2>/dev/null)" = 200 ]; do
  n=$((n + 1))
  [ "$n" -lt 30 ] || { docker logs "$CT"; echo "FAIL: server did not come up"; exit 1; }
  sleep 1
done

fail=0
check() { # <what> <got> <want>
  if [ "$2" = "$3" ]; then echo "ok   $1 = $2"; else echo "FAIL $1 = $2 (want $3)"; fail=1; fi
}
check "health from ingress IP" "$(probe 172.30.32.2 GET /api/v1/health)" 200
check "health from other peer" "$(probe 172.30.32.3 GET /api/v1/health)" 403
check "UI from other peer" "$(probe 172.30.32.3 GET /)" 403
check "UI from ingress IP" "$(probe 172.30.32.2 GET /)" 200
check "ingress without a user id" "$(NOUSER=1 probe 172.30.32.2 GET /api/v1/health)" 401
asset="$(docker exec "$CT" sh -c 'ls /opt/app/frontend/assets | grep "\.js$"' | head -1)"
check "built bundle present in image" "$([ -n "$asset" ] && echo yes || echo no)" yes
check "bundle from ingress IP" "$(probe 172.30.32.2 GET "/assets/$asset")" 200
check "draft write from ingress IP" \
  "$(probe 172.30.32.2 PUT /api/v1/config/draft '{"config":{}}')" 200
user="$(docker exec "$CT" sh -c "ps -o user,args | grep '[h]armony_addon' | awk '{print \$1}'")"
check "server user" "$user" app
[ "$fail" = 0 ] || { docker logs "$CT"; exit 1; }
