#include "showmesh/safe_ceiling_setting.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>

#include "showmesh/brightness.h"

namespace showmesh {

bool parseSafeCeilingSetting(const std::string& value, int* outPercent) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    const std::string trimmed = value.substr(begin, end - begin);
    if (trimmed.empty()) return false;

    char* stop = nullptr;
    errno = 0;
    const long long parsed = std::strtoll(trimmed.c_str(), &stop, 10);
    if (errno != 0 || stop == nullptr || *stop != '\0') return false;
    if (parsed < kMinPercent || parsed > kMaxPercent) return false;

    *outPercent = static_cast<int>(parsed);
    return true;
}

}  // namespace showmesh
