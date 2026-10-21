#ifndef PRX_LIBC_INCLUDE_SPECIFICS_LINUX_JEMALLOCPRELOAD_HPP
#define PRX_LIBC_INCLUDE_SPECIFICS_LINUX_JEMALLOCPRELOAD_HPP

#include <optional>
#include <string>

namespace JemallocPreload {

inline constexpr const char* Library = "libjemalloc.so.2";

std::optional<std::string> PreloadList(const char* request, const char* preload);

}

#endif
