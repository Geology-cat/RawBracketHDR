#pragma once

// 行（または任意の範囲）を分割して並列に処理する小さな道具。
// GCD の dispatch_apply_f を使う（10.12 でも使える C の API）。

#include <dispatch/dispatch.h>

#include <algorithm>
#include <functional>

namespace hdr {

// [0, n) を塊に分け、fn(begin, end) を並列に呼ぶ。
inline void parallel_for(int n, const std::function<void(int, int)>& fn, int min_chunk = 16) {
    if (n <= 0) return;
    const int chunks = std::max(1, std::min(n / std::max(1, min_chunk), 256));
    struct Ctx {
        const std::function<void(int, int)>* fn;
        int n, chunks;
    } ctx{&fn, n, chunks};
    dispatch_apply_f(static_cast<size_t>(chunks), dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), &ctx,
                     [](void* p, size_t i) {
                         const Ctx* c = static_cast<const Ctx*>(p);
                         const int b = static_cast<int>(static_cast<long long>(c->n) * static_cast<long long>(i) / c->chunks);
                         const int e = static_cast<int>(static_cast<long long>(c->n) * static_cast<long long>(i + 1) / c->chunks);
                         if (b < e) (*c->fn)(b, e);
                     });
}

}  // namespace hdr
