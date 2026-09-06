#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPOSITORY_DIR="$(cd "$PROJECT_DIR/.." && pwd)"
INSTALL_ROOT="/opt/soft/dns-gate"
VERSION="${1:-${DNS_GATE_VERSION:-$(tr -d '[:space:]' < "$SCRIPT_DIR/VERSION")}}"
OUTPUT_DIR="${2:-$SCRIPT_DIR/dist}"
DBIP_DATABASE="${DBIP_DATABASE:-$PROJECT_DIR/dbip-country-lite.mmdb}"

if [[ "$(uname -s)" != "Darwin" ]]; then
	echo "This script must run on macOS." >&2
	exit 1
fi
if [[ ! "$VERSION" =~ ^[0-9][0-9A-Za-z.+-]*$ ]]; then
	echo "Invalid package version: $VERSION" >&2
	exit 1
fi
if [[ ! -s "$DBIP_DATABASE" ]]; then
	echo "DB-IP Country Lite database not found: $DBIP_DATABASE" >&2
	echo "Run '$PROJECT_DIR/update-dbip.sh' before building the package." >&2
	exit 1
fi
for command in make pkgbuild file sed install; do
	if ! command -v "$command" >/dev/null 2>&1; then
		echo "Required command not found: $command" >&2
		exit 1
	fi
done

if [[ -n "${BUILD_JOBS:-}" ]]; then
	JOBS="$BUILD_JOBS"
else
	JOBS="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')"
fi
if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
	make -C "$PROJECT_DIR" clean
	make -C "$PROJECT_DIR" -j "$JOBS" all
fi
if ! file "$PROJECT_DIR/dns-gate" | grep -q 'Mach-O'; then
	echo "The built dns-gate binary is not a macOS Mach-O executable." >&2
	exit 1
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
ARCH="$(uname -m)"
PACKAGE_FILE="$OUTPUT_DIR/dns-gate-${VERSION}-macos-${ARCH}.pkg"
STAGE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dns-gate-macos.XXXXXX")"
trap 'rm -rf "$STAGE_DIR"' EXIT

PAYLOAD_ROOT="$STAGE_DIR/root$INSTALL_ROOT"
SCRIPTS_DIR="$STAGE_DIR/scripts"
install -d -m 755 "$PAYLOAD_ROOT/bin" "$PAYLOAD_ROOT/sbin" \
	"$PAYLOAD_ROOT/conf" "$PAYLOAD_ROOT/sh" "$PAYLOAD_ROOT/var/log" \
	"$PAYLOAD_ROOT/var/pid" "$PAYLOAD_ROOT/share/doc" \
	"$PAYLOAD_ROOT/share/dbip" "$SCRIPTS_DIR"
install -m 755 "$PROJECT_DIR/dns-gate" "$PAYLOAD_ROOT/sbin/dns-gate"
install -m 755 "$SCRIPT_DIR/dns-gate-service.sh" "$PAYLOAD_ROOT/bin/dns-gate-service"
install -m 755 "$PROJECT_DIR/update-dbip.sh" "$PAYLOAD_ROOT/sh/update-dbip.sh"
sed "s;{install_path};$INSTALL_ROOT;g" "$PROJECT_DIR/dns-gate.cf" \
	> "$PAYLOAD_ROOT/conf/dns-gate.cf.default"
chmod 600 "$PAYLOAD_ROOT/conf/dns-gate.cf.default"
install -m 644 "$DBIP_DATABASE" "$PAYLOAD_ROOT/share/dbip/dbip-country-lite.mmdb"
install -m 644 "$REPOSITORY_DIR/README.md" "$PAYLOAD_ROOT/share/doc/README.md"
install -m 644 "$SCRIPT_DIR/DBIP-LICENSE.txt" "$PAYLOAD_ROOT/share/doc/DBIP-LICENSE.txt"
install -m 755 "$SCRIPT_DIR/macos/preinstall" "$SCRIPTS_DIR/preinstall"
install -m 755 "$SCRIPT_DIR/macos/postinstall" "$SCRIPTS_DIR/postinstall"
chmod 1777 "$PAYLOAD_ROOT/var/log"
if command -v xattr >/dev/null 2>&1; then
	xattr -cr "$STAGE_DIR/root" "$SCRIPTS_DIR" || true
fi
export COPYFILE_DISABLE=1

PKGBUILD_ARGS=(
	--root "$STAGE_DIR/root"
	--scripts "$SCRIPTS_DIR"
	--identifier "com.acl.dns-gate"
	--version "$VERSION"
	--install-location "/"
)
if [[ -n "${PKG_SIGN_IDENTITY:-}" ]]; then
	PKGBUILD_ARGS+=(--sign "$PKG_SIGN_IDENTITY")
fi
pkgbuild "${PKGBUILD_ARGS[@]}" "$PACKAGE_FILE"

echo "Created macOS installer: $PACKAGE_FILE"
echo "Install with: sudo installer -pkg '$PACKAGE_FILE' -target /"
