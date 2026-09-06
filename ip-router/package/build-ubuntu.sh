#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPOSITORY_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
INSTALL_ROOT="/opt/soft/ip-router"
VERSION="${1:-${IP_ROUTER_VERSION:-$(tr -d '[:space:]' < "$SCRIPT_DIR/VERSION")}}"
OUTPUT_DIR="${2:-$SCRIPT_DIR/dist}"

if [[ "$(uname -s)" != "Linux" ]] || [[ ! -f /etc/os-release ]] \
	|| ! grep -Eq '(^ID=ubuntu$|^ID_LIKE=.*ubuntu)' /etc/os-release; then
	echo "This script must run on Ubuntu." >&2
	exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9][0-9A-Za-z.+:~-]*$ ]]; then
	echo "Invalid Debian package version: $VERSION" >&2
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
if ! file "$PROJECT_DIR/ip-router" | grep -q 'ELF'; then
	echo "The built ip-router binary is not a Linux ELF executable." >&2
	exit 1
fi
if ldd "$PROJECT_DIR/ip-router" 2>/dev/null | grep 'not found' >/dev/null; then
	echo "The ip-router binary has unresolved shared-library dependencies." >&2
	ldd "$PROJECT_DIR/ip-router" >&2 || true
	exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
ARCH="$(dpkg --print-architecture)"
PACKAGE_FILE="$OUTPUT_DIR/ip-router_${VERSION}_${ARCH}.deb"
STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ip-router-ubuntu.XXXXXX")"
trap 'rm -rf "$STAGE_DIR"' EXIT

PAYLOAD_ROOT="$STAGE_DIR/root$INSTALL_ROOT"
DEBIAN_DIR="$STAGE_DIR/root/DEBIAN"
install -d -m 755 "$PAYLOAD_ROOT/bin" "$PAYLOAD_ROOT/sbin" \
	"$PAYLOAD_ROOT/conf" "$PAYLOAD_ROOT/var/html" "$PAYLOAD_ROOT/var/log" \
	"$PAYLOAD_ROOT/var/pid" "$PAYLOAD_ROOT/share/doc" "$DEBIAN_DIR"
install -m 755 "$PROJECT_DIR/ip-router" "$PAYLOAD_ROOT/sbin/ip-router"
install -m 755 "$SCRIPT_DIR/ip-router-service.sh" "$PAYLOAD_ROOT/bin/ip-router-service"
sed "s;{install_path};$INSTALL_ROOT;g" "$PROJECT_DIR/ip-router.cf" \
	> "$PAYLOAD_ROOT/conf/ip-router.cf.default"
chmod 600 "$PAYLOAD_ROOT/conf/ip-router.cf.default"
install -m 644 "$PROJECT_DIR/html/index.html" "$PAYLOAD_ROOT/var/html/index.html"
install -m 644 "$PROJECT_DIR/html/tlds-alpha-by-domain.txt" \
	"$PAYLOAD_ROOT/var/html/tlds-alpha-by-domain.txt"
install -m 644 "$REPOSITORY_DIR/README.md" "$PAYLOAD_ROOT/share/doc/README.md"
install -m 755 "$SCRIPT_DIR/debian/preinst" "$DEBIAN_DIR/preinst"
install -m 755 "$SCRIPT_DIR/debian/postinst" "$DEBIAN_DIR/postinst"
install -m 755 "$SCRIPT_DIR/debian/prerm" "$DEBIAN_DIR/prerm"
install -m 755 "$SCRIPT_DIR/debian/postrm" "$DEBIAN_DIR/postrm"
chmod 1777 "$PAYLOAD_ROOT/var/log"

MAINTAINER="${PACKAGE_MAINTAINER:-ACL ip-router maintainers}"
printf '%s\n' \
	"Package: ip-router" \
	"Version: $VERSION" \
	"Section: net" \
	"Priority: optional" \
	"Architecture: $ARCH" \
	"Maintainer: $MAINTAINER" \
	"Description: Host-route and split-DNS management service for acl-master" \
	" Installs ip-router under /opt/soft/ip-router and registers it with acl-master." \
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
