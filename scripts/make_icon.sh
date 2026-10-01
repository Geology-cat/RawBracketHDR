#!/bin/bash
# app/resources/icon/icon_1024.png（1024×1024、背景透明）から app/resources/AppIcon.icns を作る。
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RES="${SCRIPT_DIR}/../app/resources"
SRC="${RES}/icon/icon_1024.png"
SET="$(mktemp -d)/AppIcon.iconset"
mkdir -p "${SET}"
for s in 16 32 128 256 512; do
    sips -z "${s}" "${s}" "${SRC}" --out "${SET}/icon_${s}x${s}.png" >/dev/null
    d=$((s * 2))
    sips -z "${d}" "${d}" "${SRC}" --out "${SET}/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "${SET}" -o "${RES}/AppIcon.icns"
echo "作成: ${RES}/AppIcon.icns"
