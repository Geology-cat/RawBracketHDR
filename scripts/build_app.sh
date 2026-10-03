#!/bin/bash
# 検証済みの Universal アプリを dist/RawHDR Composer.app に作る。
# 中間生成物は .build/universal に集め、リポジトリ直下にアプリを散らかさない。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/.build/universal"
DIST_DIR="${PROJECT_DIR}/dist"
SOURCE_APP="${BUILD_DIR}/app/RawHDR Composer.app"
DIST_APP="${DIST_DIR}/RawHDR Composer.app"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || printf '4')"

# Homebrew の clang ではなく Xcode のものを使う（古い配置ターゲットの扱いが違うため）。
export CC="$(xcrun -f clang)"
export CXX="$(xcrun -f clang++)"

# 配置ターゲットは CMakeLists.txt の値を毎回渡す（前のビルドのキャッシュに古い値が残っていても上書きする）。
TARGET_OS="$(sed -n 's/^set(CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9.]*\)".*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release -DRBH_UNIVERSAL=ON -DCMAKE_OSX_DEPLOYMENT_TARGET="${TARGET_OS}"
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure

mkdir -p "${DIST_DIR}"
rm -rf "${DIST_APP}" "${DIST_DIR}/RawBracketHDR.app"  # 旧名のアプリも消す
ditto "${SOURCE_APP}" "${DIST_APP}"
cp "${BUILD_DIR}/hdrcli/rawhdr" "${DIST_DIR}/rawhdr"

# 配布用の証明書が無い環境でも、バンドル全体の整合性を固定する（アドホック署名）。
codesign --force --deep --sign - "${DIST_APP}"
codesign --verify --deep --strict --verbose=2 "${DIST_APP}"

ARCHS="$(lipo -archs "${DIST_APP}/Contents/MacOS/RawHDR Composer")"
for a in x86_64 arm64; do
    case " ${ARCHS} " in
        *" ${a} "*) ;;
        *) echo "エラー: ${a} のスライスがありません" >&2; exit 1 ;;
    esac
done
# 最低対応 OS（Intel）が CMakeLists.txt の配置ターゲットと同じになっていること（LC_VERSION_MIN_MACOSX / LC_BUILD_VERSION）。
MINOS="$(otool -l -arch x86_64 "${DIST_APP}/Contents/MacOS/RawHDR Composer" | awk '/minos|version 10\./{print $2; exit}')"
WANT="$(sed -n 's/^set(CMAKE_OSX_DEPLOYMENT_TARGET "\([0-9.]*\)".*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")"
[ "${MINOS}" = "${WANT}" ] || { echo "エラー: 最低対応 macOS が ${MINOS}（期待 ${WANT}）" >&2; exit 1; }
PLIST_MIN="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "${DIST_APP}/Contents/Info.plist")"
[ "${PLIST_MIN}" = "${WANT}" ] || { echo "エラー: Info.plist の LSMinimumSystemVersion が ${PLIST_MIN}（期待 ${WANT}）" >&2; exit 1; }
echo "完了: ${DIST_APP}"
echo "アーキテクチャ: ${ARCHS}  最低対応 macOS（x86_64）: ${MINOS}"
