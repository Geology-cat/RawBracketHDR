#include "hdrcore/merge.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

// 箱型のぼかし（半径 r、端は端の値を延ばす）を横・縦に1回ずつ掛ける。
// 1回の台（影響の届く範囲）は正方形の半径 r。
void box_blur(std::vector<float>& img, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(img.size());
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    // 横
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* src = img.data() + static_cast<std::size_t>(y) * w;
            float* dst = tmp.data() + static_cast<std::size_t>(y) * w;
            double acc = 0.0;
            for (int k = -r; k <= r; ++k) acc += src[std::min(w - 1, std::max(0, k))];
            for (int x = 0; x < w; ++x) {
                dst[x] = static_cast<float>(acc) * inv;
                acc += src[std::min(w - 1, x + r + 1)] - src[std::max(0, x - r)];
            }
        }
    });
    // 縦
    parallel_for(w, [&](int x0, int x1) {
        std::vector<double> acc(static_cast<std::size_t>(x1 - x0), 0.0);
        for (int x = x0; x < x1; ++x) {
            for (int k = -r; k <= r; ++k) acc[x - x0] += tmp[static_cast<std::size_t>(std::min(h - 1, std::max(0, k))) * w + x];
        }
        for (int y = 0; y < h; ++y) {
            const std::size_t add = static_cast<std::size_t>(std::min(h - 1, y + r + 1)) * w;
            const std::size_t sub = static_cast<std::size_t>(std::max(0, y - r)) * w;
            float* dst = img.data() + static_cast<std::size_t>(y) * w;
            for (int x = x0; x < x1; ++x) {
                dst[x] = static_cast<float>(acc[x - x0]) * inv;
                acc[x - x0] += tmp[add + x] - tmp[sub + x];
            }
        }
    }, 64);
}

// 飽和ブロックから（チェビシェフ距離で）R 以内にあるブロックを外した「使ってよい」マスク。
std::vector<float> eroded_allowed(const std::vector<uint8_t>& sat, int w, int h, int R) {
    // 飽和ブロックの数の累積和（積分画像）。
    std::vector<int32_t> integral(static_cast<std::size_t>(w + 1) * (h + 1), 0);
    for (int y = 0; y < h; ++y) {
        int32_t row = 0;
        for (int x = 0; x < w; ++x) {
            row += sat[static_cast<std::size_t>(y) * w + x];
            integral[static_cast<std::size_t>(y + 1) * (w + 1) + (x + 1)] =
                integral[static_cast<std::size_t>(y) * (w + 1) + (x + 1)] + row;
        }
    }
    std::vector<float> out(static_cast<std::size_t>(w) * h);
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const int ya = std::max(0, y - R), yb = std::min(h, y + R + 1);
            for (int x = 0; x < w; ++x) {
                const int xa = std::max(0, x - R), xb = std::min(w, x + R + 1);
                const int32_t n = integral[static_cast<std::size_t>(yb) * (w + 1) + xb] -
                                  integral[static_cast<std::size_t>(ya) * (w + 1) + xb] -
                                  integral[static_cast<std::size_t>(yb) * (w + 1) + xa] +
                                  integral[static_cast<std::size_t>(ya) * (w + 1) + xa];
                out[static_cast<std::size_t>(y) * w + x] = n == 0 ? 1.0f : 0.0f;
            }
        }
    });
    return out;
}

}  // namespace

int auto_reference(const std::vector<RawFrame>& frames, const ExposurePlan& plan) {
    if (frames.empty() || plan.order.empty()) return -1;
    return plan.order[(plan.order.size() - 1) / 2];
}

MergeResult merge_frames(const std::vector<RawFrame>& frames, const ExposurePlan& plan, const MergeOptions& opt) {
    const int n = static_cast<int>(frames.size());
    if (n == 0) throw std::runtime_error("フレームがありません");
    const RawFrame& first = frames[0];
    for (const RawFrame& f : frames) {
        if (f.width != first.width || f.height != first.height) throw std::runtime_error("フレームの寸法が揃っていません");
        if (f.cfa.w != first.cfa.w || f.cfa.h != first.cfa.h || std::memcmp(f.cfa.color, first.cfa.color, sizeof(f.cfa.color)) != 0) {
            throw std::runtime_error("フレームのカラーフィルターの並びが揃っていません");
        }
    }

    MergeResult res;
    res.width = first.width;
    res.height = first.height;
    res.cfa = first.cfa;
    res.block = first.cfa.is_xtrans() ? 3 : 2;
    const int b = res.block;
    const int gw = (res.width + b - 1) / b, gh = (res.height + b - 1) / b;
    res.grid_w = gw;
    res.grid_h = gh;
    const std::size_t cells = static_cast<std::size_t>(gw) * gh;

    // ---- 飽和ブロック ----
    // ブロックの中のどれか1画素（どの色でも）が閾値を超えたら、そのブロックは飽和とする。
    // 1色だけ飽和した状態で使うと色かぶり（マゼンタのハイライトなど）になるため。
    std::vector<std::vector<uint8_t>> sat(n);
    for (int o = 0; o < n; ++o) {
        const int fi = plan.order[o];
        const RawFrame& f = frames[fi];
        float thr[3];
        for (int c = 0; c < 3; ++c) thr[c] = static_cast<float>(plan.clip[fi].level[c] * opt.safety);
        std::vector<uint8_t>& s = sat[o];
        s.assign(cells, 0);
        parallel_for(gh, [&](int by0, int by1) {
            for (int by = by0; by < by1; ++by) {
                for (int y = by * b; y < std::min(res.height, (by + 1) * b); ++y) {
                    const float* row = f.data.data() + static_cast<std::size_t>(y) * f.width;
                    uint8_t* srow = s.data() + static_cast<std::size_t>(by) * gw;
                    for (int x = 0; x < res.width; ++x) {
                        if (row[x] >= thr[f.cfa.at(x, y)]) srow[x / b] = 1;
                    }
                }
            }
        });
    }
    {
        std::size_t clipped = 0;
        for (uint8_t v : sat[0]) clipped += v;
        res.clipped_fraction = static_cast<double>(clipped) / static_cast<double>(cells);
    }

    // ---- 重み ----
    // 明るいフレームから順に「使ってよい」マスク M を作り、残りの重みを上から配っていく。
    //   w[N-1] = M[N-1]、w[k] = (1 − Σ_{j>k} w[j]) · M[k]、最も暗いフレームは残り全部。
    // M は「飽和ブロックから R 以内を外したマスク」を、台の半径が R 以下のぼかしで滑らかにしたもの。
    // よって飽和ブロックでは M = 0 が保証され、飽和した画素に重みが漏れることは構造上ない。
    const int R = std::max(3, opt.feather_px / (2 * b));
    const int r = R / 3;  // 箱型ぼかし3回の台の半径は 3r ≤ R
    res.weights.assign(n, std::vector<float>());
    std::vector<float> remaining(cells, 1.0f);
    for (int o = n - 1; o >= 1; --o) {
        std::vector<float> m = eroded_allowed(sat[o], gw, gh, R);
        for (int pass = 0; pass < 3; ++pass) box_blur(m, gw, gh, r);
        std::vector<float>& w = res.weights[o];
        w.resize(cells);
        for (std::size_t i = 0; i < cells; ++i) {
            const float mi = sat[o][i] ? 0.0f : std::min(1.0f, std::max(0.0f, m[i]));
            w[i] = remaining[i] * mi;
            remaining[i] -= w[i];
        }
    }
    res.weights[0] = remaining;

    // ---- 合成 ----
    // 出力 = Σ w·v / (相対露光量 · 最も暗いフレームの飽和レベル)。
    const int darkest = plan.order[0];
    const float l0 = std::min({plan.clip[darkest].level[0], plan.clip[darkest].level[1], plan.clip[darkest].level[2]});
    if (!(l0 > 0.0f)) throw std::runtime_error("飽和レベルが分かりません");
    std::vector<float> scale(n);
    for (int o = 0; o < n; ++o) scale[o] = static_cast<float>(1.0 / (plan.rel_exposure[o] * l0));
    res.data.assign(static_cast<std::size_t>(res.width) * res.height, 0.0f);
    parallel_for(res.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* dst = res.data.data() + static_cast<std::size_t>(y) * res.width;
            const std::size_t grow = static_cast<std::size_t>(y / b) * gw;
            for (int o = 0; o < n; ++o) {
                const float* src = frames[plan.order[o]].data.data() + static_cast<std::size_t>(y) * res.width;
                const float* w = res.weights[o].data() + grow;
                const float s = scale[o];
                for (int x = 0; x < res.width; ++x) {
                    const float wx = w[x / b];
                    if (wx != 0.0f) dst[x] += wx * src[x] * s;
                }
            }
            for (int x = 0; x < res.width; ++x) dst[x] = std::min(1.0f, std::max(0.0f, dst[x]));
        }
    });

    // ---- 基準フレームとの明るさの関係 ----
    res.reference = opt.reference >= 0 && opt.reference < n ? opt.reference : auto_reference(frames, plan);
    int ref_o = 0;
    for (int o = 0; o < n; ++o) {
        if (plan.order[o] == res.reference) ref_o = o;
    }
    const float lref = plan.clip[res.reference].level[1];
    res.white_scale = plan.rel_exposure[ref_o] * l0 / lref;
    res.reference_ev_offset = std::log2(res.white_scale);
    return res;
}

}  // namespace hdr
