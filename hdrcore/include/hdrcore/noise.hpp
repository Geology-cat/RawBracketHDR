#pragma once

// ノイズの量の見積もり。分散 = S·v + O（v は黒を引いた値）。
//
// 使い道:
// - DNG の NoiseProfile。Camera Raw はこれでノイズ除去の効き方を決める。無いとノイズを少なく見積もり、
//   色ノイズの大きなむら（オレンジのにじみなど）が残る（灯台の LinearRaw で確認）
// - 色補間（RCD）で、方向の手がかりがノイズと同じくらいしかない所を見分ける

#include "hdrcore/exposure.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct NoiseModel {
    double S[3] = {};  // 色ごと（R・G・B）
    double O[3] = {};
    bool valid = false;

    double variance(int c, double v) const { return S[c] * (v > 0.0 ? v : 0.0) + O[c]; }
};

// 1枚の RAW から、同じ色の近い2画素の差を明るさごとに集めて見積もる（単位は DN）。
NoiseModel estimate_noise(const RawFrame& frame, const ClipLevels& clip);

// 値を k 倍したときのノイズ（S は k 倍、O は k² 倍）。
NoiseModel scale_noise(const NoiseModel& m, double k);

}  // namespace hdr
