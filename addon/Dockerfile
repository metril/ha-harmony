# Build context is the repository root:
#   docker build -f addon/Dockerfile --build-arg BUILD_FROM=<base> .
# Default = amd64, pinned by digest (bump together with addon/build.yaml).
ARG BUILD_FROM=ghcr.io/home-assistant/amd64-base-python:3.14-alpine3.23@sha256:af4b51f68688a99e6d5bf5d7a899af078ac093523c4632baa176ca5cb8a0eb1b

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
# s6-overlay (the base image's init) stays root; only the server drops to this user.
RUN adduser -D -H -u 10001 -s /sbin/nologin app \
 && python -m compileall -q /opt/app /opt/deps
COPY --chmod=755 addon/rootfs/ /
ENV PYTHONPATH=/opt/app:/opt/deps PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
EXPOSE 8099
