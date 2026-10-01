#pragma once

// 色補間（LinearRaw で書き出すとき用）。
//
// Bayer は RCD（Ratio Corrected Demosaicing、Luis Sanz Rodríguez。RawTherapee・darktable の実装と同じ手順）。
// HDR のデータは値の幅が 2^24 を超えるので、判断はすべて比と勾配の比で行い、小さな値でも
// 微小量 eps に負けないようにしてある。
// X-Trans は近傍の同じ色の平均による簡単な補間（今後改良する）。

#include <vector>

#include "hdrcore/noise.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

// CFA（width × height、1ch）から RGB（画素ごとに R,G,B の順に並べる）を作る。
// noise はこの値の単位でのノイズ（分散 = S·(v − black) + O）。方向の手がかりがノイズと同じくらいしか
// ない所では方向を決めつけずに補間する（ノイズの多い所で迷路のような模様が出るのを防ぐ）。
// black は値に足してある底上げの量（負のノイズを残すため）。
// gain は明暗差の圧縮で掛けた倍率（block × block 画素ごと、grid_w 列。nullptr なら無し）。
// 倍率を掛けた所ではノイズも同じ倍率で大きくなっているので、見積もりに反映する。
std::vector<float> demosaic(const float* cfa, int width, int height, const CfaPattern& pattern,
                            const NoiseModel& noise = NoiseModel(), float black = 0.0f,
                            const float* gain = nullptr, int block = 2, int grid_w = 0);

}  // namespace hdr
