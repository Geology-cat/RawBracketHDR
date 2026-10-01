# RawBracketHDR

一眼カメラの AEB（オートブラケット）で撮った RAW から、**RAW のまま**HDR合成した DNG を1枚作る macOS アプリ。

- 各画素について「飽和していない中で最も明るいフレーム」を使う（ハイライトは短い露出、シャドウは長い露出。暗いフレームのシャドウは使わない）
- 切り替わりは重みを滑らかに変えて繋ぐ。飽和した画素に重みが漏れない作りにしている
- 出力は **32bit 浮動小数点の DNG**。Lightroom Classic / Camera Raw で普通の RAW と同じように現像できる
  - **CFA**（色補間前）と **LinearRaw**（このアプリが RCD で色補間）を、場面の明暗差から自動で選ぶ（手動でも選べる）。
    Camera Raw は CFA の DNG を白から下およそ 16 段までしか扱えないため（[検証記録](docs/検証記録.md)）
- 基準フレームの EXIF・レンズ情報・メーカーノートを引き継ぐので、Lightroom がレンズプロファイルを自動で当てる
- **Adobe DNG Converter**（無料）が入っていれば、基準フレームを変換して色・明るさ・プロファイルを Camera Raw に揃える
  （入っていなくても動くが、色と明るさが CR2 を開いたときとわずかに違う）
- 位置合わせ: しない（三脚）／自動／手動。平行移動で、色の並びを崩さない 2 画素単位。基準フレームは動かさない
- 対応環境: **macOS 10.13 (High Sierra) 以降**（予定。Intel・Apple Silicon）

> 開発中（v0.1）。アプリ（GUI）とコマンドライン版（`rawhdr`）がある。回転・1 画素未満の位置合わせ（手持ち撮影）と、動体（ゴースト）への対策は今後の段階で追加する。
> 計画: [docs/開発計画書.md](docs/開発計画書.md)

## ビルド

Xcode（コマンドラインツール）、CMake 3.20 以降、Ninja が要る。

```bash
cmake -S . -B .build/dev -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build .build/dev
```

配布用の Universal アプリ（Intel・Apple Silicon、macOS 10.13 以降）は次で作る（テストも実行する）。

```bash
./scripts/build_app.sh   # dist/RawBracketHDR.app と dist/rawhdr
```

## 使い方（アプリ）

1. 露出を変えて撮った RAW（またはそのフォルダ）をウインドウへドロップする
2. 自動で露出比を測って合成し、プレビューを出す。「由来マップ」でどのフレームを使ったかを色で確かめられる
   （色はフレーム一覧の番号の色）
3. 「DNG を書き出す…」で保存する。Lightroom Classic・Camera Raw でそのまま現像できる

## 使い方（コマンドライン）

```bash
# 各 RAW の情報（露出・飽和レベル・レンズなど）
.build/dev/hdrcli/rawhdr info IMG_0001.CR2 IMG_0002.CR2 IMG_0003.CR2

# 合成して DNG を書き出す（省略時は基準フレームの隣に <名前>_HDR.dng）
.build/dev/hdrcli/rawhdr merge -o out.dng IMG_0001.CR2 IMG_0002.CR2 IMG_0003.CR2
```

主なオプション:

| オプション | 意味 |
|---|---|
| `--ref N` | 基準フレーム（入力の順で1から）。省略時は露光量が中央のもの。DNG はこのフレームと同じ明るさで開く |
| `--ramp X` | 明るいフレームの重みを下げ始める明るさ（飽和の閾値に対する比、既定 0.55） |
| `--feather PX` | 重みのちらつきを抑えるぼかしの幅（画素、既定 16） |
| `--safety X` | 飽和とみなす閾値（飽和レベルに対する比、既定 0.92） |
| `--format F` | 出力形式 `auto`（既定）・`cfa`・`linear` |
| `--align` | 自動で位置合わせする |
| `--shift N:dx,dy` | N 枚目のずれを手で指定する |
| `--no-adobe` | Adobe DNG Converter があっても使わない |
| `--debug DIR` | 由来マップ（どのフレームを使ったか）とプレビューを書く |

## 仕組み

1. **読み込み**: LibRaw で RAW を展開し、黒を引いた線形の CFA 値をそのまま使う（色補間・色変換はしない）
2. **飽和レベル**: 機種表の白ではなく、データの中の飽和の山から測る（ISO によって実際の飽和値が変わるため）
3. **露出比**: EXIF の名目値ではなく、隣り合うフレームで両方とも有効な画素から実測する（継ぎ目の段差のいちばんの原因が露出比の誤差なので）
4. **重み**: 各ブロック自身の明るさで決める。明るいフレームの値が飽和の閾値の 55% を超えたところから、閾値に向けて
   重みを滑らかに下げ、残りを次に暗いフレームへ回す。なだらかなグラデーションでは切り替わりも空間的に滑らかになり、
   月や光源のようなくっきりした輪郭では輪郭の位置で切り替わる（段差は輪郭に隠れる）。飽和したブロックとその隣の重みは必ず 0
5. **出力形式**: 暗部のノイズと Camera Raw の刻み（白の 2⁻¹⁶）を比べ、段差が見えないなら CFA、見えるなら LinearRaw
6. **書き出し**: 最も暗いフレームの飽和点を 1.0 とした 32bit float の DNG。BaselineExposure で基準フレームの明るさに合わせる。
   Adobe DNG Converter があれば、基準フレームを変換した DNG から色の補正・プロファイル・白レベル・基準露出を引き継ぐ

## ライセンス

GPL 3.0（[LICENSE](LICENSE)）。同梱の LibRaw は LGPL 2.1 の条件で使う（[third_party/LibRaw/README.RawBracketHDR.md](third_party/LibRaw/README.RawBracketHDR.md)）。
