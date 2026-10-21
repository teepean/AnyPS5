#include "prx/libc/include/specifics/linux/JemallocPreload.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "jemalloc preload test failed: %s\n", what);
    std::exit(1);
}

static int Restarted(const char* program) {
    std::string name;
    std::getline(std::ifstream("/proc/self/comm"), name);
    Require(name == std::filesystem::path(program).filename().string().substr(0, 15), "the restarted process lost its name");
    const char* preload = std::getenv("LD_PRELOAD");
    Require(preload != nullptr && std::string_view(preload).starts_with(JemallocPreload::Library), "the process was not restarted with jemalloc first in LD_PRELOAD");
    if (dlopen(JemallocPreload::Library, RTLD_LAZY | RTLD_NOLOAD) == nullptr) {
        Require(dlopen(JemallocPreload::Library, RTLD_LAZY | RTLD_LOCAL) == nullptr, "jemalloc is installed but was not preloaded");
        std::fprintf(stderr, "jemalloc preload test skipped: %s is not installed\n", JemallocPreload::Library);
        return 77;
    }
    Dl_info info{};
    Require(dladdr(dlsym(RTLD_DEFAULT, "malloc"), &info) != 0 && info.dli_fname != nullptr, "malloc has no owning object");
    Require(std::strstr(info.dli_fname, "libjemalloc") != nullptr, "malloc does not resolve to jemalloc");
    void* block = std::malloc(4096);
    Require(block != nullptr, "jemalloc allocation");
    std::free(block);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "restarted") == 0) return Restarted(argv[0]);
    Require(!JemallocPreload::PreloadList(nullptr, nullptr), "no request leaves LD_PRELOAD alone");
    Require(!JemallocPreload::PreloadList(nullptr, "/opt/other.so"), "no request leaves another preload alone");
    Require(JemallocPreload::PreloadList("1", nullptr) == std::string(JemallocPreload::Library), "a request preloads jemalloc");
    Require(JemallocPreload::PreloadList("", "") == std::string(JemallocPreload::Library), "an empty request counts and an empty list is replaced");
    Require(JemallocPreload::PreloadList("1", "/opt/other.so") == std::string(JemallocPreload::Library) + ":/opt/other.so", "jemalloc goes before other preloads");
    Require(!JemallocPreload::PreloadList("1", "/usr/lib/libjemalloc.so.2"), "an existing jemalloc preload is kept");
    Require(!JemallocPreload::PreloadList("1", "/opt/a.so:libjemalloc.so.2"), "jemalloc anywhere in the list is kept");
    return 0;
}
