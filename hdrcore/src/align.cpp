#include "hdrcore/align.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

// 位置合わせに使う縮小画像。値は露出をそろえた明るさの対数、valid = 使ってよい画素か。
struct Proxy {
    int w = 0, h = 0;
    std::vector<float> v;
    std::vector<uint8_t> valid;
};

// ブロック（CFA の周期の大きさ）ごとの明るさの和を、相対露光量で割って対数にする。
// 飽和に近いブロックと、暗すぎる（ノイズに埋もれる）ブロックは使わない。
Proxy make_proxy(const RawFrame& f, const ClipLevels& clip, double rel_exposure, double dark_floor) {
    Proxy p;
    const int b = f.cfa.is_xtrans() ? 3 : 2;
    p.w = f.width / b;
    p.h = f.height / b;
    p.v.assign(static_cast<std::size_t>(p.w) * p.h, 0.0f);
    p.valid.assign(p.v.size(), 0);
    const float sat[3] = {clip.level[0] * 0.9f, clip.level[1] * 0.9f, clip.level[2] * 0.9f};
    parallel_for(p.h, [&](int y0, int y1) {
        for (int by = y0; by < y1; ++by) {
            for (int bx = 0; bx < p.w; ++bx) {
                double sum = 0.0;
                bool ok = true;
                for (int y = by * b; y < (by + 1) * b; ++y) {
                    for (int x = bx * b; x < (bx + 1) * b; ++x) {
                        const float v = f.value(x, y);
                        if (v >= sat[f.cfa.at(x, y)]) ok = false;
                        sum += v;
                    }
                }
                const std::size_t i = static_cast<std::size_t>(by) * p.w + bx;
                const double n = sum / (b * b);
                if (ok && n > dark_floor) {
                    p.v[i] = static_cast<float>(std::log(n / rel_exposure));
                    p.valid[i] = 1;
                }
            }
        }
    });
    return p;
}

Proxy downsample(const Proxy& s) {
    Proxy d;
    d.w = s.w / 2;
    d.h = s.h / 2;
    d.v.assign(static_cast<std::size_t>(d.w) * d.h, 0.0f);
    d.valid.assign(d.v.size(), 0);
    for (int y = 0; y < d.h; ++y) {
        for (int x = 0; x < d.w; ++x) {
            double sum = 0.0;
            int n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const std::size_t i = static_cast<std::size_t>(2 * y + dy) * s.w + (2 * x + dx);
                    if (s.valid[i]) {
                        sum += s.v[i];
                        ++n;
                    }
                }
            }
            if (n == 4) {
                const std::size_t i = static_cast<std::size_t>(y) * d.w + x;
                d.v[i] = static_cast<float>(sum / 4.0);
                d.valid[i] = 1;
            }
        }
    }
    return d;
}

// a(x, y) と b(x + dx, y + dy) の違い（平均の差を引いた二乗平均）。両方で有効な画素の割合も返す。
double mismatch(const Proxy& a, const Proxy& b, int dx, int dy, double& overlap) {
    double s = 0.0, ss = 0.0;
    long n = 0;
    const int x0 = std::max(0, -dx), x1 = std::min(a.w, b.w - dx);
    const int y0 = std::max(0, -dy), y1 = std::min(a.h, b.h - dy);
    for (int y = y0; y < y1; ++y) {
        const std::size_t ra = static_cast<std::size_t>(y) * a.w, rb = static_cast<std::size_t>(y + dy) * b.w;
        for (int x = x0; x < x1; ++x) {
            if (!a.valid[ra + x] || !b.valid[rb + x + dx]) continue;
            const double d = a.v[ra + x] - b.v[rb + x + dx];
            s += d;
            ss += d * d;
            ++n;
        }
    }
    overlap = static_cast<double>(n) / std::max<long>(1, static_cast<long>(a.w) * a.h);
    if (n < 64) return std::numeric_limits<double>::infinity();
    const double mean = s / n;
    return ss / n - mean * mean;
}

// 2枚のずれ（ブロック単位）。粗い段から順に、前の段の答えの周りだけを探す。
FrameShift match_pair(const Proxy& a, const Proxy& b, int max_shift_blocks) {
    std::vector<Proxy> pa = {a}, pb = {b};
    while (pa.back().w > 320 && pa.back().h > 200) {
        pa.push_back(downsample(pa.back()));
        pb.push_back(downsample(pb.back()));
    }
    int dx = 0, dy = 0;
    double best = 0.0, overlap = 0.0;
    for (int lv = static_cast<int>(pa.size()) - 1; lv >= 0; --lv) {
        const bool coarsest = lv == static_cast<int>(pa.size()) - 1;
        const int r = coarsest ? std::max(2, (max_shift_blocks >> lv) + 1) : 1;
        const int cx = coarsest ? 0 : dx * 2, cy = coarsest ? 0 : dy * 2;
        best = std::numeric_limits<double>::infinity();
        // 候補ごとの計算は独立なので並列に。
        const int side = 2 * r + 1;
        std::vector<double> scores(static_cast<std::size_t>(side) * side), overlaps(scores.size());
        parallel_for(side * side, [&](int i0, int i1) {
            for (int i = i0; i < i1; ++i) {
                const int ox = cx + (i % side) - r, oy = cy + (i / side) - r;
                scores[i] = mismatch(pa[lv], pb[lv], ox, oy, overlaps[i]);
            }
        }, 1);
        for (int i = 0; i < side * side; ++i) {
            // 同じくらいなら移動の少ない方（0 に近い方）を選ぶ。
            const int ox = cx + (i % side) - r, oy = cy + (i / side) - r;
            const double penalty = 1e-9 * (ox * ox + oy * oy);
            if (scores[i] + penalty < best) {
                best = scores[i] + penalty;
                dx = ox;
                dy = oy;
                overlap = overlaps[i];
            }
        }
    }
    FrameShift s;
    s.dx = dx;
    s.dy = dy;
    s.score = best;
    s.reliable = std::isfinite(best) && overlap > 0.05;
    return s;
}

int round_to_multiple(int v, int m) {
    return static_cast<int>(std::lround(static_cast<double>(v) / m)) * m;
}

}  // namespace

std::vector<FrameShift> estimate_shifts(const std::vector<RawFrame>& frames, const ExposurePlan& plan, int reference,
                                        const AlignOptions& opt) {
    const int n = static_cast<int>(frames.size());
    std::vector<FrameShift> out(n);
    if (n < 2 || reference < 0 || reference >= n) return out;
    const int block = frames[0].cfa.is_xtrans() ? 3 : 2;
    const int period = frames[0].cfa.is_xtrans() ? 6 : 2;
    // 縮小画像（並べた順に）。暗すぎるブロックの下限は、そのフレームの飽和レベルの 0.3%。
    std::vector<Proxy> proxies(n);
    for (int o = 0; o < n; ++o) {
        const int i = plan.order[o];
        const ClipLevels& c = plan.clip[i];
        proxies[o] = make_proxy(frames[i], c, plan.rel_exposure[o], 0.003 * c.level[1]);
    }
    // 隣どうしのずれ（order の順に、o と o+1）。
    std::vector<FrameShift> pair(n);
    const int max_blocks = std::max(1, opt.max_shift_px / block);
    for (int o = 0; o + 1 < n; ++o) {
        pair[o] = match_pair(proxies[o], proxies[o + 1], max_blocks);
        // 両方で有効な画素が足りない組（暗いフレームに光源しか写っていないなど）は当てにならないので、
        // 動かさない（三脚なら 0 が最も確からしい）。
        if (!pair[o].reliable) pair[o].dx = pair[o].dy = 0;
    }
    // 基準からたどってつなぐ。aligned_b(x) = b(x + d) で、d は「a に対する b のずれ」。
    int ref_o = 0;
    for (int o = 0; o < n; ++o) {
        if (plan.order[o] == reference) ref_o = o;
    }
    std::vector<int> bx(n, 0), by(n, 0);
    std::vector<bool> ok(n, true);
    for (int o = ref_o + 1; o < n; ++o) {
        bx[o] = bx[o - 1] + pair[o - 1].dx;
        by[o] = by[o - 1] + pair[o - 1].dy;
        ok[o] = ok[o - 1] && pair[o - 1].reliable;
    }
    for (int o = ref_o - 1; o >= 0; --o) {
        bx[o] = bx[o + 1] - pair[o].dx;
        by[o] = by[o + 1] - pair[o].dy;
        ok[o] = ok[o + 1] && pair[o].reliable;
    }
    for (int o = 0; o < n; ++o) {
        FrameShift& s = out[plan.order[o]];
        s.dx = round_to_multiple(bx[o] * block, period);
        s.dy = round_to_multiple(by[o] * block, period);
        s.reliable = ok[o];
        s.score = o == ref_o ? 0.0 : (o > ref_o ? pair[o - 1].score : pair[o].score);
    }
    out[reference] = FrameShift();
    return out;
}

void apply_shift(RawFrame& f, int dx, int dy) {
    const int period = f.cfa.is_xtrans() ? 6 : 2;
    dx = round_to_multiple(dx, period);
    dy = round_to_multiple(dy, period);
    if (dx == f.shift_x && dy == f.shift_y) return;
    if (f.original.empty()) f.original = f.data;  // 元の画素は一度だけ取っておく
    f.shift_x = dx;
    f.shift_y = dy;
    if (dx == 0 && dy == 0) {
        f.data = f.original;
        f.original.clear();
        f.original.shrink_to_fit();
        return;
    }
    const int W = f.width, H = f.height;
    const std::vector<float>& src = f.original;
    // 画像の外は、同じ色の一番近い画素で埋める（周期の倍数だけ内側に寄せる）。DefaultCrop で隠れる所。
    parallel_for(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            int sy = y + dy;
            while (sy < 0) sy += period;
            while (sy >= H) sy -= period;
            float* dst = f.data.data() + static_cast<std::size_t>(y) * W;
            const float* row = src.data() + static_cast<std::size_t>(sy) * W;
            for (int x = 0; x < W; ++x) {
                int sx = x + dx;
                while (sx < 0) sx += period;
                while (sx >= W) sx -= period;
                dst[x] = row[sx];
            }
        }
    });
}

void valid_area(const std::vector<RawFrame>& frames, int& x0, int& y0, int& x1, int& y1) {
    x0 = y0 = 0;
    x1 = frames.empty() ? 0 : frames[0].width;
    y1 = frames.empty() ? 0 : frames[0].height;
    for (const RawFrame& f : frames) {
        // aligned(x) = original(x + dx) が有効なのは 0 ≤ x + dx < W のとき。
        x0 = std::max(x0, -f.shift_x);
        y0 = std::max(y0, -f.shift_y);
        x1 = std::min(x1, f.width - f.shift_x);
        y1 = std::min(y1, f.height - f.shift_y);
    }
}

}  // namespace hdr
