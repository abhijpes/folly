/*
 * Copyright 2018-present Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/stats/detail/DigestBuilder-defs.h>

#include <chrono>
#include <condition_variable>
#include <thread>

#include <folly/Benchmark.h>
#include <folly/Range.h>
#include <folly/portability/GFlags.h>

DEFINE_int32(digest_merge_time_ns, 13000, "Time to merge into the digest");

using namespace folly;
using namespace folly::detail;

class FreeDigest {
 public:
  explicit FreeDigest(size_t) {}

  FreeDigest merge(Range<const double*>) const {
    auto start = std::chrono::steady_clock::now();
    auto finish = start + std::chrono::nanoseconds{FLAGS_digest_merge_time_ns};
    while (std::chrono::steady_clock::now() < finish) {
    }
    return FreeDigest(100);
  }
};

unsigned int append(unsigned int iters, size_t bufSize, size_t nThreads) {
  iters = 1000000;
  auto buffer = std::make_shared<DigestBuilder<FreeDigest>>(bufSize, 100);

  std::atomic<size_t> numDone{0};

  std::mutex m;
  size_t numWaiters = 0;
  std::condition_variable cv;

  std::vector<std::thread> threads;
  threads.reserve(nThreads);
  BENCHMARK_SUSPEND {
    for (size_t i = 0; i < nThreads; ++i) {
      threads.emplace_back([&]() {
        {
          std::unique_lock<std::mutex> g(m);
          ++numWaiters;
          cv.wait(g);
        }
        for (size_t iter = 0; iter < iters; ++iter) {
          buffer->append(iter);
        }
        ++numDone;
      });
    }
    while (true) {
      {
        std::unique_lock<std::mutex> g(m);
        if (numWaiters < nThreads) {
          continue;
        }
      }
      cv.notify_all();
      break;
    }
  }

  while (numDone < nThreads) {
  }

  BENCHMARK_SUSPEND {
    for (auto& thread : threads) {
      thread.join();
    }
  }
  return iters;
}

BENCHMARK_NAMED_PARAM_MULTI(append, 1000x1, 1000, 1)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 1000x2, 1000, 2)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 1000x4, 1000, 4)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 1000x8, 1000, 8)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 1000x16, 1000, 16)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 1000x32, 1000, 32)
BENCHMARK_DRAW_LINE();
BENCHMARK_NAMED_PARAM_MULTI(append, 10000x1, 10000, 1)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 10000x2, 10000, 2)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 10000x4, 10000, 4)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 10000x8, 10000, 8)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 10000x16, 10000, 16)
BENCHMARK_RELATIVE_NAMED_PARAM_MULTI(append, 10000x32, 10000, 32)

/*
 * ./digest_buffer_benchmark
 * ============================================================================
 * folly/stats/test/DigestBuilderBenchmark.cpp     relative  time/iter  iters/s
 * ============================================================================
 * append(1000x1)                                              51.79ns   19.31M
 * append(1000x2)                                    97.04%    53.37ns   18.74M
 * append(1000x4)                                    95.68%    54.12ns   18.48M
 * append(1000x8)                                    91.95%    56.32ns   17.76M
 * append(1000x16)                                   62.12%    83.36ns   12.00M
 * append(1000x32)                                   38.12%   135.85ns    7.36M
 * ----------------------------------------------------------------------------
 * append(10000x1)                                             46.34ns   21.58M
 * append(10000x2)                                   97.91%    47.33ns   21.13M
 * append(10000x4)                                   95.27%    48.64ns   20.56M
 * append(10000x8)                                   91.39%    50.70ns   19.72M
 * append(10000x16)                                  55.26%    83.85ns   11.93M
 * append(10000x32)                                  35.57%   130.25ns    7.68M
 * ============================================================================
 */

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::runBenchmarks();
  return 0;
}
