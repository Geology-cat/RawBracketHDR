#include "hdrcore/tone_compress.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/filters.hpp"
#include "hdrcore/parallel.hpp"

namespace hdr {

ToneCompressResult compress_tone(MergeResult& m, const double neutral[3], const ToneCompressOptions& opt) {
    ToneCompressResult res;
    m.gain.clear();
    m.max_gain = 1.0;
    m.opening_ev = 0.0;
    m.tone_strength = 0.0;
    const double s = std::min(1.0, std::max(0.0, opt.strength));
    if (s <= 0.0 || m.data.empty()) return res;
    const int b = m.block, gw = m.grid_w, gh = m.grid_h;
    const std::size_t cells = static_cast<std::size_t>(gw) * gh;
    // 基準フレームの白（合成の値の単位）。明るさはこれに対する段で扱う。
    const double wref = 1.0 / m.white_scale;
    double wb[3];
    for (int c = 0; c < 3; ++c) wb[c] = neutral[c] > 0.0 ? 1.0 / neutral[c] : 1.0;
    const double coef[3] = {0.25, 0.5, 0.25};

    // ---- ブロックの明るさ（ホワイトバランスを掛けた色の平均）の対数 ----
    std::vector<float> guide(cells);
    const double floor_l = wref * std::ldexp(1.0, -18);
    parallel_for(gh, [&](int by0, int by1) {
        for (int by = by0; by < by1; ++by) {
            for (int bx = 0; bx < gw; ++bx) {
                double sum[3] = {};
                int n[3] = {};
                for (int y = by * b; y < std::min(m.height, (by + 1) * b); ++y) {
                    for (int x = bx * b; x < std::min(m.width, (bx + 1) * b); ++x) {
                        const int c = m.cfa.at(x, y);
                        sum[c] += m.data[static_cast<std::size_t>(y) * m.width + x];
                        ++n[c];
                    }
                }
                double l = 0.0, wsum = 0.0;
                for (int c = 0; c < 3; ++c) {
                    if (!n[c]) continue;
                    l += coef[c] * wb[c] * sum[c] / n[c];
                    wsum += coef[c];
                }
                l = wsum > 0.0 ? l / wsum : 0.0;
                guide[static_cast<std::size_t>(by) * gw + bx] = static_cast<float>(std::log2(std::max(l, floor_l) / wref));
            }
        }
    });
    // 暗い所のノイズで倍率が画素ごとに揺れないよう、案内の値を軽く（3×3 ブロック）ならす。
    {
        std::vector<float> tmp(cells);
        parallel_for(gh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                for (int x = 0; x < gw; ++x) {
                    double acc = 0.0;
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int yy = y + dy;
                        if (yy < 0 || yy >= gh) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int xx = x + dx;
                            if (xx < 0 || xx >= gw) continue;
                            acc += guide[static_cast<std::size_t>(yy) * gw + xx];
                            ++n;
                        }
                    }
                    tmp[static_cast<std::size_t>(y) * gw + x] = static_cast<float>(acc / n);
                }
            }
        });
        guide.swap(tmp);
    }

    // ---- 大まかな明るさ（輪郭を残して滑らかに） ----
    const float space = std::max(4.0f, std::max(gw, gh) / 80.0f);
    const std::vector<float> base = bilateral_smooth(guide, gw, gh, space, static_cast<float>(opt.range_sigma));
    float hi = -1e9f;
    for (float v : base) hi = std::max(hi, v);
    std::vector<float> sorted(base);
    const std::size_t k_lo = cells * 5 / 1000;
    std::nth_element(sorted.begin(), sorted.begin() + k_lo, sorted.end());
    float lo = sorted[k_lo];
    std::nth_element(sorted.begin(), sorted.begin() + cells / 2, sorted.end());
    const float median = sorted[cells / 2];
    res.before_span = hi - lo;
    // 開いたときの明るさを整える（白を shift 段だけ下げたことにする）。
    double shift = 0.0;
    if (opt.auto_brightness) {
        shift = std::min(opt.max_brightness_change, std::max(-opt.max_brightness_change, opt.target_median - median));
    }
    hi += static_cast<float>(shift);
    lo += static_cast<float>(shift);

    // 明るい所の細かい模様は大まかな明るさより上に振れるので、その分だけ行き先を下げ、
    // 模様の明るい部分（月の明るい縁など）まで白の手前に収める。
    double overshoot = 0.0;
    {
        std::vector<float> over;
        for (std::size_t i = 0; i < cells; ++i) {
            if (base[i] + shift > opt.highlight_knee) over.push_back(guide[i] - base[i]);
        }
        if (over.size() > 20) {
            const std::size_t k = over.size() * 995 / 1000;
            std::nth_element(over.begin(), over.begin() + k, over.end());
            overshoot = std::min(2.0, std::max(0.0, static_cast<double>(over[k])));
        }
    }
    // ---- 大まかな明るさの行き先 ----
    const double hk = opt.highlight_knee, sk = opt.shadow_knee;
    const double top = std::max(hk + 0.1, opt.highlight_top - overshoot);
    const KneeCurve high(top - hk, hi - hk);
    const KneeCurve low(sk - opt.shadow_floor, sk - lo);
    m.gain.assign(cells, 1.0f);
    double gmin = 1e9, gmax = 0.0;
    for (std::size_t i = 0; i < cells; ++i) {
        const double b0 = base[i] + shift;
        double nb = b0;
        if (b0 > hk) nb = hk + high(b0 - hk);
        else if (b0 < sk) nb = sk - low(sk - b0);
        const double g = std::exp2(s * (nb - b0));
        m.gain[i] = static_cast<float>(g);
        gmin = std::min(gmin, g);
        gmax = std::max(gmax, g);
    }
    // 倍率を数画素の幅で滑らかにつなぐ（対数で 3×3 ブロックの平均を 2 回）。輪郭で倍率が急に変わると、
    // 隣り合う色の画素の関係が崩れ、色補間で縁に色の縞が出る（月の縁で確認）。
    {
        std::vector<float> lg(cells), tmp(cells);
        for (std::size_t i = 0; i < cells; ++i) lg[i] = std::log2(m.gain[i]);
        for (int pass = 0; pass < 2; ++pass) {
            parallel_for(gh, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    for (int x = 0; x < gw; ++x) {
                        double acc = 0.0;
                        int n = 0;
                        for (int dy = -1; dy <= 1; ++dy) {
                            const int yy = std::min(gh - 1, std::max(0, y + dy));
                            for (int dx = -1; dx <= 1; ++dx) {
                                acc += lg[static_cast<std::size_t>(yy) * gw + std::min(gw - 1, std::max(0, x + dx))];
                                ++n;
                            }
                        }
                        tmp[static_cast<std::size_t>(y) * gw + x] = static_cast<float>(acc / n);
                    }
                }
            });
            lg.swap(tmp);
        }
        gmin = 1e9;
        gmax = 0.0;
        for (std::size_t i = 0; i < cells; ++i) {
            m.gain[i] = std::exp2(lg[i]);
            gmin = std::min(gmin, static_cast<double>(m.gain[i]));
            gmax = std::max(gmax, static_cast<double>(m.gain[i]));
        }
    }
    res.after_span = res.before_span + std::log2(std::max(1e-9, gmin) / std::max(1e-9, gmax));

    // ---- 倍率を掛ける（ブロックの全画素・全色に同じ値） ----
    parallel_for(m.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* row = m.data.data() + static_cast<std::size_t>(y) * m.width;
            const float* g = m.gain.data() + static_cast<std::size_t>(y / b) * gw;
            for (int x = 0; x < m.width; ++x) row[x] = std::min(1.0f, row[x] * g[x / b]);
        }
    });
    m.max_gain = gmax;
    m.opening_ev = shift;
    m.tone_strength = s;
    res.opening_ev = shift;
    res.applied = true;
    res.min_gain = gmin;
    res.max_gain = gmax;
    return res;
}

}  // namespace hdr
