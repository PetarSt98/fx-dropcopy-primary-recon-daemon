#include <chrono>
#include <iostream>

#include "ingest/spsc_ring.hpp"
#include "ingest/fix_parser.hpp"
#include "core/exec_event.hpp"
#include "util/soh.hpp"

int main() {
    ingest::SpscRing<core::ExecEvent, 1u << 16> ring;
    core::ExecEvent evt{};

    const std::string msg = util::pipe_to_soh("8=FIX.4.4|35=8|150=2|39=2|17=EXEC1|11=CID1|37=OID1|31=1000000|32=100|14=100|52=1|60=1|");

    constexpr std::size_t iterations = 100000;
    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        ingest::parse_exec_report(msg.data(), msg.size(), evt);
        ring.try_push(evt);
        ring.try_pop(evt);
    }
    auto end = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "Benchmark loop " << iterations << " iterations took " << ns << " ns (" << (ns / iterations)
              << " ns/iter)\n";
    return 0;
}
