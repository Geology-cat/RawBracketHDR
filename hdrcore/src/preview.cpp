#include "hdrcore/preview.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

bool invert3(const double m[3][3], double out[3][3]) {
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!(std::fabs(det) > 1e-12)) return false;
    const double inv = 1.0 / det;
    out[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
    out[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
    out[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
    out[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
    out[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
    out[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
    out[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
    out[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
    out[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
    return true;
}

// カメラRGB（ホワイトバランス済み）→ 線形 sRGB の行列。dcraw と同じ求め方
// （カメラ←sRGB の行列の各行を和 1 に正規化してから逆行列を取る）。
void camera_to_srgb(const double cm[3][3], double out[3][3]) {
    static const double kXyzFromSrgb[3][3] = {
        {0.4124564, 0.3575761, 0.1804375}, {0.2126729, 0.7151522, 0.0721750}, {0.0193339, 0.1191920, 0.9503041}};
    double cam_rgb[3][3];
    for (int i = 0; i < 3; ++i) {
        double sum = 0.0;
        for (int j = 0; j < 3; ++j) {
            cam_rgb[i][j] = 0.0;
            for (int k = 0; k < 3; ++k) cam_rgb[i][j] += cm[i][k] * kXyzFromSrgb[k][j];
            sum += cam_rgb[i][j];
        }
        if (std::fabs(sum) > 1e-9) {
            for (int j = 0; j < 3; ++j) cam_rgb[i][j] /= sum;
        }
    }
    if (!invert3(cam_rgb, out)) {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) out[i][j] = i == j ? 1.0 : 0.0;
        }
    }
}

inline double shoulder(double x) {
    // 0.5 までは線形、その先は 1.0 に漸近する（0.5 で傾き 1 のまま繋がる）。
    if (x <= 0.5) return x;
    return 0.5 + 0.5 * (1.0 - std::exp(-(x - 0.5) / 0.5));
}

inline uint8_t to_srgb8(double v) {
    v = std::min(1.0, std::max(0.0, v));
    const double s = v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::lround(s * 255.0));
}

}  // namespace

Rgb8Image render_preview(const float* cfa, int width, int height, const CfaPattern& pattern,
                         const double color_matrix[3][3], const double neutral[3], const PreviewOptions& opt) {
    Rgb8Image out;
    const int block = pattern.is_xtrans() ? 3 : 2;
    const int longest = std::max(width, height);
    int step = std::max(1, static_cast<int>(std::ceil(static_cast<double>(longest) / block / std::max(16, opt.max_size))));
    const int cell = step * block;
    out.width = width / cell;
    out.height = height / cell;
    if (out.width <= 0 || out.height <= 0) return out;
    out.rgb.assign(static_cast<std::size_t>(out.width) * out.height * 3, 0);

    double m[3][3];
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) sum += std::fabs(color_matrix[i][j]);
    }
    if (sum > 0.0) {
        camera_to_srgb(color_matrix, m);
    } else {
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i][j] = i == j ? 1.0 : 0.0;
        }
    }
    const double gain = std::exp2(opt.exposure_ev);
    double wb[3];
    for (int c = 0; c < 3; ++c) wb[c] = neutral[c] > 0.0 ? 1.0 / neutral[c] : 1.0;

    parallel_for(out.height, [&](int y0, int y1) {
        for (int oy = y0; oy < y1; ++oy) {
            for (int ox = 0; ox < out.width; ++ox) {
                double s[3] = {};
                int n[3] = {};
                for (int y = oy * cell; y < (oy + 1) * cell; ++y) {
                    const float* row = cfa + static_cast<std::size_t>(y) * width;
                    for (int x = ox * cell; x < (ox + 1) * cell; ++x) {
                        const int c = pattern.at(x, y);
                        s[c] += row[x];
                        ++n[c];
                    }
                }
                double cam[3];
                for (int c = 0; c < 3; ++c) cam[c] = (n[c] ? s[c] / n[c] : 0.0) * wb[c] * gain;
                uint8_t* dst = out.rgb.data() + (static_cast<std::size_t>(oy) * out.width + ox) * 3;
                for (int i = 0; i < 3; ++i) {
                    double v = m[i][0] * cam[0] + m[i][1] * cam[1] + m[i][2] * cam[2];
                    if (opt.tone_map) v = shoulder(v);
                    dst[i] = to_srgb8(v);
                }
            }
        }
    });
    return out;
}

Rgb8Image render_frame_preview(const RawFrame& f, const PreviewOptions& options) {
    // 白レベルで割ってから渡す（1.0 = 白）。
    std::vector<float> norm(f.data.size());
    const float inv[3] = {1.0f / f.white[0], 1.0f / f.white[1], 1.0f / f.white[2]};
    parallel_for(f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < f.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * f.width + x;
                norm[i] = f.data[i] * inv[f.cfa.at(x, y)];
            }
        }
    });
    return render_preview(norm.data(), f.width, f.height, f.cfa, f.color_matrix, f.as_shot_neutral, options);
}

Rgb8Image apply_orientation(const Rgb8Image& in, int orientation) {
    if (orientation <= 1 || orientation > 8) return in;
    const bool swap = orientation >= 5;
    Rgb8Image out;
    out.width = swap ? in.height : in.width;
    out.height = swap ? in.width : in.height;
    out.rgb.resize(in.rgb.size());
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            int sx = x, sy = y;
            const int W = out.width, H = out.height;
            switch (orientation) {
                case 2: sx = W - 1 - x; sy = y; break;
                case 3: sx = W - 1 - x; sy = H - 1 - y; break;
                case 4: sx = x; sy = H - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = y; sy = W - 1 - x; break;
                case 7: sx = H - 1 - y; sy = W - 1 - x; break;
                case 8: sx = H - 1 - y; sy = x; break;
                default: break;
            }
            const uint8_t* s = in.rgb.data() + (static_cast<std::size_t>(sy) * in.width + sx) * 3;
            uint8_t* d = out.rgb.data() + (static_cast<std::size_t>(y) * out.width + x) * 3;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
        }
    }
    return out;
}

}  // namespace hdr
