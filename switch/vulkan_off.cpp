// ROTS: the build without NVK (no mesa-switch libvulkan.a): the deko3d renderer is the only one.
#include <memory>

#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

std::unique_ptr<Backend> CreateDekoBackend();  // deko.cpp

std::unique_ptr<Backend> CreateBackend() {
    LOGR("renderer: deko3d (this build has no Vulkan)");
    return CreateDekoBackend();
}

void WaitGpuFrame(uint64_t) {}

}  // namespace gpu
