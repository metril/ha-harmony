# Build context is the repository root:
#   docker build -f addon/Dockerfile --build-arg BUILD_FROM=<base> .
# Default = amd64, pinned by digest (bump together with addon/build.yaml).
ARG BUILD_FROM=ghcr.io/home-assistant/amd64-base-python:3.14-alpine3.23@sha256:af4b51f68688a99e6d5bf5d7a899af078ac093523c4632baa176ca5cb8a0eb1b

# Frontend build stage: node is pinned by digest (node:24-alpine); only dist/ reaches the image.
FROM node:24-alpine@sha256:116b800c3754b440ee0aa3ed2f20f5bf3d99a554bfd9adeea09ea8017f6e85f5 AS frontend
# Same relative layout as the repo: vite aliases ../../harmony_addon/frontend-shared (icons.json).
WORKDIR /repo/addon/frontend
COPY addon/frontend/package.json addon/frontend/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY harmony_addon/frontend-shared /repo/harmony_addon/frontend-shared
# tsc type-checks the tests, which import the shared page vectors.
COPY tests/vectors /repo/tests/vectors
COPY addon/frontend/ ./
RUN npm run build

FROM ${BUILD_FROM} AS deps
COPY addon/requirements.txt /tmp/requirements.txt
# Hash-locked, wheels only: nothing is compiled or fetched unpinned.
RUN pip install --no-cache-dir --require-hashes --only-binary=:all: \
      --target /opt/deps -r /tmp/requirements.txt

FROM ${BUILD_FROM}
COPY --from=deps /opt/deps /opt/deps
COPY harmony_core /opt/app/harmony_core
COPY harmony_irdb /opt/app/harmony_irdb
COPY harmony_addon /opt/app/harmony_addon
# The signed hub image + hub.json when CI bundled one (addon/firmware/ otherwise holds only .gitkeep).
COPY addon/firmware/ /opt/app/firmware/
COPY --from=frontend /repo/addon/frontend/dist /opt/app/frontend
# s6-overlay (the base image's init) stays root; only the server drops to this user.
RUN adduser -D -H -u 10001 -s /sbin/nologin app \
 && python -m compileall -q /opt/app /opt/deps
COPY --chmod=755 addon/rootfs/ /
ENV PYTHONPATH=/opt/app:/opt/deps PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
EXPOSE 8099
