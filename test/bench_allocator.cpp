#include "../include/allocator/allocator.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Scenario {
    std::string name;
    std::vector<std::size_t> sizes;
    std::size_t iterations = 0;
    std::size_t thread_count = 1;
};

struct Result {
    std::string allocator_name;
    std::string scenario_name;
    std::size_t operations = 0;
    double seconds = 0.0;
    double qps = 0.0;
    double p99_ns = 0.0;
    std::size_t current_bytes = 0;
    std::size_t peak_bytes = 0;
};

using AllocFn = void* (*)(std::size_t);
using FreeFn = void (*)(void*);

std::vector<long long> RunWorker(AllocFn alloc_fn,
                                 FreeFn free_fn,
                                 const Scenario& scenario,
                                 std::size_t worker_id) {
    std::vector<long long> samples;
    samples.reserve(scenario.iterations);

    for (std::size_t i = 0; i < scenario.iterations; ++i) {
        const std::size_t size =
            scenario.sizes[(i + worker_id) % scenario.sizes.size()];
        const auto begin = Clock::now();
        void* ptr = alloc_fn(size);
        free_fn(ptr);
        const auto end = Clock::now();
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(
                              end - begin)
                              .count());
    }

    return samples;
}

Result RunScenario(const std::string& allocator_name,
                   AllocFn alloc_fn,
                   FreeFn free_fn,
                   const Scenario& scenario) {
    const auto begin = Clock::now();
    std::vector<std::vector<long long>> all_samples(scenario.thread_count);
    std::vector<std::thread> threads;
    threads.reserve(scenario.thread_count);

    for (std::size_t i = 0; i < scenario.thread_count; ++i) {
        threads.emplace_back([&, i] {
            all_samples[i] = RunWorker(alloc_fn, free_fn, scenario, i);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    const auto end = Clock::now();
    std::vector<long long> merged;
    for (const auto& samples : all_samples) {
        merged.insert(merged.end(), samples.begin(), samples.end());
    }
    std::sort(merged.begin(), merged.end());

    Result result;
    result.allocator_name = allocator_name;
    result.scenario_name = scenario.name;
    result.operations = scenario.iterations * scenario.thread_count;
    result.seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(end - begin)
            .count();
    result.qps = result.operations / std::max(result.seconds, 1e-9);
    if (!merged.empty()) {
        const std::size_t p99_index = (merged.size() - 1) * 99 / 100;
        result.p99_ns = static_cast<double>(merged[p99_index]);
    }

    if (allocator_name == "fastallocator") {
        const my_alloc::Stats stats = my_alloc::GetStats();
        result.current_bytes = stats.current_bytes;
        result.peak_bytes = stats.peak_bytes;
    }
    return result;
}

void* SystemMalloc(std::size_t size) {
    return std::malloc(size);
}

void SystemFree(void* ptr) {
    std::free(ptr);
}

void PrintResult(const Result& result) {
    std::cout << std::left << std::setw(14) << result.allocator_name
              << std::setw(24) << result.scenario_name << std::right
              << std::setw(12) << result.operations << std::setw(14)
              << std::fixed << std::setprecision(2) << result.qps
              << std::setw(14) << std::fixed << std::setprecision(0)
              << result.p99_ns << std::setw(14) << result.current_bytes
              << std::setw(14) << result.peak_bytes << '\n';
}

}  // namespace

int main() {
    my_alloc::initialize();

    const std::vector<Scenario> scenarios = {
        {"single-fixed-64", {64}, 50000, 1},
        {"single-mixed", {8, 24, 64, 256, 1024, 4096}, 50000, 1},
        {"multi-fixed-64", {64}, 30000, 4},
        {"multi-mixed", {8, 24, 64, 256, 1024, 4096, 32768}, 30000, 4},
    };

    std::cout << std::left << std::setw(14) << "allocator"
              << std::setw(24) << "scenario" << std::right << std::setw(12)
              << "ops" << std::setw(14) << "qps" << std::setw(14) << "p99(ns)"
              << std::setw(14) << "current" << std::setw(14) << "peak" << '\n';

    for (const Scenario& scenario : scenarios) {
        PrintResult(RunScenario(
            "system", &SystemMalloc, &SystemFree, scenario));
        PrintResult(RunScenario(
            "fastallocator", &my_alloc::malloc, &my_alloc::free, scenario));
    }

    return 0;
}