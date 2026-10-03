#!/bin/bash
# 配布用のディスクイメージ dist/RawHDR-Composer-<版>.dmg を作る。
#   scripts/make_dmg.sh            アプリを作り直してから作る
#   scripts/make_dmg.sh --no-build dist/RawHDR Composer.app をそのまま使う
#
# 中身: RawHDR Composer.app・使用説明書.pdf・かんたんインストーラ.scpt・「アプリケーション」への別名
# （別名にアプリをドラッグしても入れられる）。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
DIST_DIR="${PROJECT_DIR}/dist"
APP="${DIST_DIR}/RawHDR Composer.app"
VERSION="$(sed -n 's/^project(RawHDRComposer VERSION \([0-9.]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")"
VOLNAME="RawHDR Composer ${VERSION}"
DMG="${DIST_DIR}/RawHDR-Composer-${VERSION}.dmg"
WORK="${PROJECT_DIR}/.build/dmg"
STAGE="${WORK}/stage"
MANUAL_DIR="${PROJECT_DIR}/docs/manual"

if [ "${1:-}" != "--no-build" ]; then
    "${SCRIPT_DIR}/build_app.sh"
fi
[ -d "${APP}" ] || { echo "エラー: ${APP} がありません" >&2; exit 1; }
APP_VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "${APP}/Contents/Info.plist")"
[ "${APP_VERSION}" = "${VERSION}" ] || { echo "エラー: アプリの版 ${APP_VERSION} と CMakeLists.txt の版 ${VERSION} が違います" >&2; exit 1; }

# 使用説明書（LuaLaTeX があれば組み直す。無ければ今の PDF を使う）。
if command -v lualatex >/dev/null 2>&1; then
    ( cd "${MANUAL_DIR}" && lualatex -interaction=nonstopmode manual.tex >/dev/null && lualatex -interaction=nonstopmode manual.tex >/dev/null ) \
        || echo "注意: 使用説明書を組み直せませんでした（今の PDF を使います）" >&2
fi

rm -rf "${WORK}"
mkdir -p "${STAGE}"
ditto "${APP}" "${STAGE}/RawHDR Composer.app"
cp "${MANUAL_DIR}/manual.pdf" "${STAGE}/使用説明書.pdf"
# インストーラの文言の版（@VERSION@）を埋めてからコンパイルする。
sed "s/@VERSION@/${VERSION}/g" "${SCRIPT_DIR}/installer/かんたんインストーラ.applescript" > "${WORK}/installer.applescript"
osacompile -o "${STAGE}/かんたんインストーラ.scpt" "${WORK}/installer.applescript"
ln -s /Applications "${STAGE}/アプリケーション"

# 書き込めるイメージを作り、Finder の表示（アイコンの並び）を整えてから、圧縮したイメージにする。
# HFS+ にする（古い macOS でも確実に読めるように）。
RW="${WORK}/rw.dmg"
hdiutil create -quiet -volname "${VOLNAME}" -srcfolder "${STAGE}" -fs HFS+ -format UDRW -ov "${RW}"
MOUNT="$(hdiutil attach -readwrite -noverify -noautoopen "${RW}" | awk -F '\t' '/\/Volumes\//{print $NF; exit}')"
[ -d "${MOUNT}" ] || { echo "エラー: イメージを開けません" >&2; exit 1; }
DISK_NAME="$(basename "${MOUNT}")"
# Finder の操作の許可が無い環境では並べ替えをとばす（中身は同じ）。
osascript <<EOF || echo "注意: Finder の表示を整えられませんでした（アイコンの並びは既定のまま）" >&2
tell application "Finder"
    tell disk "${DISK_NAME}"
        open
        delay 1
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set opts to the icon view options of container window
        set arrangement of opts to not arranged
        set icon size of opts to 112
        set text size of opts to 13
        set position of item "RawHDR Composer.app" of container window to {150, 120}
        set position of item "アプリケーション" of container window to {470, 120}
        set position of item "かんたんインストーラ.scpt" of container window to {150, 300}
        set position of item "使用説明書.pdf" of container window to {470, 300}
        set the bounds of container window to {200, 120, 820, 580}
        close
        -- 開き直して設定を確定させる（一度で保存されないことがある）。
        open
        update without registering applications
        delay 2
        close
    end tell
end tell
EOF
# Finder が表示の設定（.DS_Store）を書き終えるまで待つ。
for i in $(seq 1 20); do [ -f "${MOUNT}/.DS_Store" ] && break; sleep 0.5; done
sleep 1
sync
hdiutil detach -quiet "${MOUNT}" || hdiutil detach -force -quiet "${MOUNT}"

rm -f "${DMG}"
hdiutil convert -quiet "${RW}" -format UDZO -imagekey zlib-level=9 -o "${DMG}"
hdiutil verify -quiet "${DMG}"
rm -rf "${WORK}"
echo "完了: ${DMG}（$(du -h "${DMG}" | cut -f1)）"
