#pragma once

// 色補間（LinearRaw で書き出すとき用）。
//
// Bayer は RCD（Ratio Corrected Demosaicing、Luis Sanz Rodríguez。RawTherapee・darktable の実装と同じ手順）。
// HDR のデータは値の幅が 2^24 を超えるので、判断はすべて比と勾配の比で行い、小さな値でも
// 微小量 eps に負けないようにしてある。
// X-Trans は近傍の同じ色の平均による簡単な補間（今後改良する）。

#include <vector>

#include "hdrcore/raw_frame.hpp"

namespace hdr {

// CFA（width × height、1ch）から RGB（画素ごとに R,G,B の順に並べる）を作る。
std::vector<float> demosaic(const float* cfa, int width, int height, const CfaPattern& pattern);

}  // namespace hdr
