#pragma once

// 合成結果を浮動小数点の CFA DNG として書き出す（開発計画書 §6）。
//
// - 画素: 32bit float、CFA のまま（Lightroom / Camera Raw がデモザイクする）
// - 値: 最も暗いフレームの飽和点 = 1.0（WhiteLevel = 1、BlackLevel = 0。黒は合成で引き済み）
// - 見た目: BaselineExposure で基準フレームと同じ明るさに開く
// - 色: 基準フレームの ColorMatrix・AsShotNeutral（ホワイトバランスは焼き込まない）
// - レンズ: EXIF のレンズ情報・LensInfo・メーカーノート（DNGPrivateData）を引き継ぐ。
//   Lightroom / Camera Raw はこれを見てレンズプロファイルを自動で当てる
// - 向き・撮影日時・GPS・著作権なども基準フレームから引き継ぐ

#include <string>
#include <vector>

#include "hdrcore/exposure.hpp"
#include "hdrcore/merge.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct DngWriteOptions {
    bool compress = true;       // Deflate＋浮動小数点プレディクタ（可逆）
    int tile_size = 256;
    bool embed_preview = true;  // 簡易現像したプレビュー（Finder・カタログの表示用）
    // XMP にレンズプロファイル補正を有効にする初期設定を入れる（crs:LensProfileEnable=1）。
    // 既定は入れない。crs: の設定が1つでも入っていると、Camera Raw はユーザーの既定の現像設定
    // （プロファイルなど）を使わず Adobe の既定に戻してしまうため（Camera Raw 18.6 で確認）。
    // レンズの認識自体は EXIF のレンズ情報で行われるので、これが無くてもプロファイルは選べる。
    bool enable_lens_profile = false;
    // 機種ごとの BaselineExposure（Adobe が機種ごとに持つ値。分からなければ 0）。
    double camera_baseline_exposure = 0.0;
    std::string software = "RawBracketHDR";
};

void write_dng(const std::string& path, const MergeResult& merged, const std::vector<RawFrame>& frames,
               const ExposurePlan& plan, const DngWriteOptions& options = {});

}  // namespace hdr
