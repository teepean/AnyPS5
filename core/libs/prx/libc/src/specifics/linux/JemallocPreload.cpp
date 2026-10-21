#include "prx/libc/include/specifics/linux/JemallocPreload.hpp"

#include <string_view>

#if defined(__linux__)
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <system_error>
#include <unistd.h>
#endif

namespace JemallocPreload {

std::optional<std::string> PreloadList(const char* request, const char* preload) {
    if (request == nullptr) return std::nullopt;
    const std::string_view current = preload != nullptr ? preload : "";
    if (current.find("libjemalloc") != std::string_view::npos) return std::nullopt;
    std::string list = Library;
    if (!current.empty()) list.append(":").append(current);
    return list;
}

}

#if defined(__linux__)
namespace {

[[gnu::constructor(101)]] void restartWithJemalloc(int, char** arguments, char**) {
    const char* request = std::getenv("APS5_JEMALLOC");
    const auto list = JemallocPreload::PreloadList(request, std::getenv("LD_PRELOAD"));
    if (!list) {
        if (request != nullptr && dlopen(JemallocPreload::Library, RTLD_LAZY | RTLD_NOLOAD) == nullptr)
            std::fprintf(stderr, "[allocator] APS5_JEMALLOC is set but %s is not loaded; the C library's allocator is used\n", JemallocPreload::Library);
        return;
    }
    if (arguments == nullptr || arguments[0] == nullptr) {
        std::fprintf(stderr, "[allocator] APS5_JEMALLOC: the process arguments are unknown, so it is not restarted with %s\n", JemallocPreload::Library);
        return;
    }
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) {
        std::fprintf(stderr, "[allocator] APS5_JEMALLOC: cannot read /proc/self/exe (%s), so the process is not restarted with %s\n", error.message().c_str(), JemallocPreload::Library);
        return;
    }
    if (setenv("LD_PRELOAD", list->c_str(), 1) != 0) return;
    execv(executable.c_str(), arguments);
    std::fprintf(stderr, "[allocator] APS5_JEMALLOC: restarting with %s preloaded failed: %s\n", JemallocPreload::Library, std::strerror(errno));
}

}
#endif
