#pragma once

#include "profiler.h"

#include <memory>
#include <cstdint>

namespace drop {

// Factory: create the right IProfiler based on profiler_type from proto.
std::unique_ptr<IProfiler> CreateProfiler(uint32_t profiler_type);

}  // namespace drop
