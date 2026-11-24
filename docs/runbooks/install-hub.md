# Runbook: install the hub firmware from a release

For a new user with a rooted hub. You do not build anything: the release carries the signed hub, and the tool checks the
signature offline before it writes a byte. From then on updates arrive in Home Assistant (`ota-update.md`).
Steps marked `[WRITE]` change the hub; every one needs `--yes`.

## Before you start

- The hub is rooted with your own SSH key (`phase1-hub.md`: root tool, your key, for example `~/.ssh/id_ed25519.pub`; the key is yours,
  nothing here depends on the owner's) and `ssh harmony-hub` works (or pass `--host`, `--user`, `--key`).
- You have this repository checked out at the release you are installing (the tool reads the release public key from
  `common/ota/ota_pubkey_release.h`), and `python3` (no extra packages).
- The Harmony Editor add-on and the integration are installed (`README.md`); the add-on gives you the token for the next step.

## 1. Install the daemon `[WRITE]`

```
python3 tools/deploy_hub.py --host harmony-hub push --release X.Y.Z --busybox <static-mips-busybox> --yes
```

or, with a file you downloaded from the release page (`hub-X.Y.Z.img`):

```
python3 tools/deploy_hub.py push --image hub-X.Y.Z.img --busybox <static-mips-busybox> --yes
```

The tool refuses the image, before connecting for any write, unless it is a complete hub image signed with the release key
(a dev-signed, tampered, truncated or remote image is refused). It strips the header, uploads the daemon to `/data/ha-harmony/hub`
(md5 verified, atomic replace) and records the build in `/data/ha-harmony/ota-build`. It prints the daemon's md5 for the next step.

## 2. Arm the boot hook `[WRITE]`

Follow `phase4-persistence.md` (token file, optional `--rf-slot`):

```
python3 tools/install_boot_hook.py install --yes --token-file <token-file> --expect-md5 <md5 printed above>
```

The hub now starts the daemon at boot and supervises it.

## 3. Updates

Add the hub to the integration (config flow). The **Firmware** update entity appears; when a newer add-on is installed it offers the new
build, and Install uploads it, the hub verifies, restarts and rolls back by itself if the new build is not healthy.

## Notes

- Only images signed with the release key install this way, and only such a hub accepts the update entity's images. A hub you built or
  signed yourself (dev key, `--dev`, or your own key) cannot take release updates; the integration refuses to try on a hub that trusts
  the dev key.
- Space: `/data` needs about twice the image free for an update (`ota_no_space`): see `ota-update.md`.
- The SSH key (root access) and the OTA key (image signatures) are unrelated; yours is the SSH key, the release key is the project's.
