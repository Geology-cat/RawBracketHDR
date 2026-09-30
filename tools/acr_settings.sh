#!/bin/bash
# Photoshop（Camera Raw の既定の設定）で開いたときに当たった crs: の設定を表示する（検証用）。
#   tools/acr_settings.sh 入力...
set -euo pipefail
PS_APP="${PS_APP:-Adobe Photoshop 2026}"
JSX="$(mktemp -t acr_settings).jsx"
{
  echo 'app.displayDialogs = DialogModes.NO;'
  echo 'var files = ['
  for f in "$@"; do
    p="$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
    printf '  %s,\n' "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$p")"
  done
  echo '];'
  cat <<'JS'
var out = [];
for (var i = 0; i < files.length; i++) {
  var doc = app.open(new File(files[i]));
  var x = doc.xmpMetadata.rawData;
  var lines = x.split("\n");
  out.push("== " + files[i]);
  for (var j = 0; j < lines.length; j++) {
    if (lines[j].indexOf("crs:") >= 0) out.push(lines[j]);
  }
  doc.close(SaveOptions.DONOTSAVECHANGES);
}
out.join("\n");
JS
} > "$JSX"
osascript -e "tell application \"$PS_APP\" to do javascript file (POSIX file \"$JSX\")"
rm -f "$JSX"
