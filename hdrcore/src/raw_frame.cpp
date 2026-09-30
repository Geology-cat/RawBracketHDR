#include "hdrcore/raw_frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "hdrcore/parallel.hpp"
#include "libraw/libraw.h"

namespace hdr {

const TiffEntry* SourceMetadata::find_exif(uint16_t tag) const {
    for (const TiffEntry& e : exif) {
        if (e.tag == tag) return &e;
    }
    return nullptr;
}

const TiffEntry* SourceMetadata::find_ifd0(uint16_t tag) const {
    for (const TiffEntry& e : ifd0) {
        if (e.tag == tag) return &e;
    }
    return nullptr;
}

double RawFrame::nominal_ev() const {
    if (!(exposure_time > 0.0)) return 0.0;
    double ev = std::log2(exposure_time);
    if (fnumber > 0.0) ev -= 2.0 * std::log2(fnumber);
    if (iso > 0.0) ev += std::log2(iso / 100.0);
    return ev;
}

namespace {

[[noreturn]] void fail(const std::string& path, const std::string& what) {
    const std::size_t slash = path.find_last_of('/');
    throw std::runtime_error((slash == std::string::npos ? path : path.substr(slash + 1)) + ": " + what);
}

int tiff_type_size(int type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;  // BYTE ASCII SBYTE UNDEFINED
        case 3: case 8: return 2;                  // SHORT SSHORT
        case 4: case 9: case 11: case 13: return 4;  // LONG SLONG FLOAT IFD
        case 5: case 10: case 12: return 8;        // RATIONAL SRATIONAL DOUBLE
        default: return 0;
    }
}

// ---- EXIF・メーカーノートを LibRaw の解析中に拾う ------------------------------------
//
// LibRaw は TIFF/EXIF のエントリを読むたびにコールバックを呼ぶ。そのとき読み取り位置は
// 値の先頭にある（4バイトを超える値はオフセット先へ移動済み）。タグ番号の上位ビットで
// どの IFD かが分かる: 0 = EXIF、(ifd+1)<<20 = TIFF の IFD、0x50000 = GPS、
// 0x70000|0x927c = CR3 のメーカーノート（CMT3）。
struct Capture {
    SourceMetadata* meta = nullptr;
    bool order_known = false;
};

bool has_tag(const std::vector<TiffEntry>& v, uint16_t tag) {
    for (const TiffEntry& e : v) {
        if (e.tag == tag) return true;
    }
    return false;
}

void read_bytes(void* ifp, std::vector<uint8_t>& out, std::size_t n) {
    out.resize(n);
    if (n == 0) return;
    const int got = static_cast<LibRaw_abstract_datastream*>(ifp)->read(out.data(), 1, n);
    if (got < 0 || static_cast<std::size_t>(got) != n) out.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
}

void exif_callback(void* context, int tag, int type, int len, unsigned int ord, void* ifp, INT64 base) {
    Capture* cap = static_cast<Capture*>(context);
    SourceMetadata& m = *cap->meta;
    const int group = (tag >> 16) & 0xFFFF;
    const uint16_t t = static_cast<uint16_t>(tag & 0xFFFF);
    const bool le = ord == 0x4949;
    const int tsize = tiff_type_size(type);
    if (len < 0) return;

    // メーカーノート（EXIF の 0x927C、または CR3 の CMT3）。
    if (t == 0x927C && (group == 0 || group == 7)) {
        if (!m.makernote.empty() || len <= 0 || len > (64 << 20)) return;
        LibRaw_abstract_datastream* s = static_cast<LibRaw_abstract_datastream*>(ifp);
        const INT64 pos = s->tell();
        read_bytes(ifp, m.makernote, static_cast<std::size_t>(len) * static_cast<std::size_t>(std::max(1, tsize)));
        m.makernote_little_endian = le;
        m.makernote_offset = static_cast<uint32_t>(std::max<INT64>(0, pos - base));
        return;
    }
    if (tsize == 0) return;
    const std::size_t bytes = static_cast<std::size_t>(len) * static_cast<std::size_t>(tsize);
    if (bytes > (1u << 20)) return;  // 1MB を超える EXIF の値は持たない（画像の埋め込みなど）

    std::vector<TiffEntry>* dst = nullptr;
    if (group == 0) {
        // サブIFDへのポインタは引き継がない（相対オフセットが壊れるため）。
        if (t == 0x8769 || t == 0x8825 || t == 0xA005 || t == 0x014A) return;
        dst = &m.exif;
    } else if (group == 0x10) {
        // TIFF の IFD0。引き継ぐのは画像の中身に関係しない、一覧にあるタグだけ。
        static const uint16_t kIfd0[] = {0x010E, 0x010F, 0x0110, 0x0112, 0x0132, 0x013B, 0x8298, 0xC614};
        if (std::find(std::begin(kIfd0), std::end(kIfd0), t) == std::end(kIfd0)) return;
        dst = &m.ifd0;
    } else if (group == 5) {
        dst = &m.gps;
    } else {
        return;
    }
    if (has_tag(*dst, t)) return;  // 最初に現れたものを使う
    if (!cap->order_known) {
        m.little_endian = le;
        cap->order_known = true;
    } else if (m.little_endian != le) {
        return;  // バイト順が途中で変わる（別のTIFFが入れ子になっている）ものは使わない
    }
    TiffEntry e;
    e.tag = t;
    e.type = static_cast<uint16_t>(type);
    e.count = static_cast<uint32_t>(len);
    read_bytes(ifp, e.data, bytes);
    if (e.data.size() != bytes) return;
    dst->push_back(std::move(e));
}

std::string ascii_of(const TiffEntry* e) {
    if (!e || e->type != 2) return "";
    std::string s(reinterpret_cast<const char*>(e->data.data()), e->data.size());
    const std::size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

uint32_t u32_of(const uint8_t* p, bool le) {
    return le ? (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24)
              : (uint32_t(p[3]) | uint32_t(p[2]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[0]) << 24);
}

uint16_t u16_of(const uint8_t* p, bool le) {
    return le ? static_cast<uint16_t>(p[0] | p[1] << 8) : static_cast<uint16_t>(p[1] | p[0] << 8);
}

std::string trimmed(const char* s) {
    std::string r(s);
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

// Adobe の UniqueCameraModel の命名（多くは「メーカー 機種名」）に寄せる。
// 入力が DNG ならその値をそのまま使う。
std::string unique_model_of(const LibRaw& r, const SourceMetadata& m) {
    const std::string from_dng = ascii_of(m.find_ifd0(0xC614));
    if (!from_dng.empty()) return from_dng;
    const libraw_iparams_t& id = r.imgdata.idata;
    std::string make = trimmed(id.normalized_make[0] ? id.normalized_make : id.make);
    std::string model = trimmed(id.normalized_model[0] ? id.normalized_model : id.model);
    if (model.compare(0, make.size(), make) == 0) return model;
    return make + " " + model;
}

}  // namespace

RawFrame load_raw_frame(const std::string& path, bool load_pixels) {
    RawFrame f;
    f.path = path;
    const std::size_t slash = path.find_last_of('/');
    f.file_name = slash == std::string::npos ? path : path.substr(slash + 1);

    std::unique_ptr<LibRaw> raw(new LibRaw(0));
    Capture cap;
    cap.meta = &f.meta;
    raw->set_exifparser_handler(exif_callback, &cap);
    int rc = raw->open_file(path.c_str());
    if (rc != LIBRAW_SUCCESS) fail(path, std::string("読めませんでした（") + libraw_strerror(rc) + "）");

    LibRaw& r = *raw;
    const libraw_iparams_t& id = r.imgdata.idata;
    const libraw_image_sizes_t& S = r.imgdata.sizes;
    const libraw_colordata_t& C = r.imgdata.color;

    if (r.imgdata.rawdata.ioparams.fuji_width) fail(path, "斜め配列（SuperCCD）のセンサーには対応していません");
    if (id.is_foveon) fail(path, "Foveon のセンサーにはまだ対応していません");
    if (id.filters == 0) fail(path, "色補間済み（LinearRaw）のRAWにはまだ対応していません");
    if (id.filters > 1000 && id.colors != 3) fail(path, "RGB以外のカラーフィルター（CMYGなど）には対応していません");
    if (id.filters != 9 && id.filters <= 1000) fail(path, "このカラーフィルターの並びには対応していません");

    // ---- メタデータ ----
    f.make = trimmed(id.make);
    f.model = trimmed(id.model);
    f.unique_camera_model = unique_model_of(r, f.meta);
    f.exposure_time = r.imgdata.other.shutter;
    f.fnumber = r.imgdata.other.aperture;
    f.iso = r.imgdata.other.iso_speed;
    f.focal_length = r.imgdata.other.focal_len;
    f.lens_model = trimmed(r.imgdata.lens.Lens);
    f.lens_info[0] = r.imgdata.lens.MinFocal;
    f.lens_info[1] = r.imgdata.lens.MaxFocal;
    f.lens_info[2] = r.imgdata.lens.MaxAp4MinFocal;
    f.lens_info[3] = r.imgdata.lens.MaxAp4MaxFocal;
    f.camera_serial = ascii_of(f.meta.find_exif(0xA431));
    if (f.camera_serial.empty()) f.camera_serial = trimmed(r.imgdata.shootinginfo.BodySerial);
    f.datetime_original = ascii_of(f.meta.find_exif(0x9003));

    // EXIF の LensSpecification があれば LensInfo はそちらを優先する。
    if (const TiffEntry* spec = f.meta.find_exif(0xA432)) {
        if (spec->type == 5 && spec->count == 4 && spec->data.size() == 32) {
            for (int i = 0; i < 4; ++i) {
                const uint32_t num = u32_of(spec->data.data() + 8 * i, f.meta.little_endian);
                const uint32_t den = u32_of(spec->data.data() + 8 * i + 4, f.meta.little_endian);
                if (den != 0 && num != 0) f.lens_info[i] = static_cast<double>(num) / den;
            }
        }
    }

    // 色: LibRaw の cam_xyz は Adobe の ColorMatrix（D65, XYZ→カメラ）と同じもの。
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            f.color_matrix[i][j] = C.cam_xyz[i][j];
            sum += std::fabs(C.cam_xyz[i][j]);
        }
    }
    f.has_color_matrix = sum > 0.0;
    if (C.cam_mul[0] > 0.0f && C.cam_mul[1] > 0.0f && C.cam_mul[2] > 0.0f) {
        for (int c = 0; c < 3; ++c) f.as_shot_neutral[c] = C.cam_mul[1] / C.cam_mul[c];
    } else if (C.pre_mul[0] > 0.0f && C.pre_mul[1] > 0.0f && C.pre_mul[2] > 0.0f) {
        for (int c = 0; c < 3; ++c) f.as_shot_neutral[c] = C.pre_mul[1] / C.pre_mul[c];
    }

    // 向き: EXIF の Orientation があればそれ、無ければ LibRaw の flip から。
    if (const TiffEntry* o = f.meta.find_ifd0(0x0112)) {
        if (o->type == 3 && o->data.size() >= 2) f.orientation = u16_of(o->data.data(), f.meta.little_endian);
    } else {
        switch (S.flip) {
            case 3: f.orientation = 3; break;
            case 5: f.orientation = 8; break;
            case 6: f.orientation = 6; break;
            default: f.orientation = 1; break;
        }
    }
    if (f.orientation < 1 || f.orientation > 8) f.orientation = 1;

    // ---- 寸法と CFA ----
    f.width = S.width;
    f.height = S.height;
    if (f.width <= 0 || f.height <= 0) fail(path, "画像の寸法がありません");
    if (static_cast<double>(f.width) * f.height > 200.0e6) fail(path, "画像が大きすぎます");
    if (id.filters == 9) {
        f.cfa.w = f.cfa.h = 6;
    } else {
        f.cfa.w = f.cfa.h = 2;
        // Bayer の filters は 8行×2列の周期まで表せる。2×2 で繰り返していることを確かめる。
        const auto rgb = [&](int y, int x) {
            const int c = r.COLOR(y, x);
            return c == 3 ? 1 : c;
        };
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 2; ++x) {
                if (rgb(y, x) != rgb(y % 2, x)) fail(path, "2×2 で繰り返さない Bayer の並びには対応していません");
            }
        }
    }
    for (int y = 0; y < f.cfa.h; ++y) {
        for (int x = 0; x < f.cfa.w; ++x) {
            const int c = r.COLOR(y, x);
            f.cfa.color[y][x] = static_cast<uint8_t>(c == 3 ? 1 : c);
        }
    }

    // 機種の既定の切り抜き（CR2 の SensorInfo・DNG の DefaultCrop など）。
    f.crop_x = 0;
    f.crop_y = 0;
    f.crop_w = f.width;
    f.crop_h = f.height;
    {
        const libraw_raw_inset_crop_t& in = S.raw_inset_crops[0];
        const int x = static_cast<int>(in.cleft) - S.left_margin;
        const int y = static_cast<int>(in.ctop) - S.top_margin;
        if (in.cwidth > 0 && in.cheight > 0 && in.cleft != 0xFFFF && in.ctop != 0xFFFF && x >= 0 && y >= 0 &&
            x + in.cwidth <= f.width && y + in.cheight <= f.height) {
            f.crop_x = x;
            f.crop_y = y;
            f.crop_w = in.cwidth;
            f.crop_h = in.cheight;
        }
    }

    if (!load_pixels) return f;

    // ---- 画素 ----
    rc = r.unpack();
    if (rc != LIBRAW_SUCCESS) fail(path, std::string("展開できませんでした（") + libraw_strerror(rc) + "）");
    const unsigned short* src = r.imgdata.rawdata.raw_image;
    if (!src) fail(path, "CFA の生データがありません（この形式にはまだ対応していません）");
    // unpack の後の値を使う（遮光部から黒を測る形式があるため）。
    const libraw_colordata_t& RC = r.imgdata.rawdata.color;
    const libraw_image_sizes_t& RS = r.imgdata.rawdata.sizes;
    const int pitch = static_cast<int>(RS.raw_pitch / 2);
    const int top = RS.top_margin, left = RS.left_margin;
    if (RS.width != f.width || RS.height != f.height) fail(path, "画像の寸法が食い違っています");
    if (top + f.height > RS.raw_height || left + f.width > RS.raw_width) fail(path, "画像の範囲がセンサーの外にはみ出しています");

    // 黒: 全体の black + 色ごとの cblack[0..3] + 周期的な cblack[6..]（見える範囲の座標で）。
    const unsigned pat_h = RC.cblack[4], pat_w = RC.cblack[5];
    const bool has_pat = pat_h > 0 && pat_w > 0 && pat_h * pat_w <= 4096 - 6;
    f.bits = RC.raw_bps > 0 && RC.raw_bps <= 16 ? RC.raw_bps : 0;
    f.data.assign(static_cast<std::size_t>(f.width) * f.height, 0.0f);
    const int W = f.width;
    parallel_for(f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const unsigned short* row = src + static_cast<std::size_t>(y + top) * pitch + left;
            float* dst = f.data.data() + static_cast<std::size_t>(y) * W;
            for (int x = 0; x < W; ++x) {
                const int rawc = r.COLOR(y, x);
                double black = static_cast<double>(RC.black) + RC.cblack[rawc & 3];
                if (has_pat) black += RC.cblack[6 + (y % pat_h) * pat_w + (x % pat_w)];
                dst[x] = static_cast<float>(static_cast<double>(row[x]) - black);
            }
        }
    });

    // 白: 機種表の最大値から黒を引いたもの（色ごと）。実際に飽和している値はデータから別に測る。
    for (int c = 0; c < 3; ++c) {
        double black = static_cast<double>(RC.black) + RC.cblack[c];
        f.white[c] = static_cast<float>(static_cast<double>(RC.maximum) - black);
    }
    r.recycle();
    return f;
}

}  // namespace hdr
