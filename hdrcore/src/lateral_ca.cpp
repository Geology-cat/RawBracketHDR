#include "hdrcore/lateral_ca.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

// ベイヤーの 2×2 の中で、色 c の画素の位置。
bool site_of(const CfaPattern& cfa, int c, int& ox, int& oy) {
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            if (cfa.at(x, y) == c) {
                ox = x;
                oy = y;
                return true;
            }
        }
    }
    return false;
}

// 1 色の面（その色の画素だけを集めた、縦横半分の画像）。
struct Plane {
    int w = 0, h = 0, ox = 0, oy = 0;
    std::vector<float> v;
    float at(int i, int j) const {
        i = std::min(w - 1, std::max(0, i));
        j = std::min(h - 1, std::max(0, j));
        return v[static_cast<std::size_t>(j) * w + i];
    }
    // 元の画像の座標 (x, y) での双一次の補間。
    float bilinear(double x, double y) const {
        const double fx = (x - ox) * 0.5, fy = (y - oy) * 0.5;
        const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
        const float ax = static_cast<float>(fx - i), ay = static_cast<float>(fy - j);
        return (1 - ay) * ((1 - ax) * at(i, j) + ax * at(i + 1, j)) + ay * ((1 - ax) * at(i, j + 1) + ax * at(i + 1, j + 1));
    }
    // 3 次（Catmull-Rom）の補間。輪郭の行き過ぎ（明るい月の縁の負の値など）を防ぐため、近い 4 点の範囲に収める。
    float bicubic(double x, double y) const {
        const double fx = (x - ox) * 0.5, fy = (y - oy) * 0.5;
        const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
        const double tx = fx - i, ty = fy - j;
        const auto weights = [](double t, double w[4]) {
            const double t2 = t * t, t3 = t2 * t;
            w[0] = -0.5 * t3 + t2 - 0.5 * t;
            w[1] = 1.5 * t3 - 2.5 * t2 + 1.0;
            w[2] = -1.5 * t3 + 2.0 * t2 + 0.5 * t;
            w[3] = 0.5 * t3 - 0.5 * t2;
        };
        double wx[4], wy[4];
        weights(tx, wx);
        weights(ty, wy);
        double acc = 0.0;
        for (int dy = 0; dy < 4; ++dy) {
            double row = 0.0;
            for (int dx = 0; dx < 4; ++dx) row += wx[dx] * at(i - 1 + dx, j - 1 + dy);
            acc += wy[dy] * row;
        }
        const float a = at(i, j), b = at(i + 1, j), c = at(i, j + 1), d = at(i + 1, j + 1);
        const float lo = std::min(std::min(a, b), std::min(c, d)), hi = std::max(std::max(a, b), std::max(c, d));
        return std::min(hi, std::max(lo, static_cast<float>(acc)));
    }
};

Plane extract(const MergeResult& m, int ox, int oy) {
    Plane p;
    p.ox = ox;
    p.oy = oy;
    p.w = (m.width - ox + 1) / 2;
    p.h = (m.height - oy + 1) / 2;
    p.v.resize(static_cast<std::size_t>(p.w) * p.h);
    for (int j = 0; j < p.h; ++j) {
        for (int i = 0; i < p.w; ++i) p.v[static_cast<std::size_t>(j) * p.w + i] = m.data[static_cast<std::size_t>(2 * j + oy) * m.width + 2 * i + ox];
    }
    return p;
}

// 見積もりに使う輪郭の点の組（同じ色の隣り合う 2 画素 p・p'）と、その間の G の明るさの対数の差。
struct Pair {
    float x0, y0, x1, y1;
    float dg;
};

}  // namespace

LateralCa estimate_lateral_ca(const MergeResult& m) {
    LateralCa res;
    if (!m.cfa.is_bayer() || m.data.empty()) {
        res.note = "ベイヤー以外は対象外";
        return res;
    }
    const int W = m.width, H = m.height;
    const double cx = 0.5 * (W - 1), cy = 0.5 * (H - 1);
    const double R2 = cx * cx + cy * cy;
    const float* d = m.data.data();
    const auto px = [&](int x, int y) { return d[static_cast<std::size_t>(y) * W + x]; };

    // ノイズの目安: G の面の暗い所での隣どうしの差のばらつき。
    float floor_v;
    {
        int gx = 0, gy = 0;
        site_of(m.cfa, 1, gx, gy);
        const Plane g = extract(m, gx, gy);
        std::vector<float> vals(g.v);
        const std::size_t k = vals.size() / 5;
        std::nth_element(vals.begin(), vals.begin() + k, vals.end());
        const float dark = vals[k];
        std::vector<float> diffs;
        for (int j = 0; j < g.h; j += 3) {
            for (int i = 0; i + 1 < g.w; i += 3) {
                const float a = g.at(i, j), b = g.at(i + 1, j);
                if (a <= dark) diffs.push_back(std::fabs(a - b));
            }
        }
        float sd = 1e-6f;
        if (diffs.size() > 100) {
            std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
            sd = std::max(1e-9f, 1.4826f * diffs[diffs.size() / 2] / std::sqrt(2.0f));
        }
        floor_v = 30.0f * sd;
    }

    const int margin = 8;
    const float sat = 0.98f;
    for (int c : {0, 2}) {
        int ox, oy;
        if (!site_of(m.cfa, c, ox, oy)) continue;
        Plane lin = extract(m, ox, oy);
        Plane lg = lin;
        for (float& v : lg.v) v = std::log2(std::max(v, floor_v));
        // G をこの色の画素の位置で（上下左右の 4 つの G の平均）。
        const auto g_at = [&](int x, int y) { return 0.25f * (px(x - 1, y) + px(x + 1, y) + px(x, y - 1) + px(x, y + 1)); };
        std::vector<Pair> pairs;
        for (int y = oy + margin; y + 2 < H - margin; y += 2) {
            for (int x = ox + margin; x + 2 < W - margin; x += 2) {
                const float g0 = g_at(x, y);
                if (g0 < floor_v || g0 > sat || px(x, y) > sat) continue;
                for (int dir = 0; dir < 2; ++dir) {
                    const int x1 = dir ? x : x + 2, y1 = dir ? y + 2 : y;
                    const float g1 = g_at(x1, y1);
                    if (g1 < floor_v || g1 > sat || px(x1, y1) > sat) continue;
                    const float dg = std::log2(g1) - std::log2(g0);
                    if (std::fabs(dg) < 0.25f) continue;
                    pairs.push_back(Pair{static_cast<float>(x), static_cast<float>(y), static_cast<float>(x1), static_cast<float>(y1), dg});
                }
            }
        }
        res.samples = std::max(res.samples, static_cast<int>(pairs.size()));
        if (pairs.size() < 2000) continue;
        if (pairs.size() > 400000) {
            const std::size_t stride = pairs.size() / 400000 + 1;
            std::vector<Pair> thin;
            for (std::size_t i = 0; i < pairs.size(); i += stride) thin.push_back(pairs[i]);
            pairs.swap(thin);
        }
        const auto error = [&](double a, double b) {
            const int n = static_cast<int>(pairs.size());
            const int chunks = 64;
            std::vector<double> part(chunks, 0.0);
            parallel_for(chunks, [&](int c0, int c1) {
                for (int ch = c0; ch < c1; ++ch) {
                    double e = 0.0;
                    for (int i = ch * n / chunks; i < (ch + 1) * n / chunks; ++i) {
                        const Pair& p = pairs[i];
                        const auto src = [&](float x, float y, double& sx, double& sy) {
                            const double dx = x - cx, dy = y - cy;
                            const double k = 1.0 + a + b * (dx * dx + dy * dy) / R2;
                            sx = cx + dx * k;
                            sy = cy + dy * k;
                        };
                        double sx0, sy0, sx1, sy1;
                        src(p.x0, p.y0, sx0, sy0);
                        src(p.x1, p.y1, sx1, sy1);
                        const double dr = lg.bilinear(sx1, sy1) - lg.bilinear(sx0, sy0);
                        // 外れ値（動いた物・飽和の近く）に引きずられないよう、差は 2 段で頭打ちにする。
                        const double r = std::min(2.0, std::fabs(dr - p.dg));
                        e += r * r;
                    }
                    part[ch] = e;
                }
            }, 1);
            double e = 0.0;
            for (double v : part) e += v;
            return e;
        };
        const double e0 = error(0.0, 0.0);
        double ba = 0.0, bb = 0.0, be = e0;
        const auto search = [&](bool vary_a, double lo, double hi, double step) {
            const double fixed_a = ba, fixed_b = bb;
            for (double t = lo; t <= hi + 1e-12; t += step) {
                const double a = vary_a ? t : fixed_a, b = vary_a ? fixed_b : t;
                const double e = error(a, b);
                if (e < be) {
                    be = e;
                    ba = a;
                    bb = b;
                }
            }
        };
        const double lim = 0.004;
        search(true, -lim, lim, 0.0001);
        search(false, -lim, lim, 0.0002);
        search(true, ba - 0.0004, ba + 0.0004, 0.00002);
        search(false, bb - 0.0004, bb + 0.0004, 0.00002);
        search(true, ba - 0.00004, ba + 0.00004, 0.000005);
        res.improvement[c] = e0 > 0.0 ? 1.0 - be / e0 : 0.0;
        // 改善がわずか・探した範囲の端（模型が合っていない）なら補正しない。
        if (res.improvement[c] < 0.1 || std::fabs(ba) >= lim - 1e-9 || std::fabs(bb) >= lim - 1e-9) continue;
        res.a[c] = ba;
        res.b[c] = bb;
        res.corner_shift_px[c] = (ba + bb) * std::sqrt(R2);
        res.valid = true;
    }
    if (!res.valid && res.note.empty()) res.note = res.samples < 2000 ? "輪郭が少ない" : "補正しても輪郭の色のずれが減らない";
    return res;
}

void apply_lateral_ca(MergeResult& m, const LateralCa& ca) {
    if (!ca.valid || !m.cfa.is_bayer()) return;
    const int W = m.width, H = m.height;
    const double cx = 0.5 * (W - 1), cy = 0.5 * (H - 1);
    const double R2 = cx * cx + cy * cy;
    for (int c : {0, 2}) {
        if (ca.a[c] == 0.0 && ca.b[c] == 0.0) continue;
        int ox, oy;
        if (!site_of(m.cfa, c, ox, oy)) continue;
        const Plane src = extract(m, ox, oy);
        parallel_for((H - oy + 1) / 2, [&](int j0, int j1) {
            for (int j = j0; j < j1; ++j) {
                const int y = 2 * j + oy;
                for (int x = ox; x < W; x += 2) {
                    const double dx = x - cx, dy = y - cy;
                    const double k = 1.0 + ca.a[c] + ca.b[c] * (dx * dx + dy * dy) / R2;
                    m.data[static_cast<std::size_t>(y) * W + x] = src.bicubic(cx + dx * k, cy + dy * k);
                }
            }
        });
    }
}

}  // namespace hdr
