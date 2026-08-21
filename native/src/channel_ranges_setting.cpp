#include "showmesh/channel_ranges_setting.h"

#include <cstdlib>

namespace showmesh {
namespace {

constexpr unsigned long long kMaxChannel = 0xFFFFFFFFULL;

}  // namespace

std::vector<ChannelRange> parseChannelRangesSetting(const std::string& value) {
    std::vector<ChannelRange> ranges;
    std::size_t pos = 0;
    while (pos < value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string token = value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        const std::size_t dash = token.find('-');
        if (dash != std::string::npos) {
            // Held as named locals, not as substr() temporaries: strtoull
            // writes its endptr into the string's own buffer, and reading
            // that endptr after the temporary that owned the buffer is
            // destroyed is undefined behavior invisible outside a
            // sanitizer build.
            const std::string startText = token.substr(0, dash);
            const std::string countText = token.substr(dash + 1);
            char* endStart = nullptr;
            char* endCount = nullptr;
            const unsigned long long start = std::strtoull(startText.c_str(), &endStart, 10);
            const unsigned long long count = std::strtoull(countText.c_str(), &endCount, 10);
            const bool wholeToken = !startText.empty() && endStart != nullptr && *endStart == '\0' &&
                                    !countText.empty() && endCount != nullptr && *endCount == '\0';
            const bool inRange = start > 0 && start <= kMaxChannel && count > 0 && count <= kMaxChannel &&
                                 start + count <= kMaxChannel;
            if (wholeToken && inRange) {
                ranges.push_back(ChannelRange{static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(count)});
            }
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return ranges;
}

}  // namespace showmesh
