#!/usr/bin/env bash
# Build the .deb in a clean container, so it only depends on what
# debian/control declares, never on what happens to be installed here.
#
# Usage: tools/build-deb.sh [IMAGE]     (default: ubuntu:26.04)
#   e.g. tools/build-deb.sh debian:trixie
# The packages land in dist/<image>/.
set -euo pipefail

IMAGE=${1:-ubuntu:26.04}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/dist/${IMAGE//[:\/]/-}"
mkdir -p "$OUT"

docker run --rm -v "$ROOT":/src:ro -v "$OUT":/out -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
	"$IMAGE" bash -euo pipefail -c '
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends build-essential devscripts equivs lintian >/dev/null
	cp -r /src /build && cd /build && rm -rf build obj-* dist
	mk-build-deps --install --remove --tool "apt-get -y -qq --no-install-recommends" debian/control >/dev/null
	dpkg-buildpackage -us -uc -b
	lintian --info ../giac-kernel_*.deb || true
	cp ../giac-kernel_*.deb ../giac-kernel_*.buildinfo ../giac-kernel_*.changes /out/
	chown "$HOST_UID:$HOST_GID" /out/*
'
ls -l "$OUT"
