#include "hdrcore/output_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace hdr {

const char* format_name(OutputFormat f) {
    switch (f) {
        case OutputFormat::Auto: return "自動";
        case OutputFormat::Cfa: return "CFA";
        case OutputFormat::LinearRaw: return "LinearRaw";
    }
    return "";
}

FormatDecision decide_output_format(const MergeResult& m, OutputFormat requested) {
    FormatDecision d;
    // 同じ色（緑）の近い2画素の組を集める。Bayer は斜め、X-Trans は隣にある。
    int ox = 0, oy = 0;
    for (const auto& o : {std::pair<int, int>{1, 1}, {1, 0}, {0, 1}}) {
        bool all = true;
        for (int y = 0; y < m.cfa.h && all; ++y) {
            for (int x = 0; x < m.cfa.w && all; ++x) {
                if (m.cfa.at(x, y) == 1 && m.cfa.at(x + o.first, y + o.second) != 1) all = false;
            }
        }
        if (all) {
            ox = o.first;
            oy = o.second;
            break;
        }
    }
    struct Pair {
        float level, diff;
    };
    std::vector<Pair> pairs;
    pairs.reserve(static_cast<std::size_t>(m.width) * m.height / 32);
    for (int y = 0; y + oy < m.height; y += 4) {
        const float* r0 = m.data.data() + static_cast<std::size_t>(y) * m.width;
        const float* r1 = m.data.data() + static_cast<std::size_t>(y + oy) * m.width;
        for (int x = 0; x + ox < m.width; ++x) {
            if (m.cfa.at(x, y) != 1) continue;
            const float a = r0[x], b = r1[x + ox];
            if (a <= 0.0f || b <= 0.0f) continue;  // 0 で切られた所はノイズが測れない
            pairs.push_back({0.5f * (a + b), std::fabs(a - b)});
        }
    }
    const double step = std::ldexp(1.0, -16);
    if (pairs.size() >= 1000) {
        std::sort(pairs.begin(), pairs.end(), [](const Pair& p, const Pair& q) { return p.level < q.level; });
        const std::size_t lo = pairs.size() * 2 / 100, hi = std::max(lo + 1, pairs.size() * 20 / 100);
        std::vector<float> diffs, levels;
        for (std::size_t i = lo; i < hi; ++i) {
            diffs.push_back(pairs[i].diff);
            levels.push_back(pairs[i].level);
        }
        std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
        std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
        // |a−b| の中央値 ×1.4826 は差の標準偏差の推定。1画素の標準偏差はその 1/√2。
        d.shadow_noise = diffs[diffs.size() / 2] * 1.4826 / std::sqrt(2.0);
        d.shadow_level = levels[levels.size() / 2];
        d.noise_to_step = d.shadow_noise / step;
    }
    char buf[256];
    if (requested == OutputFormat::Auto) {
        d.chosen = d.noise_to_step >= kCfaNoiseToStep ? OutputFormat::Cfa : OutputFormat::LinearRaw;
        std::snprintf(buf, sizeof(buf), "暗部のノイズが Camera Raw の刻みの %.2f 倍 → %s", d.noise_to_step,
                      d.chosen == OutputFormat::Cfa ? "CFA（明暗差が Camera Raw の範囲に収まる）"
                                                    : "LinearRaw（CFA では暗部に段差が出るため）");
    } else {
        d.chosen = requested;
        std::snprintf(buf, sizeof(buf), "指定により %s（暗部のノイズは刻みの %.2f 倍）", format_name(requested), d.noise_to_step);
    }
    d.reason = buf;
    return d;
}

}  // namespace hdr
