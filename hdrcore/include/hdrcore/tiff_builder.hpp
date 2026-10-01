#pragma once

// TIFF（DNG）ファイルを組み立てる小さな道具。常にリトルエンディアンで書く。
//
// IFD ごとにエントリと画像データ（ストリップまたはタイル）を持たせ、子の IFD（EXIF・GPS・
// SubIFDs）は木にして持つ。書き出すときに子から順に配置し、オフセットを埋める。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdr {

enum TiffType : uint16_t {
    kByte = 1,
    kAscii = 2,
    kShort = 3,
    kLong = 4,
    kRational = 5,
    kSByte = 6,
    kUndefined = 7,
    kSShort = 8,
    kSLong = 9,
    kSRational = 10,
    kFloat = 11,
    kDouble = 12,
};

class TiffIfd {
public:
    // 値はリトルエンディアンのバイト列で渡す。
    void set_raw(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data_le);

    void set_ascii(uint16_t tag, const std::string& s);
    void set_bytes(uint16_t tag, const std::vector<uint8_t>& v, uint16_t type = kByte);
    void set_short(uint16_t tag, const std::vector<uint16_t>& v);
    void set_short(uint16_t tag, uint16_t v) { set_short(tag, std::vector<uint16_t>{v}); }
    void set_long(uint16_t tag, const std::vector<uint32_t>& v);
    void set_long(uint16_t tag, uint32_t v) { set_long(tag, std::vector<uint32_t>{v}); }
    void set_rational(uint16_t tag, const std::vector<double>& v);
    void set_srational(uint16_t tag, const std::vector<double>& v);
    // 分子・分母を指定する有理数（値を丸めずに書きたいとき）。
    void set_rational_exact(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>>& v);
    void set_double(uint16_t tag, const std::vector<double>& v);
    bool has(uint16_t tag) const;
    void remove(uint16_t tag);

    // 子の IFD を指すタグ（ExifIFD 34665・GPS 34853・SubIFDs 330）。SubIFDs は複数持てる。
    TiffIfd* add_child(uint16_t tag);

    // 画像データ（ストリップ／タイル）。offsets_tag と counts_tag を書き出し時に埋める。
    void set_image_blocks(uint16_t offsets_tag, uint16_t counts_tag, std::vector<std::vector<uint8_t>> blocks);

private:
    friend class TiffWriter;
    struct Entry {
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        std::vector<uint8_t> data;
    };
    std::vector<Entry> entries_;
    std::vector<std::pair<uint16_t, std::unique_ptr<TiffIfd>>> children_;
    uint16_t offsets_tag_ = 0, counts_tag_ = 0;
    std::vector<std::vector<uint8_t>> blocks_;
};

class TiffWriter {
public:
    TiffIfd& root() { return root_; }
    // ファイルに書く。失敗したら std::runtime_error。
    void write(const std::string& path);
    // メモリ上に組み立てる。
    std::vector<uint8_t> build();

private:
    uint32_t emit(TiffIfd& ifd, std::vector<uint8_t>& out);
    TiffIfd root_;
};

// バイト列を作る補助（リトルエンディアン）。
void put_u16(std::vector<uint8_t>& v, uint16_t x);
void put_u32(std::vector<uint8_t>& v, uint32_t x);
void put_u32_be(std::vector<uint8_t>& v, uint32_t x);

// 元ファイルのバイト順で書かれた値を、リトルエンディアンに直す（型の大きさごとに反転）。
std::vector<uint8_t> to_little_endian(const std::vector<uint8_t>& data, uint16_t type, bool source_little_endian);

}  // namespace hdr
