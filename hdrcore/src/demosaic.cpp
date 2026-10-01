#include "hdrcore/demosaic.hpp"

#include <algorithm>
#include <cmath>

#include "hdrcore/parallel.hpp"

namespace hdr {

namespace {

inline float sqr(float x) { return x * x; }
// intp(a, b, c) = a·b + (1 − a)·c
inline float intp(float a, float b, float c) { return a * (b - c) + c; }

// 画像の外は、色の並びを崩さない折り返しで補う（x → −x、W−1 を軸に W−1−(x−(W−1))）。
// どちらも偶奇が変わらないので、Bayer の色は元の位置と同じになる。
inline int reflect(int x, int n) {
    if (n == 1) return 0;
    while (x < 0 || x >= n) {
        if (x < 0) x = -x;
        if (x >= n) x = 2 * (n - 1) - x;
    }
    return x;
}

// ---- RCD（Bayer） --------------------------------------------------------------------------

const int kTile = 256;   // 出力するタイルの大きさ
const int kMargin = 16;  // まわりの余白（RCD の計算が届く半径 9 より大きく）
const int kSize = kTile + 2 * kMargin;

// HDR の値は 2^-24 程度まで小さくなるので、0 で割らないための微小量はそれより十分小さくする。
const float kEps = 1e-30f;
const float kEpsSq = 1e-36f;

void rcd_tile(const float* cfa_img, int W, int H, const CfaPattern& pat, int x0, int y0, std::vector<float>& out,
              std::vector<float>& buf, const NoiseModel& noise, float black, const float* gain, int block, int grid_w) {
    const int S = kSize, N = S * S;
    buf.assign(static_cast<std::size_t>(N) * 12, 0.0f);
    float* cfa = buf.data();
    float* rgb[3] = {cfa + N, cfa + 2 * N, cfa + 3 * N};
    float* VH = cfa + 4 * N;
    float* PQ = cfa + 5 * N;
    float* lpf = cfa + 6 * N;
    float* hpfV = cfa + 7 * N;
    float* hpfH = cfa + 8 * N;
    float* hpfP = cfa + 9 * N;
    float* hpfQ = cfa + 10 * N;
    float* nvar = cfa + 11 * N;  // その画素の値でのノイズの分散
    const int w1 = S, w2 = 2 * S, w3 = 3 * S, w4 = 4 * S;

    // 色（タイルの座標で）。折り返しても偶奇は同じなので、元の座標の色でよい。
    const int gx0 = x0 - kMargin, gy0 = y0 - kMargin;
    const auto fc = [&](int r, int c) { return pat.at(((gx0 + c) % 2 + 2) % 2, ((gy0 + r) % 2 + 2) % 2); };

    for (int r = 0; r < S; ++r) {
        const int gy = reflect(gy0 + r, H);
        for (int c = 0; c < S; ++c) {
            const int gx = reflect(gx0 + c, W);
            // 値は底上げ（black）してあるので通常は正。それでも下回るごくまれな値だけ小さな正の値にする
            // （比を使う計算が負で崩れないように）。
            const float v = std::max(black * 1e-3f + kEps, cfa_img[static_cast<std::size_t>(gy) * W + gx]);
            const int i = r * S + c;
            const int col = fc(r, c);
            cfa[i] = v;
            rgb[col][i] = v;
            if (noise.valid) {
                const float g = gain ? gain[static_cast<std::size_t>(gy / block) * grid_w + gx / block] : 1.0f;
                nvar[i] = static_cast<float>(noise.variance(col, (v - black) / g) * g * g);
            } else {
                nvar[i] = 0.0f;
            }
        }
    }

    // 1.1 縦・横の色差の高域（の二乗）
    for (int r = 3; r < S - 3; ++r) {
        for (int c = 3; c < S - 3; ++c) {
            const int i = r * S + c;
            hpfV[i] = sqr((cfa[i - w3] - cfa[i - w1] - cfa[i + w1] + cfa[i + w3]) - 3.0f * (cfa[i - w2] + cfa[i + w2]) + 6.0f * cfa[i]);
            hpfH[i] = sqr((cfa[i - 3] - cfa[i - 1] - cfa[i + 1] + cfa[i + 3]) - 3.0f * (cfa[i - 2] + cfa[i + 2]) + 6.0f * cfa[i]);
        }
    }
    // 1.2 縦・横の方向の判別
    for (int r = 4; r < S - 4; ++r) {
        for (int c = 4; c < S - 4; ++c) {
            const int i = r * S + c;
            // 高域フィルタ（係数 1,−1,−3,6,−3,−1,1、二乗和 58）3つ分にノイズだけで入る量（174σ²）を両方に足す。
            // 方向の差がノイズに埋もれている所では 0.5（方向を決めない）に近づく。
            const float nz = 174.0f * nvar[i];
            const float v = std::max(kEpsSq, hpfV[i - w1] + hpfV[i] + hpfV[i + w1]) + nz;
            const float h = std::max(kEpsSq, hpfH[i - 1] + hpfH[i] + hpfH[i + 1]) + nz;
            VH[i] = v / (v + h);
        }
    }
    // 2 低域（緑以外の位置）
    for (int r = 2; r < S - 2; ++r) {
        for (int c = 2; c < S - 2; ++c) {
            if (fc(r, c) == 1) continue;
            const int i = r * S + c;
            lpf[i] = cfa[i] + 0.5f * (cfa[i - w1] + cfa[i + w1] + cfa[i - 1] + cfa[i + 1]) +
                     0.25f * (cfa[i - w1 - 1] + cfa[i - w1 + 1] + cfa[i + w1 - 1] + cfa[i + w1 + 1]);
        }
    }
    // 3 赤・青の位置の緑
    for (int r = 4; r < S - 4; ++r) {
        for (int c = 4; c < S - 4; ++c) {
            if (fc(r, c) == 1) continue;
            const int i = r * S + c;
            const float ci = cfa[i];
            const float nG = kEps + std::fabs(cfa[i - w1] - cfa[i + w1]) + std::fabs(ci - cfa[i - w2]) + std::fabs(cfa[i - w1] - cfa[i - w3]) + std::fabs(cfa[i - w2] - cfa[i - w4]);
            const float sG = kEps + std::fabs(cfa[i - w1] - cfa[i + w1]) + std::fabs(ci - cfa[i + w2]) + std::fabs(cfa[i + w1] - cfa[i + w3]) + std::fabs(cfa[i + w2] - cfa[i + w4]);
            const float wG = kEps + std::fabs(cfa[i - 1] - cfa[i + 1]) + std::fabs(ci - cfa[i - 2]) + std::fabs(cfa[i - 1] - cfa[i - 3]) + std::fabs(cfa[i - 2] - cfa[i - 4]);
            const float eG = kEps + std::fabs(cfa[i - 1] - cfa[i + 1]) + std::fabs(ci - cfa[i + 2]) + std::fabs(cfa[i + 1] - cfa[i + 3]) + std::fabs(cfa[i + 2] - cfa[i + 4]);
            const float l = lpf[i];
            const float nE = cfa[i - w1] * (l + l) / (kEps + l + lpf[i - w2]);
            const float sE = cfa[i + w1] * (l + l) / (kEps + l + lpf[i + w2]);
            const float wE = cfa[i - 1] * (l + l) / (kEps + l + lpf[i - 2]);
            const float eE = cfa[i + 1] * (l + l) / (kEps + l + lpf[i + 2]);
            const float vEst = (sG * nE + nG * sE) / (nG + sG);
            const float hEst = (wG * eE + eG * wE) / (eG + wG);
            const float central = VH[i];
            const float neigh = 0.25f * (VH[i - w1 - 1] + VH[i - w1 + 1] + VH[i + w1 - 1] + VH[i + w1 + 1]);
            const float disc = std::fabs(0.5f - central) < std::fabs(0.5f - neigh) ? neigh : central;
            rgb[1][i] = std::max(0.0f, intp(disc, hEst, vEst));
        }
    }
    // 4.1 斜め（P・Q）の色差の高域
    for (int r = 3; r < S - 3; ++r) {
        for (int c = 3; c < S - 3; ++c) {
            const int i = r * S + c;
            hpfP[i] = sqr((cfa[i - w3 - 3] - cfa[i - w1 - 1] - cfa[i + w1 + 1] + cfa[i + w3 + 3]) - 3.0f * (cfa[i - w2 - 2] + cfa[i + w2 + 2]) + 6.0f * cfa[i]);
            hpfQ[i] = sqr((cfa[i - w3 + 3] - cfa[i - w1 + 1] - cfa[i + w1 - 1] + cfa[i + w3 - 3]) - 3.0f * (cfa[i - w2 + 2] + cfa[i + w2 - 2]) + 6.0f * cfa[i]);
        }
    }
    // 4.2 斜めの方向の判別（緑以外の位置）
    for (int r = 4; r < S - 4; ++r) {
        for (int c = 4; c < S - 4; ++c) {
            if (fc(r, c) == 1) continue;
            const int i = r * S + c;
            const float nz = 174.0f * nvar[i];
            const float p = std::max(kEpsSq, hpfP[i - w1 - 1] + hpfP[i] + hpfP[i + w1 + 1]) + nz;
            const float q = std::max(kEpsSq, hpfQ[i - w1 + 1] + hpfQ[i] + hpfQ[i + w1 - 1]) + nz;
            PQ[i] = p / (p + q);
        }
    }
    // 4.3 青の位置の赤・赤の位置の青
    for (int r = 6; r < S - 6; ++r) {
        for (int c = 6; c < S - 6; ++c) {
            const int own = fc(r, c);
            if (own == 1) continue;
            const int k = 2 - own;
            const int i = r * S + c;
            const float central = PQ[i];
            const float neigh = 0.25f * (PQ[i - w1 - 1] + PQ[i - w1 + 1] + PQ[i + w1 - 1] + PQ[i + w1 + 1]);
            const float disc = std::fabs(0.5f - central) < std::fabs(0.5f - neigh) ? neigh : central;
            const float* ch = rgb[k];
            const float* g = rgb[1];
            const float nwG = kEps + std::fabs(ch[i - w1 - 1] - ch[i + w1 + 1]) + std::fabs(ch[i - w1 - 1] - ch[i - w3 - 3]) + std::fabs(g[i] - g[i - w2 - 2]);
            const float neG = kEps + std::fabs(ch[i - w1 + 1] - ch[i + w1 - 1]) + std::fabs(ch[i - w1 + 1] - ch[i - w3 + 3]) + std::fabs(g[i] - g[i - w2 + 2]);
            const float swG = kEps + std::fabs(ch[i - w1 + 1] - ch[i + w1 - 1]) + std::fabs(ch[i + w1 - 1] - ch[i + w3 - 3]) + std::fabs(g[i] - g[i + w2 - 2]);
            const float seG = kEps + std::fabs(ch[i - w1 - 1] - ch[i + w1 + 1]) + std::fabs(ch[i + w1 + 1] - ch[i + w3 + 3]) + std::fabs(g[i] - g[i + w2 + 2]);
            const float nwE = ch[i - w1 - 1] - g[i - w1 - 1];
            const float neE = ch[i - w1 + 1] - g[i - w1 + 1];
            const float swE = ch[i + w1 - 1] - g[i + w1 - 1];
            const float seE = ch[i + w1 + 1] - g[i + w1 + 1];
            const float pEst = (nwG * seE + seG * nwE) / (nwG + seG);
            const float qEst = (neG * swE + swG * neE) / (neG + swG);
            rgb[k][i] = std::max(0.0f, g[i] + intp(disc, qEst, pEst));
        }
    }
    // 4.4 緑の位置の赤・青
    for (int r = 9; r < S - 9; ++r) {
        for (int c = 9; c < S - 9; ++c) {
            if (fc(r, c) != 1) continue;
            const int i = r * S + c;
            const float central = VH[i];
            const float neigh = 0.25f * (VH[i - w1 - 1] + VH[i - w1 + 1] + VH[i + w1 - 1] + VH[i + w1 + 1]);
            const float disc = std::fabs(0.5f - central) < std::fabs(0.5f - neigh) ? neigh : central;
            const float* g = rgb[1];
            const float g0 = g[i];
            const float n1 = kEps + std::fabs(g0 - g[i - w2]);
            const float s1 = kEps + std::fabs(g0 - g[i + w2]);
            const float w1g = kEps + std::fabs(g0 - g[i - 2]);
            const float e1 = kEps + std::fabs(g0 - g[i + 2]);
            for (int k = 0; k <= 2; k += 2) {
                const float* ch = rgb[k];
                const float sn = std::fabs(ch[i - w1] - ch[i + w1]);
                const float ew = std::fabs(ch[i - 1] - ch[i + 1]);
                const float nG = n1 + sn + std::fabs(ch[i - w1] - ch[i - w3]);
                const float sG = s1 + sn + std::fabs(ch[i + w1] - ch[i + w3]);
                const float wG = w1g + ew + std::fabs(ch[i - 1] - ch[i - 3]);
                const float eG = e1 + ew + std::fabs(ch[i + 1] - ch[i + 3]);
                const float nE = ch[i - w1] - g[i - w1];
                const float sE = ch[i + w1] - g[i + w1];
                const float wE = ch[i - 1] - g[i - 1];
                const float eE = ch[i + 1] - g[i + 1];
                const float vEst = (nG * sE + sG * nE) / (nG + sG);
                const float hEst = (eG * wE + wG * eE) / (eG + wG);
                rgb[k][i] = std::max(0.0f, g0 + intp(disc, hEst, vEst));
            }
        }
    }

    // 内側を書き出す。
    for (int r = kMargin; r < kMargin + kTile; ++r) {
        const int y = y0 + r - kMargin;
        if (y >= H) break;
        for (int c = kMargin; c < kMargin + kTile; ++c) {
            const int x = x0 + c - kMargin;
            if (x >= W) break;
            const int i = r * S + c;
            float* d = out.data() + (static_cast<std::size_t>(y) * W + x) * 3;
            d[0] = rgb[0][i];
            d[1] = rgb[1][i];
            d[2] = rgb[2][i];
        }
    }
}

// ---- X-Trans（簡単な補間） ------------------------------------------------------------------

void simple_demosaic(const float* cfa, int W, int H, const CfaPattern& pat, std::vector<float>& out) {
    parallel_for(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < W; ++x) {
                const int own = pat.at(x, y);
                float* d = out.data() + (static_cast<std::size_t>(y) * W + x) * 3;
                for (int c = 0; c < 3; ++c) {
                    if (c == own) {
                        d[c] = cfa[static_cast<std::size_t>(y) * W + x];
                        continue;
                    }
                    double sum = 0.0;
                    int n = 0;
                    for (int radius = 1; radius <= 2 && n == 0; ++radius) {
                        for (int dy = -radius; dy <= radius; ++dy) {
                            const int yy = y + dy;
                            if (yy < 0 || yy >= H) continue;
                            for (int dx = -radius; dx <= radius; ++dx) {
                                const int xx = x + dx;
                                if (xx < 0 || xx >= W || pat.at(xx, yy) != c) continue;
                                sum += cfa[static_cast<std::size_t>(yy) * W + xx];
                                ++n;
                            }
                        }
                    }
                    d[c] = n ? static_cast<float>(sum / n) : 0.0f;
                }
            }
        }
    });
}

}  // namespace

std::vector<float> demosaic(const float* cfa, int W, int H, const CfaPattern& pat, const NoiseModel& noise, float black,
                            const float* gain, int block, int grid_w) {
    std::vector<float> out(static_cast<std::size_t>(W) * H * 3, 0.0f);
    if (!pat.is_bayer()) {
        simple_demosaic(cfa, W, H, pat, out);
        return out;
    }
    const int tx = (W + kTile - 1) / kTile, ty = (H + kTile - 1) / kTile;
    parallel_for(tx * ty, [&](int i0, int i1) {
        std::vector<float> buf;
        for (int i = i0; i < i1; ++i) rcd_tile(cfa, W, H, pat, (i % tx) * kTile, (i / tx) * kTile, out, buf, noise, black, gain, block, grid_w);
    }, 1);
    return out;
}

}  // namespace hdr
