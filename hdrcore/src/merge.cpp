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

    // ---- ブロックの明るさ ----
    // s = ブロックの中の各画素の「値 / 飽和の閾値」の最大（どの色でも）。s ≥ 1 なら飽和ブロック。
    // 1色だけ飽和した状態で使うと色かぶり（マゼンタのハイライトなど）になるので、色をまたいで最大を取る。
    std::vector<std::vector<float>> level(n);
    for (int o = 0; o < n; ++o) {
        const int fi = plan.order[o];
        const RawFrame& f = frames[fi];
        float inv[3];
        for (int c = 0; c < 3; ++c) inv[c] = static_cast<float>(1.0 / (plan.clip[fi].level[c] * opt.safety));
        std::vector<float>& s = level[o];
        s.assign(cells, 0.0f);
        parallel_for(gh, [&](int by0, int by1) {
            for (int by = by0; by < by1; ++by) {
                float* srow = s.data() + static_cast<std::size_t>(by) * gw;
                for (int y = by * b; y < std::min(res.height, (by + 1) * b); ++y) {
                    const float* row = f.data.data() + static_cast<std::size_t>(y) * f.width;
                    for (int x = 0; x < res.width; ++x) {
                        const float v = row[x] * inv[f.cfa.at(x, y)];
                        float& d = srow[x / b];
                        if (v > d) d = v;
                    }
                }
            }
        });
    }
    {
        std::size_t clipped = 0;
        for (float v : level[0]) clipped += v >= 1.0f ? 1 : 0;
        res.clipped_fraction = static_cast<double>(clipped) / static_cast<double>(cells);
    }

    // ---- 重み ----
    // 各フレームの重み h は「そのブロック自身の明るさ」で決める。s が ramp_start 以下なら 1、
    // 飽和の閾値（s = 1）に近づくにつれ滑らかに 0 へ下げる。明るいフレームから順に、残りの重みを配る:
    //   w[N-1] = h[N-1]、w[k] = (1 − Σ_{j>k} w[j]) · h[k]、最も暗いフレームは残り全部。
    //
    // 明るさで切り替えるので、なだらかなグラデーションでは切り替わりも空間的に滑らかになり、
    // くっきりした輪郭（月・光源）では輪郭そのものの位置で切り替わる（段差は輪郭に隠れる）。
    // 空間的な余白で切り替えると、小さな明るい物の周りが最も暗い（ノイズの多い）フレームで
    // 埋まってしまうので、余白は隣の1ブロック（にじみ・わずかなずれの分）だけにとどめる。
    //
    // 飽和ブロックとその隣では h = 0 を最後に掛け直すので、飽和した画素に重みが漏れることはない。
    const float a = static_cast<float>(std::min(0.95, std::max(0.0, opt.ramp_start)));
    const int r = std::max(1, opt.feather_px / (4 * b));
    res.weights.assign(n, std::vector<float>());
    std::vector<float> remaining(cells, 1.0f);
    std::vector<float> dil(cells), h(cells), hb;
    for (int o = n - 1; o >= 1; --o) {
        const std::vector<float>& s = level[o];
        // 隣の1ブロックまで含めた最大（にじみ・わずかなずれへの余裕）。
        parallel_for(gh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < gw; ++x) {
                    float m = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int yy = std::min(gh - 1, std::max(0, y + dy));
                        const float* row = s.data() + static_cast<std::size_t>(yy) * gw;
                        for (int dx = -1; dx <= 1; ++dx) m = std::max(m, row[std::min(gw - 1, std::max(0, x + dx))]);
                    }
                    dil[static_cast<std::size_t>(y) * gw + x] = m;
                }
            }
        });
        for (std::size_t i = 0; i < cells; ++i) {
            const float t = std::min(1.0f, std::max(0.0f, (dil[i] - a) / (1.0f - a)));
            h[i] = 1.0f - t * t * (3.0f - 2.0f * t);
        }
        // 雑音で重みがちらつかないよう軽くぼかす。ただし、ぼかしで重みを「下げる」ことはしない
        // （飽和ブロックの 0 が周りへ広がると、余裕のある明るいフレームを使える所まで暗いフレームに
        // 回ってしまう）。飽和ブロック（とその隣）は最後に 0 に戻す。
        hb = h;
        box_blur(hb, gw, gh, r);
        box_blur(hb, gw, gh, r);
        std::vector<float>& w = res.weights[o];
        w.resize(cells);
        for (std::size_t i = 0; i < cells; ++i) {
            const float hi = dil[i] >= 1.0f ? 0.0f : std::min(1.0f, std::max(h[i], hb[i]));
            w[i] = remaining[i] * hi;
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
    res.reference_rel_exposure = plan.rel_exposure[ref_o];
    res.darkest_clip = l0;
    res.reference_ev_offset = std::log2(res.white_scale);
    return res;
}

}  // namespace hdr
