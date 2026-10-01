#pragma once

// 倍率色収差（横の色収差）の見積もりと補正。合成した CFA（ベイヤー）の上で、色補間・明暗差の圧縮の前に行う。
//
// レンズの倍率色収差では、R・B の像が G に対して画面の中心から放射方向にわずかに拡大・縮小される
// （明るい月の縁に、外側が赤・内側が緑の縁取りが出る）。Camera Raw の「色収差を除去」は色補間の前の
// RAW では効くが、合成・圧縮した後のデータでは十分に効かない（明暗差の圧縮で縁の明るさの形が変わり、
// 「R は G をずらしたもの」という関係が崩れるため）。そこで合成の直後に、R・B の面を放射方向に
// 拡大縮小して G に合わせる。
//
// 模型: 中心からの距離 r（対角線の半分 = 1）の点の R（B）は、G に対して倍率 1 + a + b·r² で写っている。
// a・b は、輪郭（明るさの対数の差が大きい所）で R と G の明るさの変化が一致するよう、総当たりで求める。

#include <string>

#include "hdrcore/merge.hpp"

namespace hdr {

struct LateralCa {
    bool valid = false;
    double a[3] = {0.0, 0.0, 0.0};  // 色ごと（G は 0）
    double b[3] = {0.0, 0.0, 0.0};
    double corner_shift_px[3] = {0.0, 0.0, 0.0};  // 画面の隅でのずれ（画素、確認用）
    double improvement[3] = {0.0, 0.0, 0.0};      // 補正で輪郭の不一致が減った割合（0〜1）
    int samples = 0;                               // 見積もりに使った輪郭の点の数
    std::string note;
};

// m.data（ベイヤーのみ）から見積もる。輪郭が少ない・改善が小さいときは valid = false。
LateralCa estimate_lateral_ca(const MergeResult& m);

// R・B の面を補正する（m.data を書き換える）。
void apply_lateral_ca(MergeResult& m, const LateralCa& ca);

}  // namespace hdr
