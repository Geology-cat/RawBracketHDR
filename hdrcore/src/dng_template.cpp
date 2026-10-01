#include "hdrcore/dng_template.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "hdrcore/tiff_builder.hpp"

extern char** environ;

namespace hdr {

namespace {

// ---- 小さな TIFF の読み手 ----------------------------------------------------------------

struct Ifd {
    std::vector<TiffEntry> entries;  // 値はリトルエンディアンに直してある
    const TiffEntry* find(uint16_t tag) const {
        for (const TiffEntry& e : entries) {
            if (e.tag == tag) return &e;
        }
        return nullptr;
    }
};

int type_size(int type) {
    switch (type) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

class TiffFile {
public:
    explicit TiffFile(std::vector<uint8_t> data) : d_(std::move(data)) {}

    bool parse() {
        if (d_.size() < 8) return false;
        if (d_[0] == 'I' && d_[1] == 'I') {
            le_ = true;
        } else if (d_[0] == 'M' && d_[1] == 'M') {
            le_ = false;
        } else {
            return false;
        }
        if (u16(2) != 42) return false;
        if (!read_ifd(u32(4), ifd0)) return false;
        // SubIFDs（330）の中から RAW 本体（NewSubFileType = 0）を探す。無ければ IFD0 が本体。
        raw = ifd0;
        if (const TiffEntry* sub = ifd0.find(330)) {
            for (uint32_t i = 0; i < sub->count; ++i) {
                Ifd s;
                if (!read_ifd(value_u32(*sub, i), s)) continue;
                const TiffEntry* nst = s.find(254);
                if (nst && value_u32(*nst, 0) == 0) {
                    raw = s;
                    break;
                }
            }
        }
        return true;
    }

    // 数値を取り出す（どの数値型でも）。
    static double value(const TiffEntry& e, uint32_t i) {
        const uint8_t* p = e.data.data();
        const auto rd32 = [](const uint8_t* q) { return uint32_t(q[0]) | uint32_t(q[1]) << 8 | uint32_t(q[2]) << 16 | uint32_t(q[3]) << 24; };
        if (i >= e.count) return 0.0;
        switch (e.type) {
            case 1: return p[i];
            case 3: return static_cast<double>(p[2 * i] | p[2 * i + 1] << 8);
            case 8: return static_cast<double>(static_cast<int16_t>(p[2 * i] | p[2 * i + 1] << 8));
            case 4: case 13: return rd32(p + 4 * i);
            case 9: return static_cast<int32_t>(rd32(p + 4 * i));
            case 5: {
                const uint32_t n = rd32(p + 8 * i), dd = rd32(p + 8 * i + 4);
                return dd ? static_cast<double>(n) / dd : 0.0;
            }
            case 10: {
                const int32_t n = static_cast<int32_t>(rd32(p + 8 * i)), dd = static_cast<int32_t>(rd32(p + 8 * i + 4));
                return dd ? static_cast<double>(n) / dd : 0.0;
            }
            case 11: {
                float f;
                const uint32_t b = rd32(p + 4 * i);
                std::memcpy(&f, &b, 4);
                return f;
            }
            default: return 0.0;
        }
    }
    static uint32_t value_u32(const TiffEntry& e, uint32_t i) { return static_cast<uint32_t>(value(e, i)); }

    Ifd ifd0, raw;

private:
    uint16_t u16(std::size_t o) const {
        if (o + 2 > d_.size()) return 0;
        return le_ ? static_cast<uint16_t>(d_[o] | d_[o + 1] << 8) : static_cast<uint16_t>(d_[o] << 8 | d_[o + 1]);
    }
    uint32_t u32(std::size_t o) const {
        if (o + 4 > d_.size()) return 0;
        return le_ ? (uint32_t(d_[o]) | uint32_t(d_[o + 1]) << 8 | uint32_t(d_[o + 2]) << 16 | uint32_t(d_[o + 3]) << 24)
                   : (uint32_t(d_[o]) << 24 | uint32_t(d_[o + 1]) << 16 | uint32_t(d_[o + 2]) << 8 | uint32_t(d_[o + 3]));
    }
    bool read_ifd(uint32_t off, Ifd& out) {
        if (off == 0 || off + 2 > d_.size()) return false;
        const uint16_t n = u16(off);
        if (off + 2 + 12ull * n > d_.size()) return false;
        for (uint16_t k = 0; k < n; ++k) {
            const std::size_t e = off + 2 + 12ull * k;
            TiffEntry t;
            t.tag = u16(e);
            t.type = u16(e + 2);
            t.count = u32(e + 4);
            const int ts = type_size(t.type);
            if (ts == 0) continue;
            const uint64_t bytes = static_cast<uint64_t>(t.count) * ts;
            if (bytes > (64u << 20)) continue;
            const std::size_t pos = bytes <= 4 ? e + 8 : u32(e + 8);
            if (pos + bytes > d_.size()) continue;
            std::vector<uint8_t> raw(d_.begin() + static_cast<std::ptrdiff_t>(pos), d_.begin() + static_cast<std::ptrdiff_t>(pos + bytes));
            // OpcodeList（51008・51009・51022）は中身が常にビッグエンディアンなので、そのまま持つ。
            const bool opaque = t.tag == 51008 || t.tag == 51009 || t.tag == 51022;
            t.data = opaque ? raw : to_little_endian(raw, t.type, le_);
            out.entries.push_back(std::move(t));
        }
        return true;
    }

    std::vector<uint8_t> d_;
    bool le_ = true;
};

// IFD0 から引き継ぐタグ（色・明るさ・プロファイル）。
const uint16_t kIfd0Tags[] = {
    50708,  // UniqueCameraModel
    50721, 50722,  // ColorMatrix1/2
    50723, 50724,  // CameraCalibration1/2
    50725, 50726,  // ReductionMatrix1/2
    50727,  // AnalogBalance
    50728,  // AsShotNeutral
    50729,  // AsShotWhiteXY
    50731,  // BaselineNoise
    50732,  // BaselineSharpness
    50734,  // LinearResponseLimit
    50739,  // ShadowScale
    50778, 50779,  // CalibrationIlluminant1/2
    50931, 50932,  // CameraCalibrationSignature・ProfileCalibrationSignature
    50936,  // ProfileName
    50937, 50938, 50939,  // ProfileHueSatMapDims・Data1・Data2
    50940,  // ProfileToneCurve
    50941, 50942,  // ProfileEmbedPolicy・ProfileCopyright
    50964, 50965,  // ForwardMatrix1/2
    50981, 50982,  // ProfileLookTableDims・Data
    51107, 51108,  // ProfileHueSatMapEncoding・ProfileLookTableEncoding
    51109,  // BaselineExposureOffset
    51110,  // DefaultBlackRender
};

// OpcodeList2 の中の命令が、値の大きさに依らないもの（GainMap など）だけかどうか。
bool opcodes_scale_free(const std::vector<uint8_t>& d) {
    const auto rd = [&](std::size_t o) -> uint32_t {
        if (o + 4 > d.size()) return 0;
        return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | uint32_t(d[o + 3]);
    };
    const uint32_t n = rd(0);
    std::size_t pos = 4;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t id = rd(pos), size = rd(pos + 12);
        // 1 WarpRectilinear, 2 WarpFisheye, 3 FixVignetteRadial, 9 GainMap, 13 WarpRectilinear2 は値の倍率に依らない。
        if (!(id == 1 || id == 2 || id == 3 || id == 9 || id == 13)) return false;
        pos += 16 + size;
        if (pos > d.size()) return false;
    }
    return true;
}

std::string read_converter_version(const std::string& exe) {
    // .../Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter → .../Contents/Info.plist
    const std::size_t p = exe.rfind("/MacOS/");
    if (p == std::string::npos) return "";
    std::ifstream in(exe.substr(0, p) + "/Info.plist");
    const std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t k = s.find("<key>CFBundleShortVersionString</key>");
    if (k == std::string::npos) return "";
    const std::size_t a = s.find("<string>", k), b = s.find("</string>", k);
    if (a == std::string::npos || b == std::string::npos || b < a) return "";
    return s.substr(a + 8, b - a - 8);
}

}  // namespace

std::string find_dng_converter() {
    std::vector<std::string> candidates = {"/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter"};
    if (const char* home = std::getenv("HOME")) {
        candidates.push_back(std::string(home) + "/Applications/Adobe DNG Converter.app/Contents/MacOS/Adobe DNG Converter");
    }
    for (const std::string& c : candidates) {
        if (access(c.c_str(), X_OK) == 0) return c;
    }
    return "";
}

DngTemplate read_dng_template(const std::string& dng_path) {
    DngTemplate t;
    std::ifstream in(dng_path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    TiffFile f(std::move(bytes));
    if (!f.parse()) {
        t.note = "DNG を読めませんでした";
        return t;
    }
    for (uint16_t tag : kIfd0Tags) {
        if (const TiffEntry* e = f.ifd0.find(tag)) t.ifd0.push_back(*e);
    }
    if (const TiffEntry* be = f.ifd0.find(50730)) t.baseline_exposure = TiffFile::value(*be, 0);

    // 白と黒（RAW の IFD）。黒は繰り返しの平均に、行・列ごとの差分の平均を足す。
    const Ifd& r = f.raw;
    double white = 0.0, black = 0.0;
    if (const TiffEntry* w = r.find(50717)) white = TiffFile::value(*w, 0);
    if (const TiffEntry* b = r.find(50714)) {
        for (uint32_t i = 0; i < b->count; ++i) black += TiffFile::value(*b, i);
        if (b->count) black /= b->count;
    }
    for (uint16_t delta : {uint16_t(50715), uint16_t(50716)}) {
        if (const TiffEntry* d = r.find(delta)) {
            double s = 0.0;
            for (uint32_t i = 0; i < d->count; ++i) s += TiffFile::value(*d, i);
            if (d->count) black += s / d->count;
        }
    }
    if (r.find(50712)) t.note += "LinearizationTable があります（白は線形化した後の値）。";
    t.white_minus_black = white - black;

    // 有効範囲（ActiveArea: 上・左・下・右）。無ければ画像全体。
    int iw = 0, ih = 0;
    if (const TiffEntry* e = r.find(256)) iw = static_cast<int>(TiffFile::value(*e, 0));
    if (const TiffEntry* e = r.find(257)) ih = static_cast<int>(TiffFile::value(*e, 0));
    t.active_width = iw;
    t.active_height = ih;
    if (const TiffEntry* a = r.find(50829)) {
        if (a->count >= 4) {
            t.active_height = static_cast<int>(TiffFile::value(*a, 2) - TiffFile::value(*a, 0));
            t.active_width = static_cast<int>(TiffFile::value(*a, 3) - TiffFile::value(*a, 1));
        }
    }

    // レンズ補正などの命令。OpcodeList3（色補間の後）はそのまま、OpcodeList2 は値の倍率に依らないものだけ。
    // OpcodeList1（線形化の前）は合成で値が変わるので引き継がない。
    if (const TiffEntry* o3 = r.find(51022)) t.raw.push_back(*o3);
    if (const TiffEntry* o2 = r.find(51009)) {
        if (opcodes_scale_free(o2->data)) {
            t.raw.push_back(*o2);
        } else {
            t.note += "OpcodeList2 に値を変える命令があるので引き継ぎません。";
        }
    }
    if (r.find(51008)) t.note += "OpcodeList1 は引き継ぎません。";
    for (uint16_t tag : {uint16_t(50733), uint16_t(50737), uint16_t(50738), uint16_t(50780)}) {
        // BayerGreenSplit・ChromaBlurRadius・AntiAliasStrength・BestQualityScale
        if (const TiffEntry* e = r.find(tag)) t.raw.push_back(*e);
    }
    t.valid = white > 0.0 && t.white_minus_black > 0.0;
    if (!t.valid) t.note += "白レベルが読めませんでした。";
    return t;
}

DngTemplate make_dng_template(const std::string& raw_path, const std::string& converter) {
    DngTemplate t;
    if (converter.empty()) {
        t.note = "Adobe DNG Converter が見つかりません";
        return t;
    }
    char dir_tmpl[] = "/tmp/rbh_dngconv_XXXXXX";
    char* dir = mkdtemp(dir_tmpl);
    if (!dir) {
        t.note = "一時フォルダを作れません";
        return t;
    }
    // -c: 可逆圧縮、-p0: プレビューは小さく。出力は dir/<元の名前>.dng
    std::vector<std::string> args = {converter, "-c", "-p0", "-d", dir, raw_path};
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(&a[0]);
    argv.push_back(nullptr);
    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    const int rc = posix_spawn(&pid, converter.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    int status = 0;
    if (rc == 0) waitpid(pid, &status, 0);

    std::string base = raw_path.substr(raw_path.find_last_of('/') + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    const std::string out = std::string(dir) + "/" + base + ".dng";
    struct stat st;
    if (rc == 0 && stat(out.c_str(), &st) == 0) {
        t = read_dng_template(out);
    } else {
        t.note = "Adobe DNG Converter で変換できませんでした（この版が対応していない機種かもしれません）";
    }
    std::remove(out.c_str());
    rmdir(dir);
    t.converter_version = read_converter_version(converter);
    return t;
}

}  // namespace hdr
