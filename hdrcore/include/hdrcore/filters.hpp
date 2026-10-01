#pragma once

// 画像のフィルタ（プレビューの局所トーンマッピングと、明暗差の圧縮で使う）。

#include <vector>

namespace hdr {

// 明るさの対数を、明るさの差が range_sigma 段より大きい輪郭を残して滑らかにする（バイラテラルグリッド）。
std::vector<float> bilateral_smooth(const std::vector<float>& l, int w, int h, float space, float range_sigma);

// 対数の値 t ≥ 0 を、傾き 1 で始まり [0, span] を [0, room] に収める曲線（room < span のときだけ縮める）。
struct KneeCurve {
    double room = 0.0, span = 0.0, a = 0.0;
    bool active = false;
    KneeCurve(double room, double span);
    double operator()(double t) const;
};

}  // namespace hdr
