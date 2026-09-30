// rawhdr: RAWブラケットのHDR合成（コマンドライン版）。
//
//   rawhdr info  RAW...                    各RAWの情報を表示する
//   rawhdr merge [オプション] RAW...        合成して DNG を書き出す
//
// merge のオプション:
//   -o, --output PATH     書き出す DNG（省略時は基準フレームの隣に「<名前>_HDR.dng」）
//   --ref N               基準フレーム（入力の順で 1 から数える。省略時は露光量が中央のもの）
//   --ramp X              明るいフレームの重みを下げ始める明るさ（飽和の閾値に対する比、既定 0.55）
//   --feather PX          重みのちらつきを抑えるぼかしの幅（画素、既定 16）
//   --safety X            飽和とみなす閾値（飽和レベルに対する比、既定 0.92）
//   --no-compress         DNG を圧縮しない
//   --lens-xmp            XMP でレンズプロファイル補正を有効にする（ユーザーの既定の現像設定は使われなくなる）
//   --baseline EV         機種の BaselineExposure（Adobe の値が分かるとき。既定 0）
//   --debug DIR           確認用の画像（由来マップ・プレビュー）を DIR に書く

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "hdrcore/dng_writer.hpp"
#include "hdrcore/exposure.hpp"
#include "hdrcore/merge.hpp"
#include "hdrcore/preview.hpp"
#include "hdrcore/raw_frame.hpp"

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void usage() {
    std::fprintf(stderr,
                 "使い方:\n"
                 "  rawhdr info  RAW...\n"
                 "  rawhdr merge [-o OUT.dng] [--ref N] [--ramp X] [--feather PX] [--safety X] [--no-compress]\n"
                 "               [--lens-xmp] [--baseline EV] [--debug DIR] RAW...\n");
}

std::string shutter_text(double t) {
    char buf[32];
    if (t > 0.0 && t < 0.3) {
        std::snprintf(buf, sizeof(buf), "1/%.0f", 1.0 / t);
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f", t);
    }
    return buf;
}

int cmd_info(const std::vector<std::string>& files) {
    int rc = 0;
    for (const std::string& path : files) {
        try {
            const hdr::RawFrame f = hdr::load_raw_frame(path, true);
            const hdr::ClipLevels clip = hdr::detect_clip_levels(f);
            std::printf("%s\n", f.file_name.c_str());
            std::printf("  カメラ      : %s（UniqueCameraModel: %s）\n", f.model.c_str(), f.unique_camera_model.c_str());
            std::printf("  レンズ      : %s  %.0fmm\n", f.lens_model.c_str(), f.focal_length);
            std::printf("  露出        : %s秒  F%.1f  ISO%.0f  （名目 %.2f EV）\n", shutter_text(f.exposure_time).c_str(), f.fnumber,
                        f.iso, f.nominal_ev());
            std::printf("  寸法        : %d×%d（既定の切り抜き %d,%d %d×%d）  CFA %d×%d  %dbit\n", f.width, f.height, f.crop_x,
                        f.crop_y, f.crop_w, f.crop_h, f.cfa.w, f.cfa.h, f.bits);
            std::printf("  白（機種表）: %.0f %.0f %.0f\n", f.white[0], f.white[1], f.white[2]);
            std::printf("  飽和（実測）: %.0f%s %.0f%s %.0f%s  （* = 飽和した画素がある）\n", clip.level[0], clip.detected[0] ? "*" : "",
                        clip.level[1], clip.detected[1] ? "*" : "", clip.level[2], clip.detected[2] ? "*" : "");
            std::printf("  メタデータ  : EXIF %zu件  GPS %zu件  メーカーノート %zuバイト\n", f.meta.exif.size(), f.meta.gps.size(),
                        f.meta.makernote.size());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "エラー: %s\n", e.what());
            rc = 1;
        }
    }
    return rc;
}

void write_debug(const std::string& dir, const hdr::MergeResult& m, const std::vector<hdr::RawFrame>& frames) {
    mkdir(dir.c_str(), 0755);
    const hdr::RawFrame& ref = frames[m.reference];
    for (int ev : {-2, 0, 2}) {
        hdr::PreviewOptions o;
        o.max_size = 1600;
        o.exposure_ev = m.reference_ev_offset + ev;
        const hdr::Rgb8Image img = hdr::render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, o);
        hdr::write_png(dir + "/merged_" + (ev > 0 ? "+" : "") + std::to_string(ev) + "EV.png", hdr::apply_orientation(img, ref.orientation));
    }
    // 由来マップ（暗い→明るい の順に 青→水色→緑→黄→橙→赤→桃→白）。
    static const unsigned char pal[8][3] = {{40, 40, 255}, {0, 160, 255}, {0, 220, 120}, {200, 220, 0},
                                            {255, 140, 0}, {255, 40, 40}, {255, 0, 200}, {255, 255, 255}};
    hdr::Rgb8Image sm;
    sm.width = m.grid_w;
    sm.height = m.grid_h;
    sm.rgb.assign(static_cast<std::size_t>(sm.width) * sm.height * 3, 0);
    for (std::size_t i = 0; i < static_cast<std::size_t>(sm.width) * sm.height; ++i) {
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (std::size_t o = 0; o < m.weights.size(); ++o) v += m.weights[o][i] * pal[o % 8][c];
            sm.rgb[i * 3 + c] = static_cast<unsigned char>(std::lround(std::min(255.0, v)));
        }
    }
    hdr::write_png(dir + "/source_map.png", hdr::apply_orientation(sm, ref.orientation));
}

int cmd_merge(int argc, char** argv) {
    std::vector<std::string> files;
    std::string output, debug_dir;
    hdr::MergeOptions mo;
    hdr::DngWriteOptions dopt;
    int ref = -1;
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s の値がありません\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-o" || a == "--output") {
            output = next();
        } else if (a == "--ref") {
            ref = std::atoi(next().c_str()) - 1;
        } else if (a == "--feather") {
            mo.feather_px = std::atoi(next().c_str());
        } else if (a == "--ramp") {
            mo.ramp_start = std::atof(next().c_str());
        } else if (a == "--safety") {
            mo.safety = std::atof(next().c_str());
        } else if (a == "--no-compress") {
            dopt.compress = false;
        } else if (a == "--baseline") {
            dopt.camera_baseline_exposure = std::atof(next().c_str());
        } else if (a == "--lens-xmp") {
            dopt.enable_lens_profile = true;
        } else if (a == "--debug") {
            debug_dir = next();
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "不明なオプション: %s\n", a.c_str());
            return 2;
        } else {
            files.push_back(a);
        }
    }
    if (files.size() < 2) {
        std::fprintf(stderr, "RAW を2枚以上指定してください\n");
        return 2;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<hdr::RawFrame> frames;
    for (const std::string& f : files) frames.push_back(hdr::load_raw_frame(f, true));
    std::printf("読み込み: %d枚 %.1f秒\n", static_cast<int>(frames.size()), seconds_since(t0));

    const auto t1 = std::chrono::steady_clock::now();
    const hdr::ExposurePlan plan = hdr::estimate_exposures(frames);
    std::printf("露出比の推定: %.1f秒\n", seconds_since(t1));
    for (std::size_t o = 0; o < plan.order.size(); ++o) {
        const int i = plan.order[o];
        const hdr::ClipLevels& c = plan.clip[i];
        std::printf("  [%d] %-32s %8s秒  相対 %+6.3f EV  飽和 %.0f/%.0f/%.0f\n", i + 1, frames[i].file_name.c_str(),
                    shutter_text(frames[i].exposure_time).c_str(), std::log2(plan.rel_exposure[o]), c.level[0], c.level[1], c.level[2]);
    }
    for (const hdr::PairFit& f : plan.fits) {
        std::printf("  [%d]→[%d] 比 %.4f（名目 %.4f、差 %+.3f EV）%s\n", f.dark + 1, f.bright + 1, f.ratio, f.nominal_ratio,
                    std::log2(f.ratio / f.nominal_ratio), f.measured ? "" : "  ※実測できず名目値");
    }

    mo.reference = ref;
    const auto t2 = std::chrono::steady_clock::now();
    const hdr::MergeResult m = hdr::merge_frames(frames, plan, mo);
    std::printf("合成: %.1f秒  基準 [%d] %s  BaselineExposure +%.3f EV  最暗でも飽和 %.4f%%\n", seconds_since(t2),
                m.reference + 1, frames[m.reference].file_name.c_str(), m.reference_ev_offset, m.clipped_fraction * 100.0);

    if (output.empty()) {
        const std::string& rp = frames[m.reference].path;
        const std::size_t dot = rp.find_last_of('.');
        output = (dot == std::string::npos ? rp : rp.substr(0, dot)) + "_HDR.dng";
    }
    const auto t3 = std::chrono::steady_clock::now();
    hdr::write_dng(output, m, frames, plan, dopt);
    std::printf("書き出し: %.1f秒  %s\n", seconds_since(t3), output.c_str());
    if (!debug_dir.empty()) write_debug(debug_dir, m, frames);
    std::printf("合計: %.1f秒\n", seconds_since(t0));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string cmd = argv[1];
    try {
        if (cmd == "info") return cmd_info(std::vector<std::string>(argv + 2, argv + argc));
        if (cmd == "merge") return cmd_merge(argc - 2, argv + 2);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "エラー: %s\n", e.what());
        return 1;
    }
    usage();
    return 2;
}
