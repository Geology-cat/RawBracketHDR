#pragma once

// 確認用の簡易現像（縮小・色補間・色変換・トーンカーブ）と、画像ファイルの書き出し。
// DNG のサムネイル・プレビューと、CLI・アプリでの確認表示に使う。
// 合成そのものには使わない（合成は線形のCFAのまま行う）。

#include <cstdint>
#include <string>
#include <vector>

#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct Rgb8Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb;  // 行優先、RGB の順
};

struct PreviewOptions {
    int max_size = 1024;       // 長辺の画素数
    double exposure_ev = 0.0;  // 露出の補正（段）
    bool tone_map = true;      // ハイライトを滑らかに寝かせる
};

// CFA の線形データ（値 1.0 が白の目安）から sRGB の 8bit 画像を作る。
// color_matrix は XYZ(D65)→カメラ、neutral は AsShotNeutral。
Rgb8Image render_preview(const float* cfa, int width, int height, const CfaPattern& pattern,
                         const double color_matrix[3][3], const double neutral[3], const PreviewOptions& options);

// 1枚の RAW を、その白レベルを 1.0 として簡易現像する。
Rgb8Image render_frame_preview(const RawFrame& frame, const PreviewOptions& options);

// 向き（EXIF の Orientation）に合わせて回す。
Rgb8Image apply_orientation(const Rgb8Image& image, int orientation);

// PNG・JPEG の書き出し（macOS の ImageIO を使う）。失敗したら false。
bool write_png(const std::string& path, const Rgb8Image& image);
bool write_gray_png(const std::string& path, int width, int height, const std::vector<uint8_t>& gray);
bool encode_jpeg(const Rgb8Image& image, double quality, std::vector<uint8_t>& out);

}  // namespace hdr
