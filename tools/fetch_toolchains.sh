#!/bin/sh
# Provision the cross toolchains into a durable location.
#
#   hub    OpenWrt 24.10.0 ath79 SDK toolchain (gcc 13.3, musl, mips_24kc, SOFT
#          float). The hub is an AR9330 (MIPS 24Kc) with no FPU and a kernel
#          without FP emulation, so a hard-float binary dies with SIGILL.
#          Bootlin publishes no soft-float big-endian mips32 toolchain.
#   remote Bootlin armv5-eabi MUSL, not uclibc. The Elite runs Linux 2.6.37, and
#          uClibc-ng 1.0.54 calls statx (4.11+) and clock_gettime64 (5.1+) with
#          no fallback, so stat/opendir/clock_gettime all fail with ENOSYS there.
#          musl retries the old syscalls on ENOSYS (verified on the Elite).
#
# The pins here must match .github/workflows/ci.yml, so a local cross-build and
# CI use the same compiler. Installs to vendor/toolchains/<arch>/ (gitignored),
# and reuses an already-downloaded tarball from --cache-dir if one is there.
#
#   tools/fetch_toolchains.sh [hub|remote|all] [--cache-dir DIR]
#
# Then build the hub with:
#   HP_TOOLCHAIN_ROOT=vendor/toolchains/mips_24kc cmake -S . -B build-cross \
#       -DCMAKE_BUILD_TYPE=Release -DHP_ALLOW_DEV_OTA_KEY=ON \
#       -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/toolchains/hub-mips.cmake
#   cmake --build build-cross -j
set -eu

REPO=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
DEST="$REPO/vendor/toolchains"
CACHE=""
WHAT=all

# Keep in step with ci.yml: arch_dir, toolchain name, sha256.
MIPS_ARCH=mips_24kc
MIPS_URL=https://downloads.openwrt.org/releases/24.10.0/targets/ath79/generic/openwrt-toolchain-24.10.0-ath79-generic_gcc-13.3.0_musl.Linux-x86_64.tar.zst
MIPS_SHA=ef3087ea607f52f5127c9d0bd74581f78ba26093f29b6330951117676eb66207
ARM_ARCH=armv5-eabi
ARM_TC=armv5-eabi--musl--stable-2025.08-1
ARM_SHA=8cdb4ad70c6b5a66427fa3315fe3ddde1c19c90232674dec59045e04d2a36cf1
ARM_URL=https://toolchains.bootlin.com/downloads/releases/toolchains/armv5-eabi/tarballs/$ARM_TC.tar.xz

while [ $# -gt 0 ]; do
	case "$1" in
	hub | remote | all) WHAT=$1 ;;
	--cache-dir)
		shift
		CACHE=$1
		;;
	*)
		echo "usage: $0 [hub|remote|all] [--cache-dir DIR]" >&2
		exit 2
		;;
	esac
	shift
done

# install_tc <dir> <url> <sha256> <strip-components> <tar-flag>
install_tc() {
	arch=$1 url=$2 sha=$3 strip=$4 flag=$5
	root="$DEST/$arch"
	if ls "$root"/bin/*-gcc >/dev/null 2>&1; then
		echo "[=] $arch already installed at $root"
		return 0
	fi

	file=${url##*/}
	tarball="$DEST/$file"
	if [ -n "$CACHE" ] && [ -f "$CACHE/$file" ]; then
		echo "[*] using cached tarball $CACHE/$file"
		tarball="$CACHE/$file"
	elif [ ! -f "$tarball" ]; then
		echo "[*] downloading $url"
		mkdir -p "$DEST"
		curl -fsSL --retry 3 -o "$tarball" "$url"
	fi

	echo "$sha  $tarball" | sha256sum -c -
	mkdir -p "$root"
	echo "[*] extracting into $root"
	tar $flag -xf "$tarball" -C "$root" --strip-components="$strip"
	[ "$tarball" = "$DEST/$file" ] && rm -f "$tarball"
	echo "[+] $arch ready: $root"
}

case "$WHAT" in
hub) install_tc "$MIPS_ARCH" "$MIPS_URL" "$MIPS_SHA" 2 --zstd ;;
remote) install_tc "$ARM_ARCH" "$ARM_URL" "$ARM_SHA" 1 -J ;;
all)
	install_tc "$MIPS_ARCH" "$MIPS_URL" "$MIPS_SHA" 2 --zstd
	install_tc "$ARM_ARCH" "$ARM_URL" "$ARM_SHA" 1 -J
	;;
esac

for d in "$DEST"/*/; do
	[ -d "$d" ] || continue
	for gcc in "$d"bin/*-gcc; do
		[ -x "$gcc" ] || continue
		printf '%s -> ' "$gcc"
		"$gcc" -dumpversion
	done
done
