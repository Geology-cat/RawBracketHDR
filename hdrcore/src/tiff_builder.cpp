#include "hdrcore/tiff_builder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>

namespace hdr {

void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

void put_u32_be(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 3; i >= 0; --i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

namespace {

int type_size(uint16_t type) {
    switch (type) {
        case kByte: case kAscii: case kSByte: case kUndefined: return 1;
        case kShort: case kSShort: return 2;
        case kLong: case kSLong: case kFloat: case 13: return 4;
        case kRational: case kSRational: case kDouble: return 8;
        default: return 0;
    }
}

// 有理数への変換。分母は 10^k（k ≤ 6）で、分子が 32bit に収まる範囲で最も細かいもの。
void to_fraction(double v, bool is_signed, int64_t& num, int64_t& den) {
    if (!std::isfinite(v)) v = 0.0;
    const double limit = is_signed ? 2147483647.0 : 4294967295.0;
    den = 1;
    for (int k = 0; k <= 6; ++k) {
        const double d = std::pow(10.0, k);
        if (std::fabs(v) * d > limit) break;
        den = static_cast<int64_t>(d);
        if (std::fabs(v * d - std::round(v * d)) < 1e-9) break;
    }
    num = static_cast<int64_t>(std::llround(v * static_cast<double>(den)));
    if (!is_signed && num < 0) num = 0;
}

}  // namespace

std::vector<uint8_t> to_little_endian(const std::vector<uint8_t>& data, uint16_t type, bool source_le) {
    if (source_le) return data;
    int unit = type_size(type);
    if (type == kRational || type == kSRational) unit = 4;  // 分子・分母それぞれ4バイト
    if (unit <= 1) return data;
    std::vector<uint8_t> out(data);
    for (std::size_t i = 0; i + unit <= out.size(); i += unit) std::reverse(out.begin() + i, out.begin() + i + unit);
    return out;
}

void TiffIfd::set_raw(uint16_t tag, uint16_t type, uint32_t count, std::vector<uint8_t> data) {
    remove(tag);
    entries_.push_back(Entry{tag, type, count, std::move(data)});
}

void TiffIfd::set_ascii(uint16_t tag, const std::string& s) {
    std::vector<uint8_t> d(s.begin(), s.end());
    d.push_back(0);
    const uint32_t n = static_cast<uint32_t>(d.size());
    set_raw(tag, kAscii, n, std::move(d));
}

void TiffIfd::set_bytes(uint16_t tag, const std::vector<uint8_t>& v, uint16_t type) {
    set_raw(tag, type, static_cast<uint32_t>(v.size()), v);
}

void TiffIfd::set_short(uint16_t tag, const std::vector<uint16_t>& v) {
    std::vector<uint8_t> d;
    for (uint16_t x : v) put_u16(d, x);
    set_raw(tag, kShort, static_cast<uint32_t>(v.size()), std::move(d));
}

void TiffIfd::set_long(uint16_t tag, const std::vector<uint32_t>& v) {
    std::vector<uint8_t> d;
    for (uint32_t x : v) put_u32(d, x);
    set_raw(tag, kLong, static_cast<uint32_t>(v.size()), std::move(d));
}

void TiffIfd::set_rational(uint16_t tag, const std::vector<double>& v) {
    std::vector<uint8_t> d;
    for (double x : v) {
        int64_t num, den;
        to_fraction(x, false, num, den);
        put_u32(d, static_cast<uint32_t>(num));
        put_u32(d, static_cast<uint32_t>(den));
    }
    set_raw(tag, kRational, static_cast<uint32_t>(v.size()), std::move(d));
}

void TiffIfd::set_srational(uint16_t tag, const std::vector<double>& v) {
    std::vector<uint8_t> d;
    for (double x : v) {
        int64_t num, den;
        to_fraction(x, true, num, den);
        put_u32(d, static_cast<uint32_t>(static_cast<int32_t>(num)));
        put_u32(d, static_cast<uint32_t>(den));
    }
    set_raw(tag, kSRational, static_cast<uint32_t>(v.size()), std::move(d));
}

void TiffIfd::set_rational_exact(uint16_t tag, const std::vector<std::pair<uint32_t, uint32_t>>& v) {
    std::vector<uint8_t> d;
    for (const auto& x : v) {
        put_u32(d, x.first);
        put_u32(d, x.second);
    }
    set_raw(tag, kRational, static_cast<uint32_t>(v.size()), std::move(d));
}

void TiffIfd::set_double(uint16_t tag, const std::vector<double>& v) {
    std::vector<uint8_t> d;
    for (double x : v) {
        uint64_t b;
        std::memcpy(&b, &x, 8);
        put_u32(d, static_cast<uint32_t>(b));
        put_u32(d, static_cast<uint32_t>(b >> 32));
    }
    set_raw(tag, kDouble, static_cast<uint32_t>(v.size()), std::move(d));
}

bool TiffIfd::has(uint16_t tag) const {
    for (const Entry& e : entries_) {
        if (e.tag == tag) return true;
    }
    return false;
}

void TiffIfd::remove(uint16_t tag) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.tag == tag; }),
                   entries_.end());
}

TiffIfd* TiffIfd::add_child(uint16_t tag) {
    children_.emplace_back(tag, std::unique_ptr<TiffIfd>(new TiffIfd));
    return children_.back().second.get();
}

void TiffIfd::set_image_blocks(uint16_t offsets_tag, uint16_t counts_tag, std::vector<std::vector<uint8_t>> blocks) {
    offsets_tag_ = offsets_tag;
    counts_tag_ = counts_tag;
    blocks_ = std::move(blocks);
}

uint32_t TiffWriter::emit(TiffIfd& ifd, std::vector<uint8_t>& out) {
    const auto align = [&out]() {
        if (out.size() & 1) out.push_back(0);
    };
    const auto check = [&out]() {
        if (out.size() > 0xFFFFFFF0ull) throw std::runtime_error("4GB を超える DNG は書けません");
    };

    // 画像データ
    if (!ifd.blocks_.empty()) {
        std::vector<uint32_t> offsets, counts;
        for (std::vector<uint8_t>& b : ifd.blocks_) {
            align();
            offsets.push_back(static_cast<uint32_t>(out.size()));
            counts.push_back(static_cast<uint32_t>(b.size()));
            out.insert(out.end(), b.begin(), b.end());
            check();
            std::vector<uint8_t>().swap(b);
        }
        ifd.set_long(ifd.offsets_tag_, offsets);
        ifd.set_long(ifd.counts_tag_, counts);
    }

    // 子の IFD
    std::map<uint16_t, std::vector<uint32_t>> child_offsets;
    for (auto& c : ifd.children_) child_offsets[c.first].push_back(emit(*c.second, out));
    for (auto& kv : child_offsets) ifd.set_long(kv.first, kv.second);

    // この IFD
    align();
    std::sort(ifd.entries_.begin(), ifd.entries_.end(),
              [](const TiffIfd::Entry& a, const TiffIfd::Entry& b) { return a.tag < b.tag; });
    const uint32_t start = static_cast<uint32_t>(out.size());
    const std::size_t n = ifd.entries_.size();
    std::size_t data_pos = start + 2 + 12 * n + 4;
    std::vector<uint8_t> head, tail;
    put_u16(head, static_cast<uint16_t>(n));
    for (const TiffIfd::Entry& e : ifd.entries_) {
        put_u16(head, e.tag);
        put_u16(head, e.type);
        put_u32(head, e.count);
        if (e.data.size() <= 4) {
            std::vector<uint8_t> v(e.data);
            v.resize(4, 0);
            head.insert(head.end(), v.begin(), v.end());
        } else {
            put_u32(head, static_cast<uint32_t>(data_pos + tail.size()));
            tail.insert(tail.end(), e.data.begin(), e.data.end());
            if (tail.size() & 1) tail.push_back(0);
        }
    }
    put_u32(head, 0);  // 次の IFD はない
    out.insert(out.end(), head.begin(), head.end());
    out.insert(out.end(), tail.begin(), tail.end());
    check();
    return start;
}

std::vector<uint8_t> TiffWriter::build() {
    std::vector<uint8_t> out = {'I', 'I', 42, 0, 0, 0, 0, 0};
    const uint32_t root = emit(root_, out);
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<uint8_t>(root >> (8 * i));
    return out;
}

void TiffWriter::write(const std::string& path) {
    const std::vector<uint8_t> data = build();
    const std::string tmp = path + ".tmp";
    FILE* fp = std::fopen(tmp.c_str(), "wb");
    if (!fp) throw std::runtime_error("書き出し先を開けません: " + path);
    const std::size_t w = std::fwrite(data.data(), 1, data.size(), fp);
    const bool ok = w == data.size() && std::fflush(fp) == 0;
    std::fclose(fp);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        throw std::runtime_error("書き出しに失敗しました（ディスクの空きを確かめてください）: " + path);
    }
}

}  // namespace hdr
