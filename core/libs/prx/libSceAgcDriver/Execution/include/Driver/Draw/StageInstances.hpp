#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_STAGEINSTANCES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_STAGEINSTANCES_HPP

#include "Recompiler.hpp"
#include "UserDataUse.hpp"
#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ShaderRecompiler {
struct ResourceCapture;
}

namespace AgcDriver {
class ShaderMemory;
}

namespace AgcDriver::DriverDetail {

bool StageInstancesEnabled();
bool StageInstancesVerify();
void StageInstancesBeginDraw(std::uint64_t shapeKey);
std::uint64_t StageInstanceKey(const void* shader, std::uint64_t codeAddress, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);

struct StageInstanceHit {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> compiled;
    ShaderRecompiler::SrtUserDataUse userDataUse;
};
std::array<std::uint32_t, 5> StageInstanceDrawWords(const ShaderRecompiler::RecompileRequest& request);
std::optional<StageInstanceHit> StageInstanceTry(std::uint64_t key, std::span<const std::uint32_t> userData, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const std::array<std::uint32_t, 5>& draw, ShaderMemory& memory);
void StageInstanceLearn(std::uint64_t key, const std::shared_ptr<const void>& shader, std::span<const std::uint32_t> userData, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertex, const std::array<std::uint32_t, 5>& draw, const ShaderRecompiler::ResourceCapture& capture, std::span<const ShaderRecompiler::MemoryRegion> regions, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiled);
void StageInstanceCompare(std::uint64_t key, std::uint64_t codeAddress, const ShaderRecompiler::RecompileResult& fast, std::span<const ShaderRecompiler::MemoryRegion> fastRegions, const ShaderRecompiler::RecompileResult& full, std::span<const ShaderRecompiler::MemoryRegion> fullRegions);
void StageInstanceTimed(double fastMicroseconds, double fullMicroseconds);

}

#endif
