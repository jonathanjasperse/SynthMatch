#pragma once

// Runs a loop body on several threads.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace synthmatch::match {

// 0 or less means "one per CPU core, leaving one free" so that whatever else
// is running (a DAW's audio and interface) stays responsive.
inline unsigned threadCount(int requested)
{
    if (requested > 0)
        return static_cast<unsigned>(requested);
    const unsigned cores = std::thread::hardware_concurrency();
    return cores > 2 ? cores - 1 : std::max(1u, cores);
}

// Calls fn(i) for every i in [0, count). Items are handed out one at a time, so
// uneven work balances itself. fn must be safe to call from several threads.
template <class Fn>
void parallelFor(std::size_t count, unsigned threads, Fn&& fn)
{
    if (threads <= 1 || count <= 1) {
        for (std::size_t i = 0; i < count; ++i)
            fn(i);
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    const std::size_t workers = std::min<std::size_t>(threads, count);
    pool.reserve(workers);
    for (std::size_t t = 0; t < workers; ++t) {
        pool.emplace_back([&] {
            for (;;) {
                const std::size_t i = next.fetch_add(1);
                if (i >= count)
                    break;
                fn(i);
            }
        });
    }
    for (auto& t : pool)
        t.join();
}

} // namespace synthmatch::match
