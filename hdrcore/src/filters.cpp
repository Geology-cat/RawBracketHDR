#include "hdrcore/filters.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/parallel.hpp"

namespace hdr {

// 明るさの対数を、明るさの差が range_sigma 段より大きい輪郭を残して滑らかにする（バイラテラルグリッド）。
// 空間の刻み space（画素）× 明るさの刻み range_sigma（段）の格子に画素を配り、格子の上でぼかしてから、
// 各画素の位置と明るさで読み戻す。月の縁（10 段）は残し、月の中の模様（1 段未満）はならす。
std::vector<float> bilateral_smooth(const std::vector<float>& l, int w, int h, float space, float range_sigma) {
    float lmin = 1e9f, lmax = -1e9f;
    for (float v : l) {
        lmin = std::min(lmin, v);
        lmax = std::max(lmax, v);
    }
    const int gx = static_cast<int>(w / space) + 3, gy = static_cast<int>(h / space) + 3;
    const int gz = static_cast<int>((lmax - lmin) / range_sigma) + 3;
    const std::size_t cells = static_cast<std::size_t>(gx) * gy * gz;
    std::vector<float> val(cells, 0.0f), wt(cells, 0.0f);
    const auto idx = [&](int x, int y, int z) { return (static_cast<std::size_t>(z) * gy + y) * gx + x; };
    // 配る（最も近い格子点へ）。
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = l[static_cast<std::size_t>(y) * w + x];
            const int cx = static_cast<int>(x / space + 0.5f) + 1, cy = static_cast<int>(y / space + 0.5f) + 1;
            const int cz = static_cast<int>((v - lmin) / range_sigma + 0.5f) + 1;
            val[idx(cx, cy, cz)] += v;
            wt[idx(cx, cy, cz)] += 1.0f;
        }
    }
    // 格子の上で [1,2,1] を各方向に 2 回ずつ。
    std::vector<float> tv(cells), tw(cells);
    for (int pass = 0; pass < 2; ++pass) {
        for (int axis = 0; axis < 3; ++axis) {
            const std::size_t stride = axis == 0 ? 1 : axis == 1 ? static_cast<std::size_t>(gx) : static_cast<std::size_t>(gx) * gy;
            const int len = axis == 0 ? gx : axis == 1 ? gy : gz;
            for (std::size_t i = 0; i < cells; ++i) {
                const int pos = static_cast<int>((i / stride) % len);
                const std::size_t im = pos > 0 ? i - stride : i, ip = pos + 1 < len ? i + stride : i;
                tv[i] = 0.25f * val[im] + 0.5f * val[i] + 0.25f * val[ip];
                tw[i] = 0.25f * wt[im] + 0.5f * wt[i] + 0.25f * wt[ip];
            }
            val.swap(tv);
            wt.swap(tw);
        }
    }
    // 読み戻す（3 方向の線形補間）。
    std::vector<float> out(l.size());
    parallel_for(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                const float v = l[static_cast<std::size_t>(y) * w + x];
                const float fx = x / space + 1.0f, fy = y / space + 1.0f, fz = (v - lmin) / range_sigma + 1.0f;
                const int x0 = std::min(gx - 2, static_cast<int>(fx)), y0b = std::min(gy - 2, static_cast<int>(fy)), z0 = std::min(gz - 2, static_cast<int>(fz));
                const float ax = fx - x0, ay = fy - y0b, az = fz - z0;
                double sv = 0.0, sw = 0.0;
                for (int dz = 0; dz < 2; ++dz) {
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            const float f = (dx ? ax : 1 - ax) * (dy ? ay : 1 - ay) * (dz ? az : 1 - az);
                            const std::size_t k = idx(x0 + dx, y0b + dy, z0 + dz);
                            sv += f * val[k];
                            sw += f * wt[k];
                        }
                    }
                }
                out[static_cast<std::size_t>(y) * w + x] = sw > 1e-6 ? static_cast<float>(sv / sw) : v;
            }
        }
    });
    return out;
}

KneeCurve::KneeCurve(double room_, double span_) : room(room_), span(span_) {
    if (!(span > room) || room <= 0.0) return;
    double lo = 1e-6, hi = 1e6;
    for (int i = 0; i < 100; ++i) {
        const double mid = std::sqrt(lo * hi);
        const double slope = room * mid / std::log1p(mid * span);
        if (slope > 1.0) hi = mid; else lo = mid;
    }
    a = std::sqrt(lo * hi);
    active = true;
}

double KneeCurve::operator()(double t) const { return active ? room * std::log1p(a * t) / std::log1p(a * span) : t; }

}  // namespace hdr
