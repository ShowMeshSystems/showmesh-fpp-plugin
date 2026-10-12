#include "showmesh/channel_ranges_setting.h"

#include <cstdlib>

namespace showmesh {
namespace {

constexpr unsigned long long kMaxChannel = 0xFFFFFFFFULL;

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Digits only, so strtoull's own tolerance for a sign or leading space
// cannot let "+5" or "0x10" through as a channel number.
bool wholeNumber(const std::string& text, unsigned long long* out) {
    if (text.empty() || text.size() > 10) return false;
    if (text.find_first_not_of("0123456789") != std::string::npos) return false;
    *out = std::strtoull(text.c_str(), nullptr, 10);
    return true;
}

bool strictToken(const std::string& token, ChannelRange* out) {
    const std::size_t dash = token.find('-');
    if (dash == std::string::npos) return false;
    unsigned long long start = 0;
    unsigned long long count = 0;
    if (!wholeNumber(token.substr(0, dash), &start) || !wholeNumber(token.substr(dash + 1), &count)) return false;
    if (start == 0 || count == 0 || start + count > kMaxChannel) return false;
    *out = ChannelRange{static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(count)};
    return true;
}

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

ChannelRangesParse parseChannelRangesSettingStrict(const std::string& value) {
    ChannelRangesParse result;
    if (trimmed(value).empty()) return result;
    std::size_t pos = 0;
    while (true) {
        const std::size_t comma = value.find(',', pos);
        const std::string token =
            trimmed(value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        ChannelRange range;
        if (!strictToken(token, &range)) {
            result.ok = false;
            result.ranges.clear();
            result.error = "\"" + token + "\" is not a start-count pair such as 1-512";
            return result;
        }
        result.ranges.push_back(range);
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

std::string formatChannelRangesSetting(const std::vector<ChannelRange>& ranges) {
    std::string text;
    for (const ChannelRange& r : ranges) {
        if (!text.empty()) text += ',';
        text += std::to_string(r.startChannel) + "-" + std::to_string(r.channelCount);
    }
    return text;
}

std::vector<std::string> configureChannelRangesFrom(const ChannelRangeSources& sources, std::uint32_t totalChannels,
                                                    BrightnessEngine* engine, std::string* lastRejectedExclude) {
    std::vector<std::string> log;
    RangeConfig config;
    config.exclude = engine->ranges().exclude;

    ChannelRangesParse excluded = parseChannelRangesSettingStrict(sources.excludeSetting);
    if (excluded.ok) {
        RangeConfig excludeOnly;
        excludeOnly.exclude = excluded.ranges;
        const ValidationResult valid = validateRanges(excludeOnly, totalChannels);
        if (!valid.ok) {
            excluded.ok = false;
            excluded.error = valid.error;
        }
    }
    if (excluded.ok) {
        config.exclude = excluded.ranges;
        lastRejectedExclude->clear();
    } else if (*lastRejectedExclude != sources.excludeSetting) {
        *lastRejectedExclude = sources.excludeSetting;
        log.push_back("ShowMeshExcludeChannelRanges setting rejected: " + excluded.error +
                      "; keeping the previous excluded channels");
    }

    config.apply = parseChannelRangesSetting(sources.applySetting);
    if (!config.apply.empty()) {
        const ValidationResult result = engine->configureRanges(config, totalChannels);
        if (result.ok) return log;
        log.push_back("ShowMeshChannelRanges setting rejected: " + result.error + "; falling back to output ranges");
    }

    config.apply = sources.outputRanges;
    if (!config.apply.empty()) {
        const ValidationResult result = engine->configureRanges(config, totalChannels);
        if (result.ok) return log;
        log.push_back("GetOutputRanges() produced ranges the engine rejected: " + result.error +
                      "; using the full channel buffer");
    }

    config.apply.assign(1, ChannelRange{1, totalChannels});
    engine->configureRanges(config, totalChannels);
    return log;
}

}  // namespace showmesh
