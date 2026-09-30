# RawBracketHDR

一眼カメラの AEB（オートブラケット）で撮った RAW から、**RAW のまま**HDR合成した DNG を1枚作る macOS アプリ。

- 各画素について「飽和していない中で最も明るいフレーム」を使う（ハイライトは短い露出、シャドウは長い露出。暗いフレームのシャドウは使わない）
- 切り替わりは重みを滑らかに変えて繋ぐ。飽和した画素に重みが漏れない作りにしている
- 出力は **32bit 浮動小数点の CFA DNG**（色補間していない RAW）。Lightroom Classic / Camera Raw で普通の RAW と同じように現像できる
- 基準フレームの EXIF・レンズ情報・メーカーノートを引き継ぐので、Lightroom がレンズプロファイルを自動で当てる
- 対応環境: **macOS 10.13 (High Sierra) 以降**（予定。Intel・Apple Silicon）

> 開発中（v0.1）。アプリ（GUI）とコマンドライン版（`rawhdr`）がある。位置合わせは今後の段階で追加する（現在は三脚撮影のみ）。
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
| `--debug DIR` | 由来マップ（どのフレームを使ったか）とプレビューを書く |

## 仕組み

1. **読み込み**: LibRaw で RAW を展開し、黒を引いた線形の CFA 値をそのまま使う（色補間・色変換はしない）
2. **飽和レベル**: 機種表の白ではなく、データの中の飽和の山から測る（ISO によって実際の飽和値が変わるため）
3. **露出比**: EXIF の名目値ではなく、隣り合うフレームで両方とも有効な画素から実測する（継ぎ目の段差のいちばんの原因が露出比の誤差なので）
4. **重み**: 各ブロック自身の明るさで決める。明るいフレームの値が飽和の閾値の 55% を超えたところから、閾値に向けて
   重みを滑らかに下げ、残りを次に暗いフレームへ回す。なだらかなグラデーションでは切り替わりも空間的に滑らかになり、
   月や光源のようなくっきりした輪郭では輪郭の位置で切り替わる（段差は輪郭に隠れる）。飽和したブロックとその隣の重みは必ず 0
5. **書き出し**: 最も暗いフレームの飽和点を 1.0 とした 32bit float の CFA DNG。BaselineExposure で基準フレームの明るさに合わせる

## ライセンス

GPL 3.0（[LICENSE](LICENSE)）。同梱の LibRaw は LGPL 2.1 の条件で使う（[third_party/LibRaw/README.RawBracketHDR.md](third_party/LibRaw/README.RawBracketHDR.md)）。
