#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPOSITORY_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
INSTALL_ROOT="/opt/soft/dns-gate"
VERSION="${1:-${DNS_GATE_VERSION:-$(tr -d '[:space:]' < "$SCRIPT_DIR/VERSION")}}"
OUTPUT_DIR="${2:-$SCRIPT_DIR/dist}"
DBIP_DATABASE="${DBIP_DATABASE:-$PROJECT_DIR/dbip-country-lite.mmdb}"

if [[ "$(uname -s)" != "Linux" ]] || [[ ! -f /etc/os-release ]] \
	|| ! grep -Eq '(^ID=ubuntu$|^ID_LIKE=.*ubuntu)' /etc/os-release; then
	echo "This script must run on Ubuntu." >&2
	exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9][0-9A-Za-z.+:~-]*$ ]]; then
	echo "Invalid Debian package version: $VERSION" >&2
	exit 1
fi
if [[ ! -s "$DBIP_DATABASE" ]]; then
	echo "DB-IP Country Lite database not found: $DBIP_DATABASE" >&2
	echo "Run '$PROJECT_DIR/update-dbip.sh' before building the package." >&2
	exit 1
fi
for command in make dpkg-deb dpkg file sed install; do
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "Required command not found: $command" >&2
		exit 1
	fi
done

if [[ -n "${BUILD_JOBS:-}" ]]; then
	JOBS="$BUILD_JOBS"
else
	JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
fi
if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
	make -C "$PROJECT_DIR" clean
	make -C "$PROJECT_DIR" -j "$JOBS" all
fi
if ! file "$PROJECT_DIR/dns-gate" | grep -q 'ELF'; then
	echo "The built dns-gate binary is not a Linux ELF executable." >&2
	exit 1
fi
if ldd "$PROJECT_DIR/dns-gate" 2>/dev/null | grep 'not found' >/dev/null; then
	echo "The dns-gate binary has unresolved shared-library dependencies." >&2
	ldd "$PROJECT_DIR/dns-gate" >&2 || true
	exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
ARCH="$(dpkg --print-architecture)"
PACKAGE_FILE="$OUTPUT_DIR/dns-gate_${VERSION}_${ARCH}.deb"
STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dns-gate-ubuntu.XXXXXX")"
trap 'rm -rf "$STAGE_DIR"' EXIT

PAYLOAD_ROOT="$STAGE_DIR/root$INSTALL_ROOT"
DEBIAN_DIR="$STAGE_DIR/root/DEBIAN"
install -d -m 755 "$PAYLOAD_ROOT/bin" "$PAYLOAD_ROOT/sbin" \
	"$PAYLOAD_ROOT/conf" "$PAYLOAD_ROOT/sh" "$PAYLOAD_ROOT/var/log" \
	"$PAYLOAD_ROOT/var/pid" "$PAYLOAD_ROOT/share/doc" \
	"$PAYLOAD_ROOT/share/dbip" "$DEBIAN_DIR"
install -m 755 "$PROJECT_DIR/dns-gate" "$PAYLOAD_ROOT/sbin/dns-gate"
install -m 755 "$SCRIPT_DIR/dns-gate-service.sh" "$PAYLOAD_ROOT/bin/dns-gate-service"
install -m 755 "$PROJECT_DIR/update-dbip.sh" "$PAYLOAD_ROOT/sh/update-dbip.sh"
sed "s;{install_path};$INSTALL_ROOT;g" "$PROJECT_DIR/dns-gate.cf" \
	> "$PAYLOAD_ROOT/conf/dns-gate.cf.default"
chmod 600 "$PAYLOAD_ROOT/conf/dns-gate.cf.default"
install -m 644 "$DBIP_DATABASE" "$PAYLOAD_ROOT/share/dbip/dbip-country-lite.mmdb"
install -m 644 "$REPOSITORY_DIR/README.md" "$PAYLOAD_ROOT/share/doc/README.md"
install -m 644 "$SCRIPT_DIR/DBIP-LICENSE.txt" "$PAYLOAD_ROOT/share/doc/DBIP-LICENSE.txt"
install -m 755 "$SCRIPT_DIR/debian/preinst" "$DEBIAN_DIR/preinst"
install -m 755 "$SCRIPT_DIR/debian/postinst" "$DEBIAN_DIR/postinst"
install -m 755 "$SCRIPT_DIR/debian/prerm" "$DEBIAN_DIR/prerm"
install -m 755 "$SCRIPT_DIR/debian/postrm" "$DEBIAN_DIR/postrm"
chmod 1777 "$PAYLOAD_ROOT/var/log"

MAINTAINER="${PACKAGE_MAINTAINER:-ACL dns-gate maintainers}"
printf '%s\n' \
	"Package: dns-gate" \
	"Version: $VERSION" \
	"Section: net" \
	"Priority: optional" \
	"Architecture: $ARCH" \
	"Maintainer: $MAINTAINER" \
	"Recommends: curl, gzip" \
	"Description: DNS proxy with GeoIP route integration for acl-master" \
	" Installs dns-gate under /opt/soft/dns-gate and registers it with acl-master." \
	> "$DEBIAN_DIR/control"

if dpkg-deb --help 2>&1 | grep -- '--root-owner-group' >/dev/null; then
	dpkg-deb --root-owner-group --build "$STAGE_DIR/root" "$PACKAGE_FILE"
elif [[ "$(id -u)" = "0" ]]; then
	dpkg-deb --build "$STAGE_DIR/root" "$PACKAGE_FILE"
elif command -v fakeroot >/dev/null 2>&1; then
	fakeroot dpkg-deb --build "$STAGE_DIR/root" "$PACKAGE_FILE"
else
	echo "dpkg-deb lacks --root-owner-group; install fakeroot or run as root." >&2
	exit 1
fi

echo "Created Ubuntu installer: $PACKAGE_FILE"
echo "Install with: sudo apt install '$PACKAGE_FILE'"
