#pragma once

// 位置合わせ（開発計画書 §4）。いまは平行移動だけで、移動量は CFA の周期の倍数
// （Bayer は 2 画素、X-Trans は 6 画素）に限る。こうすると色の並びが崩れず、CFA のまま書き出せる。
//
// - 基準フレームは動かさない（レンズプロファイルが元の光学中心と寸法を前提にしているため）
// - 他のフレームを基準フレームに合わせる。隣り合う露出どうしで推定し、つないで基準まで届かせる
//   （露出の離れたフレームどうしは、両方で有効な画素が少なく合わせにくいため）
// - 動かした分だけ画像の端に無効な所ができるので、DNG の DefaultCrop で隠す

#include <string>
#include <vector>

#include "hdrcore/exposure.hpp"
#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct FrameShift {
    int dx = 0, dy = 0;  // aligned(x, y) = original(x + dx, y + dy)。基準フレームは (0, 0)
    double score = 0.0;  // 合わせた後の違い（対数の二乗平均。小さいほどよく合っている）
    bool reliable = true;  // 両方で有効な画素が十分あったか
};

struct AlignOptions {
    int max_shift_px = 96;  // 探す範囲（画素）
};

// 各フレームのずれを推定する（入力の番号の順）。plan は位置合わせ前の推定でよい。
std::vector<FrameShift> estimate_shifts(const std::vector<RawFrame>& frames, const ExposurePlan& plan, int reference,
                                        const AlignOptions& options = {});

// ずれを当てはめる（元の画素は frame.original に残すので、何度当て直してもよい）。
// 移動量は CFA の周期の倍数に丸める。
void apply_shift(RawFrame& frame, int dx, int dy);

// すべてのフレームで有効な範囲（基準フレームの座標）。
void valid_area(const std::vector<RawFrame>& frames, int& x0, int& y0, int& x1, int& y1);

}  // namespace hdr
