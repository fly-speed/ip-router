#!/bin/sh

set -eu

output=${1:-dbip-country-lite.mmdb}
edition=${2:-$(date -u +%Y-%m)}
url="https://download.db-ip.com/free/dbip-country-lite-${edition}.mmdb.gz"
compressed="${output}.download.$$"
temporary="${output}.tmp.$$"

cleanup() {
	rm -f "$compressed" "$temporary"
}
trap cleanup EXIT HUP INT TERM

echo "Downloading $url"
curl -fL "$url" -o "$compressed"
gzip -dc "$compressed" > "$temporary"
mv -f "$temporary" "$output"
echo "Updated $output"
