#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_SHAPECENSUS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_SHAPECENSUS_HPP

#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ShaderRecompiler {
struct ResourceCapture;
}

namespace AgcDriver::DriverDetail {

bool ShapeCensusEnabled();
void ShapeCensusBeginDraw(std::uint64_t shapeKey);
void ShapeCensusStage(std::uint64_t codeAddress, std::size_t codeOffset, std::uint32_t pushOffset, std::span<const std::uint32_t> userData, const std::shared_ptr<const ShaderRecompiler::ResourceCapture>& capture, std::span<const ShaderRecompiler::MemoryRegion> regions, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& compiled, double stageMicroseconds);

}

#endif
