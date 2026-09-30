#include "hdrcore/dng_writer.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "hdrcore/parallel.hpp"
#include "hdrcore/preview.hpp"
#include "hdrcore/tiff_builder.hpp"

namespace hdr {

namespace {

// ---- タグ番号 ----
enum : uint16_t {
    kNewSubFileType = 254,
    kImageWidth = 256,
    kImageLength = 257,
    kBitsPerSample = 258,
    kCompression = 259,
    kPhotometric = 262,
    kImageDescription = 270,
    kMake = 271,
    kModel = 272,
    kStripOffsets = 273,
    kOrientation = 274,
    kSamplesPerPixel = 277,
    kRowsPerStrip = 278,
    kStripByteCounts = 279,
    kPlanarConfig = 284,
    kSoftware = 305,
    kDateTime = 306,
    kArtist = 315,
    kPredictor = 317,
    kTileWidth = 322,
    kTileLength = 323,
    kTileOffsets = 324,
    kTileByteCounts = 325,
    kSubIFDs = 330,
    kSampleFormat = 339,
    kXmp = 700,
    kCopyright = 33432,
    kCfaRepeatPatternDim = 33421,
    kCfaPattern = 33422,
    kExifIfd = 34665,
    kGpsIfd = 34853,
    kDngVersion = 50706,
    kDngBackwardVersion = 50707,
    kUniqueCameraModel = 50708,
    kCfaPlaneColor = 50710,
    kCfaLayout = 50711,
    kBlackLevelRepeatDim = 50713,
    kBlackLevel = 50714,
    kWhiteLevel = 50717,
    kDefaultScale = 50718,
    kDefaultCropOrigin = 50719,
    kDefaultCropSize = 50720,
    kColorMatrix1 = 50721,
    kAsShotNeutral = 50728,
    kBaselineExposure = 50730,
    kBaselineNoise = 50731,
    kBaselineSharpness = 50732,
    kLinearResponseLimit = 50734,
    kCameraSerialNumber = 50735,
    kLensInfo = 50736,
    kDngPrivateData = 50740,
    kCalibrationIlluminant1 = 50778,
    kRawDataUniqueId = 50781,
    kOriginalRawFileName = 50827,
    kPreviewColorSpace = 50970,
};

// 浮動小数点プレディクタ（TIFF Technical Note 3。libtiff の fpDiff と同じ）:
// 1行の float を「最上位バイトの並び、次のバイトの並び…」に並べ替えてから、バイト単位で差分を取る。
void fp_predict_row(const float* src, int n, uint8_t* out, std::vector<uint8_t>& tmp) {
    tmp.resize(static_cast<std::size_t>(n) * 4);
    std::memcpy(tmp.data(), src, tmp.size());
    for (int i = 0; i < n; ++i) {
        for (int b = 0; b < 4; ++b) {
            // リトルエンディアンの float の b バイト目は、最上位から数えて (3 − b) 番目。
            out[static_cast<std::size_t>(3 - b) * n + i] = tmp[static_cast<std::size_t>(4) * i + b];
        }
    }
    for (std::size_t i = static_cast<std::size_t>(n) * 4 - 1; i >= 1; --i) out[i] = static_cast<uint8_t>(out[i] - out[i - 1]);
}

std::vector<std::vector<uint8_t>> encode_tiles(const MergeResult& m, int tile, bool compress) {
    const int tx = (m.width + tile - 1) / tile, ty = (m.height + tile - 1) / tile;
    std::vector<std::vector<uint8_t>> blocks(static_cast<std::size_t>(tx) * ty);
    bool failed = false;
    parallel_for(static_cast<int>(blocks.size()), [&](int i0, int i1) {
        std::vector<float> row(tile);
        std::vector<uint8_t> raw(static_cast<std::size_t>(tile) * tile * 4), tmp;
        for (int i = i0; i < i1; ++i) {
            const int x0 = (i % tx) * tile, y0 = (i / tx) * tile;
            for (int y = 0; y < tile; ++y) {
                // タイルが画像の端をはみ出す所は、端の画素を延ばして埋める。
                const int sy = std::min(m.height - 1, y0 + y);
                const float* src = m.data.data() + static_cast<std::size_t>(sy) * m.width;
                for (int x = 0; x < tile; ++x) row[x] = src[std::min(m.width - 1, x0 + x)];
                uint8_t* dst = raw.data() + static_cast<std::size_t>(y) * tile * 4;
                if (compress) {
                    fp_predict_row(row.data(), tile, dst, tmp);
                } else {
                    std::memcpy(dst, row.data(), static_cast<std::size_t>(tile) * 4);
                }
            }
            if (!compress) {
                blocks[i] = raw;
                continue;
            }
            uLongf len = compressBound(static_cast<uLong>(raw.size()));
            std::vector<uint8_t> z(len);
            if (compress2(z.data(), &len, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
                failed = true;
                continue;
            }
            z.resize(len);
            blocks[i] = std::move(z);
        }
    });
    if (failed) throw std::runtime_error("画素の圧縮に失敗しました");
    return blocks;
}

std::vector<uint8_t> rgb_bytes(const Rgb8Image& img) { return img.rgb; }

void set_rgb8_image(TiffIfd& ifd, const Rgb8Image& img, uint32_t subfile_type) {
    ifd.set_long(kNewSubFileType, subfile_type);
    ifd.set_long(kImageWidth, static_cast<uint32_t>(img.width));
    ifd.set_long(kImageLength, static_cast<uint32_t>(img.height));
    ifd.set_short(kBitsPerSample, std::vector<uint16_t>{8, 8, 8});
    ifd.set_short(kCompression, 1);
    ifd.set_short(kPhotometric, 2);
    ifd.set_short(kSamplesPerPixel, 3);
    ifd.set_long(kRowsPerStrip, static_cast<uint32_t>(img.height));
    ifd.set_short(kPlanarConfig, 1);
    ifd.set_image_blocks(kStripOffsets, kStripByteCounts, {rgb_bytes(img)});
}

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c; break;
        }
    }
    return o;
}

std::string format_double(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
    return buf;
}

std::string make_xmp(const MergeResult& m, const std::vector<RawFrame>& frames, const ExposurePlan& plan,
                     const DngWriteOptions& opt) {
    std::string sources, ratios;
    for (std::size_t o = 0; o < plan.order.size(); ++o) {
        if (o) {
            sources += ";";
            ratios += ";";
        }
        sources += frames[plan.order[o]].file_name;
        ratios += format_double(std::log2(plan.rel_exposure[o]), 4);
    }
    std::string x;
    x += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
    x += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n";
    x += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
    x += "  <rdf:Description rdf:about=\"\"\n";
    x += "    xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\"\n";
    if (opt.enable_lens_profile) x += "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n";
    x += "    xmlns:rbh=\"https://github.com/Geology-cat/RawBracketHDR/ns/1.0/\"\n";
    x += "   xmp:CreatorTool=\"" + xml_escape(opt.software) + "\"\n";
    if (opt.enable_lens_profile) {
        x += "   crs:LensProfileEnable=\"1\"\n";
        x += "   crs:LensProfileSetup=\"LensDefaults\"\n";
    }
    x += "   rbh:ReferenceFile=\"" + xml_escape(frames[m.reference].file_name) + "\"\n";
    x += "   rbh:SourceFiles=\"" + xml_escape(sources) + "\"\n";
    x += "   rbh:RelativeExposureEV=\"" + ratios + "\"/>\n";
    x += " </rdf:RDF>\n";
    x += "</x:xmpmeta>\n";
    x += "<?xpacket end=\"w\"?>";
    return x;
}

// 出力データから 16 バイトの識別子を作る（Lightroom のキャッシュの区別に使われる）。
std::vector<uint8_t> unique_id(const std::vector<float>& data) {
    uint64_t h1 = 1469598103934665603ull, h2 = 0x9E3779B97F4A7C15ull;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data.data());
    const std::size_t n = data.size() * sizeof(float);
    for (std::size_t i = 0; i < n; i += 7) {
        h1 = (h1 ^ p[i]) * 1099511628211ull;
        h2 = (h2 ^ p[n - 1 - i]) * 0xBF58476D1CE4E5B9ull + i;
    }
    std::vector<uint8_t> id(16);
    for (int i = 0; i < 8; ++i) {
        id[i] = static_cast<uint8_t>(h1 >> (8 * i));
        id[8 + i] = static_cast<uint8_t>(h2 >> (8 * i));
    }
    return id;
}

}  // namespace

void write_dng(const std::string& path, const MergeResult& m, const std::vector<RawFrame>& frames,
               const ExposurePlan& plan, const DngWriteOptions& opt) {
    if (m.reference < 0 || m.reference >= static_cast<int>(frames.size())) throw std::runtime_error("基準フレームがありません");
    const RawFrame& ref = frames[m.reference];
    const SourceMetadata& meta = ref.meta;

    TiffWriter writer;
    TiffIfd& ifd0 = writer.root();

    // ---- 確認用の画像（IFD0 のサムネイルと、SubIFD のプレビュー） ----
    PreviewOptions po;
    po.exposure_ev = m.reference_ev_offset;
    po.max_size = 256;
    const Rgb8Image thumb = render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, po);
    set_rgb8_image(ifd0, thumb, 1);

    // ---- 元のファイルから引き継ぐ IFD0 のタグ ----
    for (const TiffEntry& e : meta.ifd0) {
        if (e.tag == 0xC614 || e.tag == kOrientation) continue;
        ifd0.set_raw(e.tag, e.type, e.count, to_little_endian(e.data, e.type, meta.little_endian));
    }
    if (!ifd0.has(kMake) && !ref.make.empty()) ifd0.set_ascii(kMake, ref.make);
    if (!ifd0.has(kModel) && !ref.model.empty()) ifd0.set_ascii(kModel, ref.model);
    ifd0.set_short(kOrientation, static_cast<uint16_t>(ref.orientation));
    ifd0.set_ascii(kSoftware, opt.software);

    // ---- DNG のタグ ----
    ifd0.set_bytes(kDngVersion, {1, 4, 0, 0});
    ifd0.set_bytes(kDngBackwardVersion, {1, 4, 0, 0});
    ifd0.set_ascii(kUniqueCameraModel, ref.unique_camera_model);
    if (ref.has_color_matrix) {
        std::vector<double> cm;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) cm.push_back(ref.color_matrix[i][j]);
        }
        ifd0.set_srational(kColorMatrix1, cm);
        ifd0.set_short(kCalibrationIlluminant1, 21);  // D65
    }
    ifd0.set_rational(kAsShotNeutral, {ref.as_shot_neutral[0], ref.as_shot_neutral[1], ref.as_shot_neutral[2]});
    ifd0.set_srational(kBaselineExposure, {opt.camera_baseline_exposure + m.reference_ev_offset});
    ifd0.set_rational(kBaselineNoise, {1.0});
    ifd0.set_rational(kBaselineSharpness, {1.0});
    ifd0.set_rational(kLinearResponseLimit, {1.0});
    if (!ref.camera_serial.empty()) ifd0.set_ascii(kCameraSerialNumber, ref.camera_serial);
    if (ref.lens_info[0] > 0.0) {
        ifd0.set_rational(kLensInfo, {ref.lens_info[0], ref.lens_info[1], ref.lens_info[2], ref.lens_info[3]});
    }
    ifd0.set_ascii(kOriginalRawFileName, ref.file_name);
    ifd0.set_bytes(kRawDataUniqueId, unique_id(m.data));
    ifd0.set_long(kPreviewColorSpace, 2);  // sRGB

    // メーカーノート: DNG の決まり（"Adobe\0" + "MakN" + 長さ + バイト順 + 元のオフセット + 本体）。
    if (!meta.makernote.empty()) {
        std::vector<uint8_t> p = {'A', 'd', 'o', 'b', 'e', 0, 'M', 'a', 'k', 'N'};
        put_u32_be(p, static_cast<uint32_t>(meta.makernote.size() + 6));
        p.push_back(meta.makernote_little_endian ? 'I' : 'M');
        p.push_back(meta.makernote_little_endian ? 'I' : 'M');
        put_u32_be(p, meta.makernote_offset);
        p.insert(p.end(), meta.makernote.begin(), meta.makernote.end());
        ifd0.set_bytes(kDngPrivateData, p);
    }

    const std::string xmp = make_xmp(m, frames, plan, opt);
    ifd0.set_bytes(kXmp, std::vector<uint8_t>(xmp.begin(), xmp.end()));

    // ---- EXIF・GPS ----
    if (!meta.exif.empty()) {
        TiffIfd* exif = ifd0.add_child(kExifIfd);
        for (const TiffEntry& e : meta.exif) exif->set_raw(e.tag, e.type, e.count, to_little_endian(e.data, e.type, meta.little_endian));
    }
    if (!meta.gps.empty()) {
        TiffIfd* gps = ifd0.add_child(kGpsIfd);
        for (const TiffEntry& e : meta.gps) gps->set_raw(e.tag, e.type, e.count, to_little_endian(e.data, e.type, meta.little_endian));
    }

    // ---- RAW 本体（SubIFD 0） ----
    TiffIfd* raw = ifd0.add_child(kSubIFDs);
    raw->set_long(kNewSubFileType, 0);
    raw->set_long(kImageWidth, static_cast<uint32_t>(m.width));
    raw->set_long(kImageLength, static_cast<uint32_t>(m.height));
    raw->set_short(kBitsPerSample, 32);
    raw->set_short(kSampleFormat, 3);  // IEEE 浮動小数点
    raw->set_short(kCompression, opt.compress ? 8 : 1);
    if (opt.compress) raw->set_short(kPredictor, 3);
    raw->set_short(kPhotometric, 32803);  // CFA
    raw->set_short(kSamplesPerPixel, 1);
    raw->set_short(kPlanarConfig, 1);
    raw->set_long(kTileWidth, static_cast<uint32_t>(opt.tile_size));
    raw->set_long(kTileLength, static_cast<uint32_t>(opt.tile_size));
    raw->set_image_blocks(kTileOffsets, kTileByteCounts, encode_tiles(m, opt.tile_size, opt.compress));
    raw->set_short(kCfaRepeatPatternDim, std::vector<uint16_t>{static_cast<uint16_t>(m.cfa.h), static_cast<uint16_t>(m.cfa.w)});
    std::vector<uint8_t> pattern;
    for (int y = 0; y < m.cfa.h; ++y) {
        for (int x = 0; x < m.cfa.w; ++x) pattern.push_back(m.cfa.color[y][x]);
    }
    raw->set_bytes(kCfaPattern, pattern);
    raw->set_bytes(kCfaPlaneColor, {0, 1, 2});
    raw->set_short(kCfaLayout, 1);
    raw->set_short(kBlackLevelRepeatDim, std::vector<uint16_t>{1, 1});
    raw->set_long(kBlackLevel, 0);
    raw->set_long(kWhiteLevel, 1);
    raw->set_rational(kDefaultScale, {1.0, 1.0});
    raw->set_long(kDefaultCropOrigin, std::vector<uint32_t>{static_cast<uint32_t>(ref.crop_x), static_cast<uint32_t>(ref.crop_y)});
    raw->set_long(kDefaultCropSize, std::vector<uint32_t>{static_cast<uint32_t>(ref.crop_w), static_cast<uint32_t>(ref.crop_h)});

    // ---- プレビュー（SubIFD 1） ----
    if (opt.embed_preview) {
        po.max_size = 1024;
        const Rgb8Image preview = render_preview(m.data.data(), m.width, m.height, m.cfa, ref.color_matrix, ref.as_shot_neutral, po);
        TiffIfd* pv = ifd0.add_child(kSubIFDs);
        set_rgb8_image(*pv, preview, 1);
    }

    writer.write(path);
}

}  // namespace hdr
