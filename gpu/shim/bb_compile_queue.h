#pragma once
#include <array>
#include <cstddef>

namespace BbCompile {
enum class JobKind { Shader, Graphics, Compute, None };
// A stream of shader translations cannot indefinitely postpone a ready pipeline.
// Two shader opportunities per cycle preserve their role as pipeline prerequisites.
inline JobKind SelectFairJob(std::size_t shaders, std::size_t graphics,
                             std::size_t compute, unsigned& cursor) {
    constexpr std::array cycle{JobKind::Shader, JobKind::Graphics,
                               JobKind::Shader, JobKind::Compute};
    for (unsigned i = 0; i < cycle.size(); ++i) {
        const unsigned position = (cursor + i) % cycle.size();
        const auto kind = cycle[position];
        if ((kind == JobKind::Shader && shaders) ||
            (kind == JobKind::Graphics && graphics) ||
            (kind == JobKind::Compute && compute)) {
            cursor = (position + 1) % cycle.size();
            return kind;
        }
    }
    return JobKind::None;
}
} // namespace BbCompile
