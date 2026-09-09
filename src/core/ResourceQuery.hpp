#pragma once

#include "core/KeyBifArchive.hpp"
#include "core/LooseArchiveCatalog.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace neobif {

// Shared by tree selection and session-wide exports. A resource in a BIF and
// one in a MOD with the same numeric index are still distinct resources.
enum class ResourceSource { KeyBif, LooseArchive };
struct ResourceSelection {
    ResourceSource source{ResourceSource::KeyBif};
    std::size_t resourceIndex{std::numeric_limits<std::size_t>::max()};
};

struct ResourceTypeSummary {
    std::uint16_t type{};
    std::string extension;
    std::size_t total{};
    std::size_t available{};
};

// Small search grammar: text fragments, exact extensions (.tpc / tpc /
// *.tpc), and filename wildcards (* / ?). No regular expressions or disk I/O.
class ResourceQuery final {
public:
    explicit ResourceQuery(std::string text);
    bool empty() const noexcept { return pattern_.empty() && mode_ == Mode::Text; }
    bool isTypeQuery() const noexcept { return mode_ == Mode::Extension; }
    const std::string& extension() const noexcept { return pattern_; }
    bool matchesContext(std::string_view text) const;
    bool matches(std::string_view fileName, std::string_view extension,
                 std::initializer_list<std::string_view> metadata = {}) const;

private:
    enum class Mode { Text, Extension, FilenameGlob };
    Mode mode_{Mode::Text};
    std::string pattern_;
};

// Operate on the entire currently indexed KEY/game session, not displayed
// nodes, tree pages, filters or selections. Include unavailable resources so
// that the existing exporter must disclose any omission before proceeding.
std::vector<ResourceTypeSummary> summarizeResourceTypes(
    const KeyBifArchive& archive, const LooseArchiveCatalog& loose);
std::vector<ResourceSelection> selectResourceType(
    const KeyBifArchive& archive, const LooseArchiveCatalog& loose,
    std::uint16_t type);

} // namespace neobif
