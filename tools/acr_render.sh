#!/bin/bash
# Photoshop（Camera Raw の既定の設定）で RAW/DNG を開き、TIFF に書き出す（検証用）。
#   tools/acr_render.sh 入力 出力.tif [入力 出力.tif ...]
# Photoshop の名前は PS_APP で変えられる（既定 "Adobe Photoshop 2026"）。
set -euo pipefail
PS_APP="${PS_APP:-Adobe Photoshop 2026}"
JSX="$(mktemp -t acr_render).jsx"
{
  echo 'app.displayDialogs = DialogModes.NO;'
  echo 'var jobs = ['
  while [ $# -ge 2 ]; do
    src="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    dst="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
    printf '  [%s, %s],\n' "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$src")" "$(python3 -c 'import json,sys;print(json.dumps(sys.argv[1]))' "$dst")"
    shift 2
  done
  echo '];'
  cat <<'JS'
var log = [];
for (var i = 0; i < jobs.length; i++) {
  try {
    var doc = app.open(new File(jobs[i][0]));
    var o = new TiffSaveOptions();
    o.imageCompression = TIFFEncoding.NONE;
    doc.saveAs(new File(jobs[i][1]), o, true);
    log.push("ok " + doc.width + "x" + doc.height + " " + doc.bitsPerChannel + " " + jobs[i][1]);
    doc.close(SaveOptions.DONOTSAVECHANGES);
  } catch (e) {
    log.push("NG " + jobs[i][0] + ": " + e);
  }
}
log.join("\n");
JS
} > "$JSX"
osascript -e "tell application \"$PS_APP\" to do javascript file (POSIX file \"$JSX\")"
rm -f "$JSX"
