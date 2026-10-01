# LibRaw（同梱）

- 版: 0.22.2（無改変）
- 入手元: https://www.libraw.org/data/LibRaw-0.22.2.tar.gz
- SHA-256: de86b035655accff8d4010f1a221fdf50d353cb7b1422ba26f14a0db92612cfa
- 同梱したもの: `libraw/` `internal/` `src/`（`src/Makefile` を除く）と、COPYRIGHT・LICENSE.CDDL・LICENSE.LGPL・README.md・Changelog.txt
- ライセンス: LibRaw は LGPL 2.1 と CDDL 1.0 の選択制。本プロジェクトは GPL 3.0 なので、
  GPL と両立する **LGPL 2.1** の条件で使う（CDDL は GPL と両立しない）
- 使い方: `hdrcore/src/raw_frame.cpp` から、カメラRAWの展開（`unpack`）、黒・白・切り抜き・色の情報、
  EXIF・メーカーノートの取り出し（コールバック）だけを使う。Deflate の DNG のため `USE_ZLIB` を付け、macOS 標準の libz をリンクする
- 更新するとき: 版を上げると黒・白・切り抜きの表が変わり、同じRAWでも出力が変わることがある。
  上げたら開発記録に書き、サンプルで値を比べること
