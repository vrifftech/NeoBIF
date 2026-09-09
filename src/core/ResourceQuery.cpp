#include "core/ResourceQuery.hpp"

#include <algorithm>
#include <map>

namespace neobif {
namespace {
char folded(char value) {
    const auto ch = static_cast<unsigned char>(value);
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : value;
}
std::string lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), folded);
    return out;
}
bool contains(std::string_view haystack, std::string_view needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char left, char right) { return folded(left) == right; }) != haystack.end();
}
bool isSpace(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}
bool wildcardMatch(std::string_view pattern, std::string_view name) {
    // Iterative matching: user-entered wildcards cannot recurse or grow a
    // regex engine stack. Patterns are matched against the whole filename.
    std::size_t p = 0, n = 0, star = std::string_view::npos, retry = 0;
    while (n < name.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == folded(name[n]))) {
            ++p;
            ++n;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            retry = n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++retry;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
bool isUnknownTypeExtension(std::string_view text) {
    // Unknown GFF/KEY resource types retain NeoBIF's type_ABCD convention.
    return text.size() == 9 && text.substr(0, 5) == "type_" &&
        std::all_of(text.begin() + 5, text.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
} // namespace

ResourceQuery::ResourceQuery(std::string text) {
    const auto first = std::find_if_not(text.begin(), text.end(), isSpace);
    const auto last = std::find_if_not(text.rbegin(), text.rend(), isSpace).base();
    if (first >= last) return;
    pattern_ = lower(std::string(first, last));
    if (pattern_.size() > 2 && pattern_.substr(0, 2) == "*." &&
        pattern_.find_first_of("*?", 2) == std::string::npos) {
        pattern_.erase(0, 2);
        mode_ = Mode::Extension;
    } else if (pattern_.front() == '.' && pattern_.find_first_of("*?") == std::string::npos) {
        pattern_.erase(0, 1);
        mode_ = Mode::Extension;
    } else if (pattern_.find_first_of("*?") != std::string::npos) {
        mode_ = Mode::FilenameGlob;
    } else if (isKnownResourceExtension(pattern_) || isUnknownTypeExtension(pattern_)) {
        mode_ = Mode::Extension;
    }
}

bool ResourceQuery::matchesContext(std::string_view text) const {
    return mode_ == Mode::Text && (pattern_.empty() || contains(text, pattern_));
}

bool ResourceQuery::matches(std::string_view fileName, std::string_view extension,
                            std::initializer_list<std::string_view> metadata) const {
    if (mode_ == Mode::Extension) return !pattern_.empty() && lower(extension) == pattern_;
    if (mode_ == Mode::FilenameGlob) return wildcardMatch(pattern_, fileName);
    if (matchesContext(fileName) || matchesContext(extension)) return true;
    return std::any_of(metadata.begin(), metadata.end(),
        [this](std::string_view text) { return matchesContext(text); });
}

std::vector<ResourceTypeSummary> summarizeResourceTypes(
    const KeyBifArchive& archive, const LooseArchiveCatalog& loose) {
    if (!archive.isOpen()) return {}; // NeoBIF is not a standalone archive editor.
    std::map<std::uint16_t, ResourceTypeSummary> types;
    const auto add = [&types](const auto& resources) {
        for (const auto& resource : resources) {
            auto& summary = types[resource.type];
            summary.type = resource.type;
            ++summary.total;
            if (resource.extractable) ++summary.available;
        }
    };
    add(archive.resources());
    add(loose.resources());
    std::vector<ResourceTypeSummary> result;
    result.reserve(types.size());
    for (auto& entry : types) {
        entry.second.extension = resourceTypeExtension(entry.first);
        result.push_back(std::move(entry.second));
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        const auto a = lower(left.extension), b = lower(right.extension);
        return a != b ? a < b : left.type < right.type;
    });
    return result;
}

std::vector<ResourceSelection> selectResourceType(
    const KeyBifArchive& archive, const LooseArchiveCatalog& loose, std::uint16_t type) {
    std::vector<ResourceSelection> result;
    if (!archive.isOpen()) return result;
    for (const auto& resource : archive.resources()) {
        if (resource.type == type) result.push_back({ResourceSource::KeyBif, resource.index});
    }
    for (const auto& resource : loose.resources()) {
        if (resource.type == type) result.push_back({ResourceSource::LooseArchive, resource.index});
    }
    return result;
}

} // namespace neobif
