#pragma once

// Adobe DNG Converter で基準フレームを DNG に変換し、Adobe の解釈（色・明るさ・プロファイル・
// レンズ補正の命令）を合成結果の DNG に引き継ぐ（開発計画書 §6.3「テンプレートDNG方式」）。
//
// LibRaw からは得られない次の値が手に入る。
// - BaselineExposure と白レベル（Adobe が機種・ISO ごとに持つ。6D Mark II の ISO 640 では白 11301・+0.25）
// - CameraCalibration（機種ごとの色の補正。ホワイトバランスの解釈が Camera Raw と揃う）
// - 2つの光源の ColorMatrix・ForwardMatrix と Adobe Standard のプロファイル
// - OpcodeList（カメラ内蔵のレンズ補正など）
//
// DNG Converter が無いときは使わない（LibRaw の値で書く）。

#include <cstdint>
#include <string>
#include <vector>

#include "hdrcore/raw_frame.hpp"

namespace hdr {

struct DngTemplate {
    bool valid = false;
    std::string converter_version;
    std::vector<TiffEntry> ifd0;  // 引き継ぐ IFD0 のタグ（値はリトルエンディアンに直してある）
    std::vector<TiffEntry> raw;   // 引き継ぐ RAW の IFD のタグ（OpcodeList など）
    double baseline_exposure = 0.0;
    double white_minus_black = 0.0;  // Adobe の白レベル − 黒レベル（元の RAW の DN）
    int active_width = 0, active_height = 0;
    // Adobe が機種・ISO ごとに校正したノイズ（白−黒で割った値に対して、分散 = S·x + O。色ごと）。
    bool has_noise = false;
    double noise_S[3] = {}, noise_O[3] = {};
    std::string note;  // 引き継がなかったものなどの説明
};

// DNG Converter の場所（無ければ空）。/Applications と ~/Applications を探す。
std::string find_dng_converter();

// raw_path を DNG Converter で変換して読む。失敗したら valid = false（理由は note）。
DngTemplate make_dng_template(const std::string& raw_path, const std::string& converter);

// 既にある DNG を読む（入力が DNG のときや、テスト用）。
DngTemplate read_dng_template(const std::string& dng_path);

}  // namespace hdr
