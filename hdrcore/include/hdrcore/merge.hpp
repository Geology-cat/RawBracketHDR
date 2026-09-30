#pragma once

// HDR合成の本体（開発計画書 §5）。
//
// 原則: 各画素について「飽和していない（安全マージン込み）フレームのうち、最も明るいもの」を使う。
// 切り替わりは重みを滑らかに変えて繋ぐ。そのとき、飽和した画素に重みが漏れないよう、
// 使ってよい領域をまず縮め（erode）、その内側でだけぼかす。
//
// 出力は CFA のまま（色補間しない）。値は「最も暗いフレームの飽和点 = 1.0」の線形の値で、
// すべて 0〜1 に収まる（開発計画書 §6.1 案2）。

#include <cstdint>
#include <vector>

#include "hdrcore/exposure.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct MergeOptions {
    // 飽和とみなす閾値 = 飽和レベル × safety。飽和の手前の非線形な所を避ける。
    double safety = 0.92;
    // 切り替わりの幅（画素）。広いほど滑らかだが、暗いフレームを使う範囲が少し広がる。
    int feather_px = 64;
    // 基準フレーム（入力の番号）。-1 = 自動（名目の露光量が中央のもの）。
    int reference = -1;
};

struct MergeResult {
    int width = 0, height = 0;
    CfaPattern cfa;
    std::vector<float> data;  // 合成した CFA（0〜1）

    int reference = -1;           // 基準フレーム（入力の番号）
    double white_scale = 1.0;     // 出力値 × white_scale = 基準フレームを白レベル 1.0 で見た値
    double reference_ev_offset = 0.0;  // = log2(white_scale)。DNG の BaselineExposure に足す

    // 確認用: ブロック（CFA の周期）単位の重み。order の順（暗い→明るい）。
    int block = 2;
    int grid_w = 0, grid_h = 0;
    std::vector<std::vector<float>> weights;
    double clipped_fraction = 0.0;  // 最も暗いフレームでも飽和していたブロックの割合
};

// frames と plan は estimate_exposures() に渡したもの・返ってきたもの。
MergeResult merge_frames(const std::vector<RawFrame>& frames, const ExposurePlan& plan, const MergeOptions& options);

// 自動で選ぶ基準フレーム（入力の番号）。
int auto_reference(const std::vector<RawFrame>& frames, const ExposurePlan& plan);

}  // namespace hdr
