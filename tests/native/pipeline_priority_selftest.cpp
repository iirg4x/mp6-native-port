#include <cstdio>
#include <utility>

namespace clear { struct PipelineConfig {}; }
namespace gx { struct PipelineConfig {}; }
enum class ShaderType { Clear, GX };
enum class PipelinePriority { Background, Normal, Blocking };
using PipelineRef = unsigned;
using NewPipelineCallback = int;
static PipelinePriority priority_seen;
template <typename Config>
PipelineRef find_pipeline(ShaderType, const Config&, NewPipelineCallback&&);
template <typename Config>
PipelineRef find_pipeline_impl(ShaderType, const Config&, NewPipelineCallback&&,
                              PipelinePriority priority = PipelinePriority::Normal) {
    priority_seen = priority;
    return 42;
}
#include "pipeline_subject.inc"
int main() {
    auto a = find_pipeline(ShaderType::Clear, clear::PipelineConfig{}, 0);
    if (a != 42 || priority_seen != PipelinePriority::Blocking) {
        std::puts("FAIL clear can be skipped while compiling"); return 1;
    }
    auto b = find_pipeline(ShaderType::GX, gx::PipelineConfig{}, 0);
    if (b != 42 || priority_seen != PipelinePriority::Blocking) {
        std::puts("FAIL game draw can be skipped while compiling"); return 1;
    }
    std::puts("PASS required pipeline priority");
}
