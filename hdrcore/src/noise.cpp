#include "hdrcore/noise.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

#include "hdrcore/parallel.hpp"

namespace hdr {

NoiseModel scale_noise(const NoiseModel& m, double k) {
    NoiseModel r = m;
    for (int c = 0; c < 3; ++c) {
        r.S[c] = m.S[c] * k;
        r.O[c] = m.O[c] * k * k;
    }
    return r;
}

NoiseModel estimate_noise(const RawFrame& f, const ClipLevels& clip) {
    NoiseModel model;
    // 明るさの帯（DN）。暗い所の読み出しノイズ（O）が大事なので、0 付近を細かく。
    std::vector<double> edges = {-1e9, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    const int nb = static_cast<int>(edges.size()) - 1;
    struct Bin {
        std::vector<float> d;
        double level = 0.0;
    };
    std::vector<Bin> bins[3];
    for (auto& b : bins) b.assign(nb, Bin());
    std::mutex mu;
    // 同じ色で横に並ぶ3画素の2階差分 a − 2b + c（分散は 6σ²）。明るさのなだらかな傾きを打ち消すので、
    // 被写体の模様をノイズと取り違えにくい。間隔は Bayer なら 2、X-Trans は 1〜6。
    parallel_for(f.height, [&](int y0, int y1) {
        std::vector<Bin> local[3];
        for (auto& b : local) b.assign(nb, Bin());
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x + 12 < f.width; ++x) {
                const int c = f.cfa.at(x, y);
                int k = 1;
                while (k <= 6 && f.cfa.at(x + k, y) != c) ++k;
                if (k > 6 || f.cfa.at(x + 2 * k, y) != c) continue;
                const double a = f.value(x, y), b = f.value(x + k, y), cc = f.value(x + 2 * k, y);
                if (std::max(a, std::max(b, cc)) >= 0.9 * clip.level[c]) continue;
                const double lv = b;
                const int i = static_cast<int>(std::upper_bound(edges.begin(), edges.end(), lv) - edges.begin()) - 1;
                if (i < 0 || i >= nb) continue;
                local[c][i].d.push_back(static_cast<float>(std::fabs(a - 2.0 * b + cc)));
                local[c][i].level += lv;
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        for (int c = 0; c < 3; ++c) {
            for (int i = 0; i < nb; ++i) {
                bins[c][i].d.insert(bins[c][i].d.end(), local[c][i].d.begin(), local[c][i].d.end());
                bins[c][i].level += local[c][i].level;
            }
        }
    }, 16);
    bool ok = true;
    for (int c = 0; c < 3; ++c) {
        // 帯ごとの分散（中央絶対偏差から。2階差分の分散の 1/6 が1画素の分散）。
        double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        int used = 0;
        for (int i = 0; i < nb; ++i) {
            Bin& b = bins[c][i];
            const std::size_t n = b.d.size();
            if (n < 200) continue;
            const double lv = b.level / n;
            if (lv > 0.5 * clip.level[c]) continue;
            std::nth_element(b.d.begin(), b.d.begin() + n / 2, b.d.end());
            const double sd = 1.4826 * b.d[n / 2];
            const double var = sd * sd / 6.0;
            // 分散は明るさとともに大きくなるので、相対誤差がそろうよう 1/var² で重み付けする。
            const double w = static_cast<double>(n) / std::max(1e-9, var * var);
            const double x = std::max(0.0, lv);
            sw += w;
            sx += w * x;
            sy += w * var;
            sxx += w * x * x;
            sxy += w * x * var;
            ++used;
        }
        const double den = sw * sxx - sx * sx;
        if (used < 3 || !(std::fabs(den) > 0.0)) {
            ok = false;
            continue;
        }
        double S = (sw * sxy - sx * sy) / den;
        double O = (sy - S * sx) / sw;
        if (S < 0.0) {
            S = 0.0;
            O = sy / sw;
        }
        model.S[c] = S;
        model.O[c] = std::max(O, 0.01);
    }
    model.valid = ok;
    return model;
}

}  // namespace hdr
