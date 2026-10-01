#pragma once

// 出力形式（CFA か LinearRaw か）の決め方（docs/検証記録.md「CFA と LinearRaw の精度」）。
//
// Camera Raw は CFA の DNG を、白レベルを上限としておよそ 16bit の精度で色補間する。場面の明暗差が
// 大きいと、最も暗い所の値がその刻み（白レベルの 2^-16）に近づき、持ち上げたときに段差が出る。
// ただし、暗い所のノイズが刻みより十分大きければ、段差はノイズに紛れて見えない。
// そこで「最も暗い所のノイズ ÷ 刻み」で決める。足りなければ LinearRaw（浮動小数点のまま処理される）。

#include <string>

#include "hdrcore/merge.hpp"

namespace hdr {

enum class OutputFormat { Auto, Cfa, LinearRaw };

struct FormatDecision {
    OutputFormat chosen = OutputFormat::Cfa;  // Auto 以外
    double shadow_level = 0.0;   // 暗い所（明るさの下から 2〜20%）の値の中央値（白 = 1）
    double shadow_noise = 0.0;   // そこでのノイズ（標準偏差）
    double noise_to_step = 0.0;  // ノイズ ÷ Camera Raw の刻み（2^-16）
    std::string reason;
};

// 暗い所のノイズが刻みのこの倍以上なら CFA で段差は見えない、とみなす。
// 実写で確かめたのは 0.09・0.11（段差が出た）と 4.4 以上（出なかった）だけなので、間を取りつつ
// 段差が出ない側に寄せて 1.0 にしている（docs/検証記録.md）。
constexpr double kCfaNoiseToStep = 1.0;

FormatDecision decide_output_format(const MergeResult& merged, OutputFormat requested);

const char* format_name(OutputFormat f);

}  // namespace hdr
