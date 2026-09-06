#!/bin/sh

set -eu

output=${1:-html/tlds-alpha-by-domain.txt}
temporary="${output}.tmp"
url=https://data.iana.org/TLD/tlds-alpha-by-domain.txt

trap 'rm -f "$temporary"' EXIT HUP INT TERM
curl -fsS "$url" -o "$temporary"
grep -q '^# Version ' "$temporary"
mv "$temporary" "$output"
trap - EXIT HUP INT TERM

echo "Updated IANA TLD list: $output"
