#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_DRIVER_DRAW_DRAWREGISTERKEY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_DRIVER_DRAW_DRAWREGISTERKEY_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"

namespace AgcDriver::DriverDetail {

struct RegisterStateKeyParts {
    std::uint64_t stableRegisters = 0;
    std::uint64_t userDataRegisters = 0;
    std::uint64_t auxiliaryRegisters = 0;
};

std::uint64_t RegisterStateKey(const QueueState& queue, bool allUserWords = false, bool incremental = true, std::uint64_t* shapeKey = nullptr, RegisterStateKeyParts* parts = nullptr);
// Gives the live queue state's register banks their key masks and fingerprints, so copies handed
// to draw preparations carry them and keep the incremental fingerprint.
void PrimeRegisterFingerprints(const QueueState& queue);
std::uint64_t DrawShaderSetKey(const QueueState& queue, const ShaderRegistry& registry);

}

#endif
