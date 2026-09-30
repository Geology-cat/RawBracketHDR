#include "hdrcore/exposure.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

#include "hdrcore/parallel.hpp"

namespace hdr {

// ---- 飽和レベル --------------------------------------------------------------------
//
// 色ごとに値のヒストグラムを取り、最大値のすぐ下に画素が固まっていれば（飽和の山）
// そこを飽和レベルとする。最大値の画素がわずかしかなければ、そのフレームは飽和していない。
ClipLevels detect_clip_levels(const RawFrame& f) {
    ClipLevels out;
    const int kBins = 1 << 16;
    std::vector<uint32_t> hist[3];
    for (auto& h : hist) h.assign(kBins, 0);
    std::mutex mu;
    parallel_for(f.height, [&](int y0, int y1) {
        std::vector<uint32_t> local[3];
        for (auto& h : local) h.assign(kBins, 0);
        for (int y = y0; y < y1; ++y) {
            const float* row = f.data.data() + static_cast<std::size_t>(y) * f.width;
            for (int x = 0; x < f.width; ++x) {
                const float v = row[x];
                if (v <= 0.0f) continue;
                const int b = std::min(kBins - 1, static_cast<int>(v + 0.5f));
                ++local[f.cfa.at(x, y)][b];
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < kBins; ++i) hist[c][i] += local[c][i];
        }
    });
    for (int c = 0; c < 3; ++c) {
        uint64_t total = 0;
        int vmax = -1;
        for (int i = 0; i < kBins; ++i) {
            total += hist[c][i];
            if (hist[c][i]) vmax = i;
        }
        out.level[c] = f.white[c];
        if (vmax <= 0) continue;
        const int tol = std::max(2, static_cast<int>(vmax * 0.001));
        uint64_t near = 0;
        for (int i = std::max(0, vmax - tol); i <= vmax; ++i) near += hist[c][i];
        const uint64_t need = std::max<uint64_t>(16, total / 50000);
        if (near >= need) {
            out.level[c] = static_cast<float>(vmax - tol);
            out.detected[c] = true;
        }
    }
    return out;
}

namespace {

struct Sample {
    float b, d;  // 明るいフレーム・暗いフレームのブロック平均（同じ色）
    uint8_t c;
};

// D = s·B + o を最小二乗で当てはめる。相対残差の大きいものを外して繰り返す。
void robust_fit(const std::vector<Sample>& samples, int channel, double& slope, double& offset, int& used) {
    std::vector<const Sample*> s;
    s.reserve(samples.size());
    for (const Sample& x : samples) {
        if (channel < 0 || x.c == channel) s.push_back(&x);
    }
    slope = 0.0;
    offset = 0.0;
    used = static_cast<int>(s.size());
    if (s.size() < 50) return;
    for (int iter = 0; iter < 4; ++iter) {
        double sb = 0, sd = 0, sbb = 0, sbd = 0;
        const double n = static_cast<double>(s.size());
        for (const Sample* x : s) {
            sb += x->b;
            sd += x->d;
            sbb += static_cast<double>(x->b) * x->b;
            sbd += static_cast<double>(x->b) * x->d;
        }
        const double den = n * sbb - sb * sb;
        if (!(std::fabs(den) > 0.0)) return;
        slope = (n * sbd - sb * sd) / den;
        offset = (sd - slope * sb) / n;
        if (iter == 3) break;
        // 相対残差の中央絶対偏差で外れ値を外す。
        std::vector<double> rel(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            const double pred = slope * s[i]->b + offset;
            rel[i] = (s[i]->d - pred) / std::max(1.0, std::fabs(pred));
        }
        std::vector<double> tmp(rel.size());
        for (std::size_t i = 0; i < rel.size(); ++i) tmp[i] = std::fabs(rel[i]);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        const double mad = std::max(1e-6, tmp[tmp.size() / 2]);
        std::vector<const Sample*> kept;
        kept.reserve(s.size());
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (std::fabs(rel[i]) <= 5.0 * mad) kept.push_back(s[i]);
        }
        if (kept.size() < 50) break;
        s.swap(kept);
    }
    used = static_cast<int>(s.size());
}

PairFit fit_pair(const RawFrame& dark, const ClipLevels& clip_d, const RawFrame& bright, const ClipLevels& clip_b,
                 double nominal, const ExposureOptions& opt) {
    PairFit fit;
    fit.nominal_ratio = nominal;
    // ブロック（CFA の周期の4倍四方）ごとに、色ごとの平均と最大を取る。
    const int bs = dark.cfa.w * 2;
    const int bw = dark.width / bs, bh = dark.height / bs;
    std::vector<Sample> fit_samples, diag_samples;
    std::mutex mu;
    parallel_for(bh, [&](int by0, int by1) {
        std::vector<Sample> lf, ld;
        for (int by = by0; by < by1; ++by) {
            for (int bx = 0; bx < bw; ++bx) {
                double sum_b[3] = {}, sum_d[3] = {};
                float max_b[3] = {}, max_d[3] = {};
                float min_b[3] = {1e30f, 1e30f, 1e30f};
                int n[3] = {};
                for (int y = by * bs; y < (by + 1) * bs; ++y) {
                    for (int x = bx * bs; x < (bx + 1) * bs; ++x) {
                        const int c = dark.cfa.at(x, y);
                        const float vb = bright.value(x, y), vd = dark.value(x, y);
                        sum_b[c] += vb;
                        sum_d[c] += vd;
                        max_b[c] = std::max(max_b[c], vb);
                        min_b[c] = std::min(min_b[c], vb);
                        max_d[c] = std::max(max_d[c], vd);
                        ++n[c];
                    }
                }
                for (int c = 0; c < 3; ++c) {
                    if (!n[c]) continue;
                    if (max_b[c] >= clip_b.level[c] || max_d[c] >= clip_d.level[c]) continue;
                    const Sample s{static_cast<float>(sum_b[c] / n[c]), static_cast<float>(sum_d[c] / n[c]),
                                   static_cast<uint8_t>(c)};
                    if (s.b < opt.fit_lower * clip_b.level[c]) continue;
                    ld.push_back(s);
                    // 輪郭をまたぐブロックは、わずかなずれや動体で比が狂うので当てはめには使わない。
                    const bool flat = max_b[c] - min_b[c] <= 0.3f * s.b + 0.01f * clip_b.level[c];
                    if (flat && max_b[c] < opt.fit_upper * clip_b.level[c]) lf.push_back(s);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        fit_samples.insert(fit_samples.end(), lf.begin(), lf.end());
        diag_samples.insert(diag_samples.end(), ld.begin(), ld.end());
    });

    // 黒のわずかなずれ・長秒の暗電流・かぶりなどの「足し算の誤差」は、値が小さいほど比を狂わせる。
    // そのため、明るいフレームの値が大きい所だけで当てはめ、数が足りないときだけ下限を下げる。
    // 合成で比が効くのは切り替わる明るさ（明るいフレームの飽和の手前）なので、この選び方で合う。
    struct Tier {
        double lower;
        std::size_t need;
    };
    static const Tier kTiers[] = {{0.10, 300}, {0.03, 1000}, {0.01, 3000}};
    bool enough = false;
    std::vector<Sample> use;
    for (const Tier& t : kTiers) {
        use.clear();
        for (const Sample& s : fit_samples) {
            if (s.b >= t.lower * clip_b.level[s.c]) use.push_back(s);
        }
        if (use.size() >= t.need) {
            enough = true;
            break;
        }
    }
    fit_samples.swap(use);
    double slope = 0, offset = 0;
    int used = 0;
    if (enough) robust_fit(fit_samples, -1, slope, offset, used);
    fit.samples = used;
    if (enough && slope > 0.0) {
        fit.ratio = 1.0 / slope;
        fit.offset = offset;
        fit.measured = true;
        // 名目値から大きく外れたら（1/3段以上）、動体や光源の変化を疑って名目値に戻す。
        if (std::fabs(std::log2(fit.ratio / nominal)) > 0.34) fit.measured = false;
    }
    if (!fit.measured) {
        fit.ratio = nominal;
        fit.offset = 0.0;
    }
    for (int c = 0; c < 3; ++c) {
        double s = 0, o = 0;
        int u = 0;
        robust_fit(fit_samples, c, s, o, u);
        fit.ratio_channel[c] = s > 0.0 && u >= 200 ? 1.0 / s : 0.0;
    }

    // 明るさの帯ごとの比（非線形性の診断）。
    static const double kEdges[] = {0.01, 0.02, 0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.85, 0.9, 0.95, 0.98, 1.0};
    const int nb = static_cast<int>(sizeof(kEdges) / sizeof(kEdges[0])) - 1;
    std::vector<double> sb(nb, 0.0), sd(nb, 0.0);
    std::vector<int> cnt(nb, 0);
    for (const Sample& s : diag_samples) {
        const double frac = s.b / clip_b.level[s.c];
        for (int i = 0; i < nb; ++i) {
            if (frac >= kEdges[i] && frac < kEdges[i + 1]) {
                sb[i] += s.b;
                sd[i] += s.d - fit.offset;
                ++cnt[i];
                break;
            }
        }
    }
    for (int i = 0; i < nb; ++i) {
        RatioBin b;
        b.lo = kEdges[i];
        b.hi = kEdges[i + 1];
        b.samples = cnt[i];
        b.ratio = cnt[i] > 0 && sd[i] > 0.0 ? sb[i] / sd[i] : 0.0;
        fit.bins.push_back(b);
    }
    return fit;
}

}  // namespace

ExposurePlan estimate_exposures(const std::vector<RawFrame>& frames, const ExposureOptions& options) {
    ExposurePlan plan;
    const int n = static_cast<int>(frames.size());
    plan.order.resize(n);
    std::iota(plan.order.begin(), plan.order.end(), 0);
    std::stable_sort(plan.order.begin(), plan.order.end(),
                     [&](int a, int b) { return frames[a].nominal_ev() < frames[b].nominal_ev(); });

    // 飽和レベル。飽和の山が見つからないフレームは、同じ ISO のフレームで見つかった値を借りる
    // （同じ ISO なら飽和値は同じ）。どれにも無ければ機種表の白。
    plan.clip.resize(n);
    for (int i = 0; i < n; ++i) plan.clip[i] = detect_clip_levels(frames[i]);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            if (plan.clip[i].detected[c]) continue;
            float best = 0.0f;
            for (int j = 0; j < n; ++j) {
                if (j == i || !plan.clip[j].detected[c] || frames[j].iso != frames[i].iso) continue;
                if (best == 0.0f || plan.clip[j].level[c] < best) best = plan.clip[j].level[c];
            }
            if (best > 0.0f) plan.clip[i].level[c] = best;
        }
    }

    // 名目の露光量。EXIF のシャッター速度は「1/320」のように丸めた表示値なので、
    // 1/3段または1/2段の刻み（合うほう）に寄せてから比を取る。
    std::vector<double> ev(n);
    for (int i = 0; i < n; ++i) ev[i] = frames[i].nominal_ev();
    double best_err = 1e9, best_step = 0.0;
    for (double step : {1.0 / 3.0, 0.5}) {
        double err = 0.0;
        for (double e : ev) err += std::fabs(e - std::round(e / step) * step);
        if (err < best_err - 1e-9) {
            best_err = err;
            best_step = step;
        }
    }
    if (n > 0 && best_err / n < 0.06) {
        for (double& e : ev) e = std::round(e / best_step) * best_step;
    }

    plan.rel_exposure.assign(n, 1.0);
    for (int k = 0; k + 1 < n; ++k) {
        const int d = plan.order[k], b = plan.order[k + 1];
        PairFit fit = fit_pair(frames[d], plan.clip[d], frames[b], plan.clip[b], std::exp2(ev[b] - ev[d]), options);
        fit.dark = d;
        fit.bright = b;
        plan.rel_exposure[k + 1] = plan.rel_exposure[k] * fit.ratio;
        plan.fits.push_back(std::move(fit));
    }
    return plan;
}

}  // namespace hdr
