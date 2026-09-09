#include "core/KeyBifArchive.hpp"
#include "core/ArchiveExport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neobif {
namespace {

constexpr std::size_t kKeyHeaderSize = 64u;
constexpr std::size_t kKeyBifEntrySize = 12u;
constexpr std::size_t kKeyResourceEntrySize = 22u;
constexpr std::size_t kBifHeaderSize = 20u;
constexpr std::size_t kBifVariableEntrySize = 16u;
constexpr std::uint32_t kMaximumTableRecords = 1000000u;
constexpr std::uint32_t kMaximumBifFiles = 4096u;
constexpr std::uint64_t kMaximumBrowserCandidateRecords = 500000u;
constexpr std::size_t kMaximumBrowserIndexedResources = 250000u;
constexpr std::size_t kMaximumIndexedResources = 1000000u;
constexpr std::size_t kMaximumBrowserCandidates = 8192u;
constexpr std::uint16_t kMaximumBifPathBytes = 4096u;
constexpr std::uint32_t kBrowserTableChunkRecords = 4096u;
constexpr std::size_t kYieldInterval = 4096u;
constexpr std::size_t kMaximumReportedIssues = 10000u;
constexpr std::size_t kMaximumCandidateReportedIssues = 256u;
constexpr std::uint64_t kMaximumNativeKeyBytes = 128u * 1024u * 1024u;

struct ResourceTypeName {
    std::uint16_t type;
    const char* extension;
};

constexpr ResourceTypeName kKotORResourceTypes[] = {
    {0x0000u, "res"}, {0x0001u, "bmp"}, {0x0002u, "mve"},
    {0x0003u, "tga"}, {0x0004u, "wav"}, {0x0006u, "plt"},
    {0x0007u, "ini"}, {0x0008u, "mp3"}, {0x0009u, "mpg"},
    {0x000Au, "txt"}, {0x000Bu, "wma"}, {0x000Cu, "wmv"},
    {0x000Du, "xmv"}, {0x000Eu, "log"}, {0x07D0u, "plh"},
    {0x07D1u, "tex"}, {0x07D2u, "mdl"}, {0x07D3u, "thg"},
    {0x07D5u, "fnt"}, {0x07D7u, "lua"}, {0x07D8u, "slt"},
    {0x07D9u, "nss"}, {0x07DAu, "ncs"}, {0x07DBu, "mod"},
    {0x07DCu, "are"}, {0x07DDu, "set"}, {0x07DEu, "ifo"},
    {0x07DFu, "bic"}, {0x07E0u, "wok"}, {0x07E1u, "2da"},
    {0x07E2u, "tlk"}, {0x07E6u, "txi"}, {0x07E7u, "git"},
    {0x07E8u, "bti"}, {0x07E9u, "uti"}, {0x07EAu, "btc"},
    {0x07EBu, "utc"}, {0x07EDu, "dlg"}, {0x07EEu, "itp"},
    {0x07EFu, "btt"}, {0x07F0u, "utt"}, {0x07F1u, "dds"},
    {0x07F2u, "bts"}, {0x07F3u, "uts"}, {0x07F4u, "ltr"},
    {0x07F5u, "gff"}, {0x07F6u, "fac"}, {0x07F7u, "bte"},
    {0x07F8u, "ute"}, {0x07F9u, "btd"}, {0x07FAu, "utd"},
    {0x07FBu, "btp"}, {0x07FCu, "utp"}, {0x07FDu, "dft"},
    {0x07FEu, "gic"}, {0x07FFu, "gui"}, {0x0800u, "css"},
    {0x0801u, "ccs"}, {0x0802u, "btm"}, {0x0803u, "utm"},
    {0x0804u, "dwk"}, {0x0805u, "pwk"}, {0x0806u, "btg"},
    {0x0807u, "utg"}, {0x0808u, "jrl"}, {0x0809u, "sav"},
    {0x080Au, "utw"}, {0x080Bu, "4pc"}, {0x080Cu, "ssf"},
    {0x080Du, "hak"}, {0x080Eu, "nwm"}, {0x080Fu, "bik"},
    {0x0BB8u, "lyt"}, {0x0BB9u, "vis"}, {0x0BBAu, "rim"},
    {0x0BBBu, "pth"}, {0x0BBCu, "lip"}, {0x0BBDu, "bwm"},
    {0x0BBEu, "txb"}, {0x0BBFu, "tpc"}, {0x0BC0u, "mdx"},
    {0x0BC1u, "rsv"}, {0x0BC2u, "sig"}, {0x0BC3u, "xbx"},
    {0x270Du, "erf"}, {0x270Eu, "bif"}, {0x270Fu, "key"},
};

struct KeyEntry {
    std::string resref;
    std::uint16_t type{};
    std::uint32_t id{};
};

struct BifVariableEntry {
    std::uint32_t id{};
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint32_t type{};
    bool boundsValid{};
};

struct ParsedBif {
    std::vector<BifVariableEntry> entries;
    std::unordered_map<std::uint32_t, std::size_t> idToIndex;
};

std::uint16_t readU16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1] << 8u);
}

std::uint32_t readU32(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8u) |
           (static_cast<std::uint32_t>(bytes[2]) << 16u) |
           (static_cast<std::uint32_t>(bytes[3]) << 24u);
}

bool checkedRange(std::uint64_t offset,
                  std::uint64_t length,
                  std::uint64_t total) {
    return offset <= total && length <= total - offset;
}

void appendArchiveIssue(std::vector<ArchiveIssue>& issues, ArchiveIssue issue) {
    if (issues.size() < kMaximumReportedIssues) {
        issues.push_back(std::move(issue));
    } else if (issues.size() == kMaximumReportedIssues) {
        issues.push_back({IssueSeverity::Warning,
                          "Additional archive issues were omitted after the reporting limit was reached",
                          std::nullopt,
                          std::nullopt});
    }
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string pathKey(const std::filesystem::path& path) {
    return lowerAscii(path.lexically_normal().generic_string());
}

std::string trimResref(const std::uint8_t* bytes, std::size_t length) {
    std::size_t end = 0;
    while (end < length && bytes[end] != 0u) ++end;
    while (end > 0 && (bytes[end - 1] == ' ' || bytes[end - 1] == '\t')) --end;
    return std::string(reinterpret_cast<const char*>(bytes), end);
}

std::filesystem::path portableRelativePath(std::string stored) {
    std::replace(stored.begin(), stored.end(), '\\', '/');
    if (stored.size() >= 2u && std::isalpha(static_cast<unsigned char>(stored[0])) &&
        stored[1] == ':') {
        stored.erase(0, 2);
    }
    while (!stored.empty() && stored.front() == '/') stored.erase(stored.begin());

    std::filesystem::path result;
    for (const auto& component : std::filesystem::path(stored)) {
        const std::string text = component.generic_string();
        if (text.empty() || text == ".") continue;
        if (text == "..") return {};
        result /= component;
    }
    return result;
}

std::optional<std::filesystem::path> resolveCaseInsensitive(
    const std::filesystem::path& root,
    const std::filesystem::path& relative) {
    std::error_code ec;
    std::filesystem::path current = root;
    for (const auto& component : relative) {
        const std::filesystem::path exact = current / component;
        if (std::filesystem::exists(exact, ec) && !ec) {
            current = exact;
            continue;
        }
        ec.clear();
        if (!std::filesystem::is_directory(current, ec) || ec) return std::nullopt;
        const std::string wanted = lowerAscii(component.generic_string());
        bool found = false;
        std::filesystem::path matched;
        for (std::filesystem::directory_iterator it(
                 current, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (lowerAscii(it->path().filename().generic_string()) == wanted) {
                if (found) return std::nullopt;
                matched = it->path();
                found = true;
            }
        }
        if (!found) return std::nullopt;
        current = matched;
    }
    if (!std::filesystem::is_regular_file(current, ec) || ec) return std::nullopt;
    return current;
}

void appendUniquePath(std::vector<std::filesystem::path>& paths,
                      const std::filesystem::path& path) {
    if (path.empty()) return;
    const std::string key = pathKey(path);
    if (std::none_of(paths.begin(), paths.end(), [&](const auto& existing) {
            return pathKey(existing) == key;
        })) {
        paths.push_back(path);
    }
}

std::filesystem::path resolveBifPath(
    const std::filesystem::path& keyPath, const std::string& storedPath,
    std::uint32_t, const std::vector<std::filesystem::path>& supplementaryFiles) {
    const auto relative = portableRelativePath(storedPath);
    if (relative.empty()) return {};
    if (auto direct = resolveCaseInsensitive(keyPath.parent_path(), relative)) return *direct;
    std::vector<std::filesystem::path> matches;
    if (auto local = resolveCaseInsensitive(keyPath.parent_path(), relative.filename())) appendUniquePath(matches, *local);
    for (const auto& file : supplementaryFiles) {
        std::error_code ec;
        if (pathKey(file.filename()) == pathKey(relative.filename()) &&
            std::filesystem::is_regular_file(file, ec) && !ec) appendUniquePath(matches, file);
    }
    // Multiple same-name candidates need an explicit KEY-entry-to-file mapping.
    return matches.size() == 1u ? matches.front() : std::filesystem::path{};
}

std::string sanitizeComponent(std::string value) {
    for (char& ch : value) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 0x20u || ch == '/' || ch == '\\' || ch == ':' || ch == '*' ||
            ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '.')) value.pop_back();
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    if (value.empty() || value == "." || value == "..") value = "unnamed";

    static const std::unordered_set<std::string> reserved = {
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
        "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4",
        "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    if (reserved.count(lowerAscii(value)) != 0u) value.insert(value.begin(), '_');
    return value;
}

std::string bifFolderName(const BifInfo& bif) {
    std::filesystem::path path = portableRelativePath(bif.storedPath);
    std::string value = path.stem().string();
    if (value.empty()) value = "bif_" + std::to_string(bif.index);
    return sanitizeComponent(value);
}

std::filesystem::path relativeOutputPath(const ResourceInfo& resource,
                                         const BifInfo& bif,
                                         ExtractionLayout layout) {
    const std::string file = sanitizeComponent(resource.resref) + "." +
                             sanitizeComponent(resource.extension);
    const std::string type = sanitizeComponent(resource.extension);
    const std::string bifName = bifFolderName(bif);
    switch (layout) {
    case ExtractionLayout::BifAndType:
        return std::filesystem::path(bifName) / type / file;
    case ExtractionLayout::Bif:
        return std::filesystem::path(bifName) / file;
    case ExtractionLayout::Type:
        return std::filesystem::path(type) / file;
    case ExtractionLayout::Flat:
        return std::filesystem::path(file);
    }
    return std::filesystem::path(file);
}

std::vector<std::filesystem::path> uniqueOutputPaths(
    const std::vector<std::size_t>& indices, const std::vector<ResourceInfo>& resources,
    const std::vector<BifInfo>& bifs, ExtractionLayout layout) {
    std::vector<std::filesystem::path> result;
    for (auto index : indices) {
        if (index >= resources.size() || resources[index].bifIndex >= bifs.size()) result.emplace_back();
        else result.push_back(relativeOutputPath(resources[index], bifs[resources[index].bifIndex], layout));
    }
    return result;
}

std::string joinStatus(const std::vector<std::string>& parts) {
    std::ostringstream stream;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) stream << "; ";
        stream << parts[i];
    }
    return stream.str();
}

void cooperativeYield(const BrowserYieldCallback& yield, std::size_t counter) {
    if (yield && counter != 0u && counter % kYieldInterval == 0u) yield();
}

bool buildResourceIndex(const std::vector<KeyEntry>& keyEntries,
                        std::vector<ParsedBif>& parsedBifs,
                        std::vector<BifInfo>& bifs,
                        std::vector<ResourceInfo>& resources,
                        std::vector<ArchiveIssue>& issues,
                        std::string& error,
                        const BrowserYieldCallback& yield = {},
                        std::size_t maximumResources = kMaximumIndexedResources) {
    resources.clear();
    error.clear();
    if (keyEntries.size() > maximumResources) {
        error = "KEY resource count exceeds the aggregate index limit";
        return false;
    }
    resources.reserve(keyEntries.size());
    std::vector<std::unordered_set<std::uint32_t>> referencedIds(bifs.size());
    std::unordered_map<std::string, std::uint32_t> nameTypeCounts;
    for (std::size_t keyIndex = 0; keyIndex < keyEntries.size(); ++keyIndex) {
        cooperativeYield(yield, keyIndex);
        const KeyEntry& key = keyEntries[keyIndex];
        ResourceInfo resource;
        resource.index = resources.size();
        resource.resref = key.resref;
        resource.type = key.type;
        resource.extension = resourceTypeExtension(key.type);
        resource.resourceId = key.id;
        resource.bifIndex = (key.id >> 20u) & 0x0FFFu;
        resource.tableIndex = key.id & 0x000FFFFFu;
        resource.engineIndex = key.id & 0x00003FFFu;

        std::vector<std::string> status;
        if (resource.bifIndex >= bifs.size()) {
            status.push_back("KEY resource references an out-of-range BIF index");
            appendArchiveIssue(issues, {IssueSeverity::Error, status.back(), std::nullopt, key.id});
        } else {
            BifInfo& bif = bifs[resource.bifIndex];
            ParsedBif& parsed = parsedBifs[resource.bifIndex];
            const BifVariableEntry* selected = nullptr;
            std::optional<std::size_t> selectedIndex;
            bool encodedIndexMismatch = false;
            if (resource.tableIndex < parsed.entries.size()) {
                const BifVariableEntry& candidate = parsed.entries[resource.tableIndex];
                if (candidate.id == key.id) {
                    selected = &candidate;
                    selectedIndex = resource.tableIndex;
                } else {
                    encodedIndexMismatch = true;
                }
            }
            if (selected == nullptr) {
                const auto exact = parsed.idToIndex.find(key.id);
                if (exact != parsed.idToIndex.end()) {
                    selected = &parsed.entries[exact->second];
                    selectedIndex = exact->second;
                }
            }
            if (selected != nullptr) {
                resource.bifEntryFound = true;
                resource.idMatches = true;
                resource.typeMatches = selected->type == key.type;
                resource.offset = selected->offset;
                resource.size = selected->size;
                resource.boundsValid = selected->boundsValid;
                resource.extractable = bif.available && bif.valid && resource.boundsValid && resource.typeMatches;
                referencedIds[resource.bifIndex].insert(selected->id);
                if (selectedIndex && *selectedIndex != resource.tableIndex) {
                    status.push_back("Resource ID was located at BIF table index " +
                                     std::to_string(*selectedIndex) +
                                     " instead of encoded index " +
                                     std::to_string(resource.tableIndex));
                    appendArchiveIssue(issues, {IssueSeverity::Warning, status.back(),
                                      resource.bifIndex, key.id});
                }
                if (!resource.typeMatches) {
                    status.push_back("Inconsistent BIF/KEY resource type; ordinary extraction disabled");
                    appendArchiveIssue(issues, {IssueSeverity::Error, status.back(), resource.bifIndex, key.id});
                }
                if (!resource.boundsValid) status.push_back("Payload overlaps BIF header/tables or lies outside BIF bounds");
            } else if (!bif.available) {
                status.push_back("BIF file is missing");
            } else {
                status.push_back(encodedIndexMismatch
                    ? "Encoded BIF table index points to a different resource ID and no exact ID match exists"
                    : "KEY resource has no matching BIF table entry");
                appendArchiveIssue(issues, {IssueSeverity::Error, status.back(), resource.bifIndex, key.id});
            }
            bif.resourceIndices.push_back(resource.index);
        }
        if (resource.tableIndex != resource.engineIndex) {
            status.push_back("Resource index exceeds the 14-bit index used by the KotOR loader");
            appendArchiveIssue(issues, {IssueSeverity::Warning, status.back(), resource.bifIndex, key.id});
        }
        if (status.empty()) status.push_back(resource.extractable ? "Ready" : "Unavailable");
        resource.status = joinStatus(status);

        const std::string duplicateKey = lowerAscii(resource.resref) + '#' +
                                         std::to_string(resource.type);
        const std::uint32_t duplicateCount = ++nameTypeCounts[duplicateKey];
        if (duplicateCount == 2u) {
            appendArchiveIssue(issues, {IssueSeverity::Warning,
                              "Duplicate resource name and type in KEY: " +
                                  resource.fileName(),
                              resource.bifIndex,
                              resource.resourceId});
        }
        resources.push_back(std::move(resource));
    }

    std::size_t scannedBifEntries = 0;
    for (std::size_t bifIndex = 0; bifIndex < parsedBifs.size(); ++bifIndex) {
        const ParsedBif& parsed = parsedBifs[bifIndex];
        BifInfo& bif = bifs[bifIndex];
        for (std::size_t entryIndex = 0; entryIndex < parsed.entries.size(); ++entryIndex) {
            cooperativeYield(yield, ++scannedBifEntries);
            const BifVariableEntry& entry = parsed.entries[entryIndex];
            if (referencedIds[bifIndex].count(entry.id) != 0u) continue;
            if (resources.size() >= maximumResources) {
                error = "Combined KEY and unindexed BIF resources exceed the aggregate index limit";
                resources.clear();
                return false;
            }
            ResourceInfo resource;
            resource.index = resources.size();
            resource.resref = "resource_" + hexResourceId(entry.id).substr(2);
            resource.type = static_cast<std::uint16_t>(entry.type & 0xFFFFu);
            resource.extension = resourceTypeExtension(resource.type);
            resource.resourceId = entry.id;
            resource.bifIndex = static_cast<std::uint32_t>(bifIndex);
            resource.tableIndex = static_cast<std::uint32_t>(entryIndex);
            resource.engineIndex = entry.id & 0x00003FFFu;
            resource.offset = entry.offset;
            resource.size = entry.size;
            resource.keyed = false;
            resource.bifEntryFound = true;
            resource.idMatches = true;
            resource.typeMatches = true;
            resource.boundsValid = entry.boundsValid;
            resource.extractable = bif.available && bif.valid && entry.boundsValid;
            resource.status = resource.extractable ? "Unindexed BIF resource; ready" :
                                                     "Unindexed BIF resource; unavailable";
            bif.resourceIndices.push_back(resource.index);
            resources.push_back(std::move(resource));
            appendArchiveIssue(issues, {IssueSeverity::Warning,
                              "BIF resource is not indexed by chitin.key",
                              bifIndex,
                              entry.id});
        }
    }
    return true;
}

} // namespace

std::string resourceTypeExtension(std::uint16_t type) {
    for (const auto& entry : kKotORResourceTypes) {
        if (entry.type == type) return entry.extension;
    }
    std::ostringstream stream;
    stream << "type_" << std::uppercase << std::hex << std::setw(4)
           << std::setfill('0') << type;
    return stream.str();
}

bool isKnownResourceExtension(const std::string& extension) {
    const auto value = lowerAscii(extension);
    return std::any_of(std::begin(kKotORResourceTypes), std::end(kKotORResourceTypes),
        [&value](const ResourceTypeName& entry) { return value == entry.extension; });
}

std::string resourceTypeLabel(std::uint16_t type) {
    std::ostringstream stream;
    stream << resourceTypeExtension(type) << " (0x" << std::uppercase << std::hex
           << std::setw(4) << std::setfill('0') << type << ')';
    return stream.str();
}

std::string formatByteSize(std::uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1u < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream stream;
    if (unit == 0u) stream << bytes;
    else stream << std::fixed << std::setprecision(value >= 100.0 ? 0 : (value >= 10.0 ? 1 : 2)) << value;
    stream << ' ' << units[unit];
    return stream.str();
}

std::string hexResourceId(std::uint32_t id) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << id;
    return stream.str();
}

std::string extractionLayoutName(ExtractionLayout layout) {
    switch (layout) {
    case ExtractionLayout::BifAndType: return "BIF and type";
    case ExtractionLayout::Bif: return "BIF";
    case ExtractionLayout::Type: return "type";
    case ExtractionLayout::Flat: return "flat";
    }
    return "unknown";
}

std::string ResourceInfo::fileName() const {
    return sanitizeComponent(resref) + "." + sanitizeComponent(extension);
}

void KeyBifArchive::clear() {
    keyPath_.clear();
    keySnapshot_ = {};
    buildYear_ = 0;
    buildDay_ = 0;
    bifs_.clear();
    resources_.clear();
    issues_.clear();
    lastError_.clear();
    open_ = false;
}

bool KeyBifArchive::open(const std::filesystem::path& keyPath,
                         const std::vector<std::filesystem::path>& supplementaryFiles,
                         const std::map<std::size_t, std::filesystem::path>& explicitRelocations,
                         const JobControl& job) {
    clear();
    keyPath_ = std::filesystem::absolute(keyPath).lexically_normal();
    job.check();
    try { keySnapshot_ = neoshared::erf::capture_regular_file_identity(keyPath_); }
    catch (const std::exception& ex) { lastError_ = ex.what(); return false; }

    std::ifstream keyStream(keyPath, std::ios::binary | std::ios::ate);
    if (!keyStream) {
        lastError_ = "Unable to open KEY file: " + keyPath.string();
        return false;
    }
    const std::streamoff keyLength = keyStream.tellg();
    if (keyLength < static_cast<std::streamoff>(kKeyHeaderSize)) {
        lastError_ = "KEY file is shorter than its 64-byte header";
        return false;
    }
    const std::uintmax_t keySize = static_cast<std::uintmax_t>(keyLength);
    if (keySize > std::numeric_limits<std::size_t>::max() ||
        keySize > kMaximumNativeKeyBytes) {
        lastError_ = "KEY file exceeds the 128 MiB safety limit";
        return false;
    }
    keyStream.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> keyBytes(static_cast<std::size_t>(keySize));
    keyStream.read(reinterpret_cast<char*>(keyBytes.data()), keyLength);
    if (!keyStream) {
        lastError_ = "Unable to read complete KEY file";
        return false;
    }

    if (std::memcmp(keyBytes.data(), "KEY ", 4) != 0) {
        lastError_ = "Not a KEY archive: expected KEY signature";
        return false;
    }
    if (std::memcmp(keyBytes.data() + 4, "V1  ", 4) != 0 &&
        std::memcmp(keyBytes.data() + 4, "V1.0", 4) != 0) {
        lastError_ = "Unsupported KEY version";
        return false;
    }

    const std::uint32_t bifCount = readU32(keyBytes.data() + 8);
    const std::uint32_t keyCount = readU32(keyBytes.data() + 12);
    const std::uint32_t bifTableOffset = readU32(keyBytes.data() + 16);
    const std::uint32_t keyTableOffset = readU32(keyBytes.data() + 20);
    buildYear_ = readU32(keyBytes.data() + 24);
    buildDay_ = readU32(keyBytes.data() + 28);

    if (bifCount > kMaximumBifFiles || keyCount > kMaximumTableRecords) {
        lastError_ = "KEY table count exceeds the safe record limit";
        return false;
    }
    if (!checkedRange(bifTableOffset,
                      static_cast<std::uint64_t>(bifCount) * kKeyBifEntrySize,
                      keyBytes.size()) ||
        !checkedRange(keyTableOffset,
                      static_cast<std::uint64_t>(keyCount) * kKeyResourceEntrySize,
                      keyBytes.size())) {
        lastError_ = "KEY table extends beyond the end of the file";
        return false;
    }

    bifs_.reserve(bifCount);
    for (std::uint32_t i = 0; i < bifCount; ++i) {
        const std::uint8_t* entry = keyBytes.data() + bifTableOffset +
                                    static_cast<std::size_t>(i) * kKeyBifEntrySize;
        BifInfo bif;
        bif.index = i;
        bif.declaredFileSize = readU32(entry);
        const std::uint32_t nameOffset = readU32(entry + 4);
        const std::uint16_t nameLength = readU16(entry + 8);
        bif.driveFlags = readU16(entry + 10);
        if (nameLength > kMaximumBifPathBytes ||
            !checkedRange(nameOffset, nameLength, keyBytes.size())) {
            bif.storedPath = "invalid_bif_" + std::to_string(i) + ".bif";
            bif.messages.push_back(nameLength > kMaximumBifPathBytes
                ? "BIF filename exceeds the safe path-length limit"
                : "BIF filename lies outside the KEY file");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), i, std::nullopt});
        } else {
            const std::uint8_t* nameBytes = keyBytes.data() + nameOffset;
            std::size_t actualLength = 0;
            while (actualLength < nameLength && nameBytes[actualLength] != 0u) ++actualLength;
            bif.storedPath.assign(reinterpret_cast<const char*>(nameBytes), actualLength);
        }
        const auto chosen = explicitRelocations.find(i);
        bif.resolvedPath = chosen != explicitRelocations.end()
            ? std::filesystem::absolute(chosen->second).lexically_normal()
            : resolveBifPath(keyPath_, bif.storedPath, bif.declaredFileSize, supplementaryFiles);
        if (chosen != explicitRelocations.end()) {
            bif.messages.push_back("BIF explicitly mapped by the user: " + bif.resolvedPath.string());
            appendArchiveIssue(issues_, {IssueSeverity::Warning, bif.messages.back(), i, std::nullopt});
        }
        bifs_.push_back(std::move(bif));
    }

    std::vector<KeyEntry> keyEntries;
    keyEntries.reserve(keyCount);
    for (std::uint32_t i = 0; i < keyCount; ++i) {
        const std::uint8_t* entry = keyBytes.data() + keyTableOffset +
                                    static_cast<std::size_t>(i) * kKeyResourceEntrySize;
        KeyEntry key;
        key.resref = trimResref(entry, 16);
        key.type = readU16(entry + 16);
        key.id = readU32(entry + 18);
        if (key.resref.empty()) key.resref = "unnamed_" + hexResourceId(key.id).substr(2);
        keyEntries.push_back(std::move(key));
    }

    std::vector<ParsedBif> parsedBifs(bifs_.size());
    std::uint64_t aggregateBifRecords = 0;
    for (std::size_t bifIndex = 0; bifIndex < bifs_.size(); ++bifIndex) {
        job.update(bifIndex, bifs_.size(), "Indexing BIF archives");
        BifInfo& bif = bifs_[bifIndex];
        if (bif.resolvedPath.empty()) {
            bif.messages.push_back("Referenced BIF is missing or ambiguous; use Relocate BIF to select the exact KEY entry and file");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        try { bif.snapshot = neoshared::erf::capture_regular_file_identity(bif.resolvedPath); }
        catch (const std::exception& ex) {
            bif.messages.push_back(ex.what());
            appendArchiveIssue(issues_, {IssueSeverity::Error, ex.what(), bifIndex, std::nullopt});
            continue;
        }
        std::ifstream stream(bif.resolvedPath, std::ios::binary | std::ios::ate);
        if (!stream) {
            bif.messages.push_back("Unable to open resolved BIF file");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        const std::streamoff length = stream.tellg();
        if (length < static_cast<std::streamoff>(kBifHeaderSize)) {
            bif.messages.push_back("BIF is shorter than its 20-byte header");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        bif.actualFileSize = static_cast<std::uintmax_t>(length);
        bif.available = true;
        if (bif.declaredFileSize != 0u && bif.actualFileSize != bif.declaredFileSize) {
            std::ostringstream message;
            message << "KEY declares " << bif.declaredFileSize << " bytes, file contains "
                    << bif.actualFileSize;
            bif.messages.push_back(message.str());
            appendArchiveIssue(issues_, {IssueSeverity::Warning, message.str(), bifIndex, std::nullopt});
        }
        stream.seekg(0, std::ios::beg);
        std::array<std::uint8_t, kBifHeaderSize> header{};
        stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (!stream || std::memcmp(header.data(), "BIFF", 4) != 0) {
            bif.messages.push_back("Not a BIFF archive");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        if (std::memcmp(header.data() + 4, "V1  ", 4) != 0 &&
            std::memcmp(header.data() + 4, "V1.0", 4) != 0) {
            bif.messages.push_back("Unsupported BIFF version");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        bif.variableResourceCount = readU32(header.data() + 8);
        bif.fixedResourceCount = readU32(header.data() + 12);
        bif.tableOffset = readU32(header.data() + 16);
        if (bif.variableResourceCount > kMaximumTableRecords || bif.fixedResourceCount > kMaximumTableRecords) {
            bif.messages.push_back("Variable resource count exceeds safe limit");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        if (bif.variableResourceCount >
            kMaximumBrowserCandidateRecords - aggregateBifRecords) {
            lastError_ = "Combined BIF tables exceed the aggregate indexing limit";
            return false;
        }
        aggregateBifRecords += bif.variableResourceCount;
        if (bif.tableOffset < kBifHeaderSize || !checkedRange(bif.tableOffset,
                          static_cast<std::uint64_t>(bif.variableResourceCount) *
                              kBifVariableEntrySize + static_cast<std::uint64_t>(bif.fixedResourceCount) * 20u,
                          bif.actualFileSize)) {
            bif.messages.push_back("Variable resource table extends beyond the BIF file");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }
        if (bif.fixedResourceCount != 0u) {
            bif.messages.push_back("Fixed-resource table is present; KotOR uses variable resources only, so fixed entries are reported but not extracted");
            appendArchiveIssue(issues_, {IssueSeverity::Warning, bif.messages.back(), bifIndex, std::nullopt});
        }

        ParsedBif& parsed = parsedBifs[bifIndex];
        parsed.entries.reserve(bif.variableResourceCount);
        stream.seekg(static_cast<std::streamoff>(bif.tableOffset), std::ios::beg);
        for (std::uint32_t entryIndex = 0; entryIndex < bif.variableResourceCount; ++entryIndex) {
            if (entryIndex % kYieldInterval == 0u) job.check();
            std::array<std::uint8_t, kBifVariableEntrySize> raw{};
            stream.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if (!stream) {
                bif.messages.push_back("Unable to read complete variable resource table");
                appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
                parsed.entries.clear();
                break;
            }
            BifVariableEntry entry;
            entry.id = readU32(raw.data());
            entry.offset = readU32(raw.data() + 4);
            entry.size = readU32(raw.data() + 8);
            entry.type = readU32(raw.data() + 12);
            const std::uint64_t payloadStart = bif.tableOffset +
                static_cast<std::uint64_t>(bif.variableResourceCount) * kBifVariableEntrySize +
                static_cast<std::uint64_t>(bif.fixedResourceCount) * 20u;
            entry.boundsValid = checkedRange(entry.offset, entry.size, bif.actualFileSize) &&
                (entry.size == 0u || entry.offset >= payloadStart);
            if (!entry.boundsValid) {
                appendArchiveIssue(issues_, {IssueSeverity::Error,
                                   "BIF resource overlaps the header/tables or extends beyond the file",
                                   bifIndex,
                                   entry.id});
            }
            if (!parsed.idToIndex.emplace(entry.id, parsed.entries.size()).second) {
                appendArchiveIssue(issues_, {IssueSeverity::Warning,
                                   "Duplicate resource ID in BIF variable table",
                                   bifIndex,
                                   entry.id});
            }
            parsed.entries.push_back(entry);
        }
        bif.valid = parsed.entries.size() == bif.variableResourceCount;
    }

    if (!unchangedInput(keyPath_, keySnapshot_)) { lastError_ = "KEY changed while indexing; rescan"; return false; }
    for (const auto& bif : bifs_) if (bif.available && !unchangedInput(bif.resolvedPath, bif.snapshot)) {
        lastError_ = "BIF changed while indexing; rescan: " + bif.resolvedPath.string(); return false;
    }
    if (!buildResourceIndex(keyEntries, parsedBifs, bifs_, resources_, issues_, lastError_, [&job] { job.check(); })) {
        return false;
    }

    open_ = true;
    return true;
}

bool KeyBifArchive::openBrowser(const BrowserArchiveFile& keyFile,
                                const std::vector<BrowserArchiveFile>& files,
                                const BrowserRangeReader& reader,
                                const BrowserYieldCallback& yield) {
    clear();
    keyPath_ = std::filesystem::path(keyFile.relativePath);
    if (!reader) {
        lastError_ = "Browser archive range reader is unavailable";
        return false;
    }
    if (keyFile.sessionId == 0 || keyFile.fileId == 0 || keyFile.relativePath.empty()) {
        lastError_ = "Browser KEY file identity is invalid";
        return false;
    }
    if (keyFile.size < kKeyHeaderSize) {
        lastError_ = "KEY file is shorter than its 64-byte header";
        return false;
    }
    const auto readExact = [&](const BrowserArchiveFile& file,
                               std::uint64_t offset,
                               std::size_t length,
                               std::vector<std::uint8_t>& bytes,
                               const char* description) {
        bytes.clear();
        if (length == 0u) return true;
        std::string readError;
        if (!reader(file, offset, length, bytes, readError) || bytes.size() != length) {
            lastError_ = readError.empty()
                ? std::string("Unable to read complete ") + description
                : std::move(readError);
            return false;
        }
        return true;
    };

    std::vector<std::uint8_t> keyHeader;
    if (!readExact(keyFile, 0u, kKeyHeaderSize, keyHeader, "KEY header")) return false;
    if (std::memcmp(keyHeader.data(), "KEY ", 4) != 0) {
        lastError_ = "Not a KEY archive: expected KEY signature";
        return false;
    }
    if (std::memcmp(keyHeader.data() + 4, "V1  ", 4) != 0 &&
        std::memcmp(keyHeader.data() + 4, "V1.0", 4) != 0) {
        lastError_ = "Unsupported KEY version";
        return false;
    }

    const std::uint32_t bifCount = readU32(keyHeader.data() + 8);
    const std::uint32_t keyCount = readU32(keyHeader.data() + 12);
    const std::uint32_t bifTableOffset = readU32(keyHeader.data() + 16);
    const std::uint32_t keyTableOffset = readU32(keyHeader.data() + 20);
    buildYear_ = readU32(keyHeader.data() + 24);
    buildDay_ = readU32(keyHeader.data() + 28);
    if (bifCount > kMaximumBifFiles || keyCount > kMaximumTableRecords) {
        lastError_ = "KEY table count exceeds the safe record limit";
        return false;
    }
    if (keyCount > kMaximumBrowserIndexedResources) {
        lastError_ = "KEY resource count exceeds the practical browser indexing limit";
        return false;
    }

    const std::size_t bifTableLength =
        static_cast<std::size_t>(bifCount) * kKeyBifEntrySize;
    const std::size_t keyTableLength =
        static_cast<std::size_t>(keyCount) * kKeyResourceEntrySize;
    if (!checkedRange(bifTableOffset, bifTableLength, keyFile.size) ||
        !checkedRange(keyTableOffset, keyTableLength, keyFile.size)) {
        lastError_ = "KEY table extends beyond the end of the file";
        return false;
    }

    std::vector<std::uint8_t> bifTableBytes;
    std::vector<std::uint8_t> keyTableBytes;
    if (!readExact(keyFile, bifTableOffset, bifTableLength,
                   bifTableBytes, "KEY BIF table") ||
        !readExact(keyFile, keyTableOffset, keyTableLength,
                   keyTableBytes, "KEY resource table")) {
        return false;
    }

    bifs_.reserve(bifCount);
    for (std::uint32_t index = 0; index < bifCount; ++index) {
        cooperativeYield(yield, index);
        const std::uint8_t* entry = bifTableBytes.data() +
            static_cast<std::size_t>(index) * kKeyBifEntrySize;
        BifInfo bif;
        bif.index = index;
        bif.declaredFileSize = readU32(entry);
        const std::uint32_t nameOffset = readU32(entry + 4);
        const std::uint16_t nameLength = readU16(entry + 8);
        bif.driveFlags = readU16(entry + 10);
        if (nameLength > kMaximumBifPathBytes ||
            !checkedRange(nameOffset, nameLength, keyFile.size)) {
            bif.storedPath = "invalid_bif_" + std::to_string(index) + ".bif";
            bif.messages.push_back(nameLength > kMaximumBifPathBytes
                ? "BIF filename exceeds the safe path-length limit"
                : "BIF filename lies outside the KEY file");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), index, std::nullopt});
        } else if (nameLength != 0u) {
            std::vector<std::uint8_t> nameBytes;
            if (!readExact(keyFile, nameOffset, nameLength, nameBytes, "BIF filename")) {
                return false;
            }
            std::size_t actualLength = 0;
            while (actualLength < nameBytes.size() && nameBytes[actualLength] != 0u) {
                ++actualLength;
            }
            bif.storedPath.assign(
                reinterpret_cast<const char*>(nameBytes.data()), actualLength);
        }
        if (bif.storedPath.empty()) {
            bif.storedPath = "unnamed_bif_" + std::to_string(index) + ".bif";
            bif.messages.push_back("BIF filename is empty");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), index, std::nullopt});
        }
        bifs_.push_back(std::move(bif));
    }

    std::vector<KeyEntry> keyEntries;
    keyEntries.reserve(keyCount);
    for (std::uint32_t index = 0; index < keyCount; ++index) {
        cooperativeYield(yield, index);
        const std::uint8_t* entry = keyTableBytes.data() +
            static_cast<std::size_t>(index) * kKeyResourceEntrySize;
        KeyEntry key;
        key.resref = trimResref(entry, 16);
        key.type = readU16(entry + 16);
        key.id = readU32(entry + 18);
        if (key.resref.empty()) key.resref = "unnamed_" + hexResourceId(key.id).substr(2);
        keyEntries.push_back(std::move(key));
    }

    struct CandidateIssue {
        IssueSeverity severity{IssueSeverity::Warning};
        std::string message;
        std::optional<std::uint32_t> resourceId;
    };
    struct Candidate {
        BrowserArchiveFile file;
        std::uint32_t variableCount{};
        std::uint32_t fixedCount{};
        std::uint32_t tableOffset{};
        std::optional<std::uint32_t> embeddedBifIndex;
        ParsedBif parsed;
        std::vector<CandidateIssue> issues;
        bool headerValid{};
        bool valid{};
        bool replacementSafe{};
        bool assigned{};
    };
    const auto appendCandidateIssue = [](Candidate& candidate, CandidateIssue issue) {
        if (candidate.issues.size() < kMaximumCandidateReportedIssues) {
            candidate.issues.push_back(std::move(issue));
        } else if (candidate.issues.size() == kMaximumCandidateReportedIssues) {
            candidate.issues.push_back({
                IssueSeverity::Warning,
                "Additional BIF candidate issues were omitted after the reporting limit was reached",
                std::nullopt});
        }
    };

    if (files.size() > kMaximumBrowserCandidates) {
        lastError_ = "The browser selection contains too many archive candidates";
        return false;
    }
    std::vector<Candidate> candidates;
    std::uint64_t aggregateCandidateRecords = 0;
    for (const BrowserArchiveFile& file : files) {
        if (file.sessionId == 0 || file.fileId == 0 || file.relativePath.empty() ||
            lowerAscii(std::filesystem::path(file.relativePath).extension().string()) != ".bif") {
            continue;
        }
        Candidate candidate;
        candidate.file = file;
        if (file.size < kBifHeaderSize) {
            appendCandidateIssue(candidate, {IssueSeverity::Error,
                "BIF is shorter than its 20-byte header", std::nullopt});
            candidates.push_back(std::move(candidate));
            continue;
        }

        std::vector<std::uint8_t> header;
        std::string readError;
        if (!reader(file, 0, kBifHeaderSize, header, readError) ||
            header.size() != kBifHeaderSize) {
            appendCandidateIssue(candidate, {IssueSeverity::Error,
                readError.empty() ? "Unable to read BIF header" : readError, std::nullopt});
            candidates.push_back(std::move(candidate));
            continue;
        }
        if (std::memcmp(header.data(), "BIFF", 4) != 0) {
            appendCandidateIssue(candidate, {IssueSeverity::Error, "Not a BIFF archive", std::nullopt});
            candidates.push_back(std::move(candidate));
            continue;
        }
        if (std::memcmp(header.data() + 4, "V1  ", 4) != 0 &&
            std::memcmp(header.data() + 4, "V1.0", 4) != 0) {
            appendCandidateIssue(candidate, {IssueSeverity::Error, "Unsupported BIFF version", std::nullopt});
            candidates.push_back(std::move(candidate));
            continue;
        }
        candidate.headerValid = true;
        candidate.variableCount = readU32(header.data() + 8);
        candidate.fixedCount = readU32(header.data() + 12);
        candidate.tableOffset = readU32(header.data() + 16);
        if (candidate.variableCount > kMaximumTableRecords || candidate.fixedCount > kMaximumTableRecords) {
            appendCandidateIssue(candidate, {IssueSeverity::Error,
                "Variable resource count exceeds safe limit", std::nullopt});
            candidates.push_back(std::move(candidate));
            continue;
        }
        if (candidate.variableCount >
            kMaximumBrowserCandidateRecords - aggregateCandidateRecords) {
            lastError_ = "Combined browser BIF candidate tables exceed the aggregate indexing limit";
            return false;
        }
        aggregateCandidateRecords += candidate.variableCount;
        const std::uint64_t tableLength =
            static_cast<std::uint64_t>(candidate.variableCount) * kBifVariableEntrySize;
        if (candidate.tableOffset < kBifHeaderSize || !checkedRange(candidate.tableOffset, tableLength + static_cast<std::uint64_t>(candidate.fixedCount) * 20u, file.size)) {
            appendCandidateIssue(candidate, {IssueSeverity::Error,
                "Variable resource table extends beyond the BIF file", std::nullopt});
            candidates.push_back(std::move(candidate));
            continue;
        }
        if (candidate.fixedCount != 0u) {
            appendCandidateIssue(candidate, {IssueSeverity::Warning,
                "Fixed-resource table is present; KotOR uses variable resources only, so fixed entries are reported but not extracted",
                std::nullopt});
        }

        candidate.parsed.entries.reserve(candidate.variableCount);
        std::optional<std::uint32_t> commonBifIndex;
        bool commonIndex = true;
        bool tableComplete = true;
        bool payloadBoundsValid = true;
        for (std::uint32_t chunkStart = 0; chunkStart < candidate.variableCount;
             chunkStart += kBrowserTableChunkRecords) {
            const std::uint32_t chunkRecords = std::min(
                kBrowserTableChunkRecords, candidate.variableCount - chunkStart);
            const std::size_t chunkBytes =
                static_cast<std::size_t>(chunkRecords) * kBifVariableEntrySize;
            const std::uint64_t chunkOffset = candidate.tableOffset +
                static_cast<std::uint64_t>(chunkStart) * kBifVariableEntrySize;
            std::vector<std::uint8_t> tableChunk;
            readError.clear();
            if (!reader(file, chunkOffset, chunkBytes, tableChunk, readError) ||
                tableChunk.size() != chunkBytes) {
                appendCandidateIssue(candidate, {IssueSeverity::Error,
                    readError.empty() ? "Unable to read complete variable resource table" : readError,
                    std::nullopt});
                tableComplete = false;
                break;
            }
            for (std::uint32_t chunkIndex = 0; chunkIndex < chunkRecords; ++chunkIndex) {
                const std::uint32_t entryIndex = chunkStart + chunkIndex;
                cooperativeYield(yield, entryIndex);
                const std::uint8_t* raw = tableChunk.data() +
                    static_cast<std::size_t>(chunkIndex) * kBifVariableEntrySize;
                BifVariableEntry entry;
                entry.id = readU32(raw);
                entry.offset = readU32(raw + 4);
                entry.size = readU32(raw + 8);
                entry.type = readU32(raw + 12);
                entry.boundsValid = checkedRange(entry.offset, entry.size, file.size) &&
                    (entry.size == 0u || entry.offset >= candidate.tableOffset + tableLength + static_cast<std::uint64_t>(candidate.fixedCount) * 20u);
                if (!entry.boundsValid) {
                    payloadBoundsValid = false;
                    appendCandidateIssue(candidate, {IssueSeverity::Error,
                        "BIF resource overlaps the header/tables or extends beyond the file", entry.id});
                }
                if (!candidate.parsed.idToIndex.emplace(
                        entry.id, candidate.parsed.entries.size()).second) {
                    appendCandidateIssue(candidate, {IssueSeverity::Warning,
                        "Duplicate resource ID in BIF variable table", entry.id});
                }
                const std::uint32_t encodedBifIndex = (entry.id >> 20u) & 0x0FFFu;
                if (!commonBifIndex) commonBifIndex = encodedBifIndex;
                else if (*commonBifIndex != encodedBifIndex) commonIndex = false;
                candidate.parsed.entries.push_back(entry);
            }
        }
        if (commonIndex) candidate.embeddedBifIndex = commonBifIndex;
        candidate.valid = tableComplete &&
            candidate.parsed.entries.size() == candidate.variableCount;
        candidate.replacementSafe = candidate.valid && payloadBoundsValid;
        candidates.push_back(std::move(candidate));
    }

    const std::filesystem::path keyParent =
        std::filesystem::path(keyFile.relativePath).parent_path();
    std::vector<ParsedBif> parsedBifs(bifs_.size());
    for (std::size_t bifIndex = 0; bifIndex < bifs_.size(); ++bifIndex) {
        BifInfo& bif = bifs_[bifIndex];
        const std::filesystem::path stored = portableRelativePath(bif.storedPath);
        const std::filesystem::path expected = (keyParent / stored).lexically_normal();
        const std::string expectedKey = pathKey(expected);
        const std::string storedKey = pathKey(stored);
        const std::string leafKey = pathKey(stored.filename());
        int bestScore = 0;
        int bestIdentityRank = -1;
        int bestSafetyRank = -1;
        std::optional<std::size_t> bestCandidate;
        bool tied = false;
        for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            Candidate& candidate = candidates[candidateIndex];
            if (candidate.assigned) continue;
            const std::filesystem::path candidatePath(candidate.file.relativePath);
            const std::string candidateKey = pathKey(candidatePath);
            const std::string candidateLeaf = pathKey(candidatePath.filename());
            int identityScore = 0;
            if (!expectedKey.empty() && candidateKey == expectedKey) identityScore += 10000;
            if (!storedKey.empty() && candidateKey == storedKey) identityScore += 9000;
            if (!leafKey.empty() && candidateLeaf == leafKey) identityScore += 5000;
            if (candidate.file.explicitBifIndex) {
                identityScore = *candidate.file.explicitBifIndex == bifIndex ? 100000 : 0;
            }
            if (identityScore == 0) continue;
            const int score = identityScore;
            const int identityRank = identityScore != 0 ? 1 : 0;
            const int safetyRank = candidate.replacementSafe ? 2 : (candidate.valid ? 1 : 0);
            // Archive validity is the primary ordering criterion.  An exact
            // filename match must never displace a structurally safe BIF with
            // a malformed or out-of-bounds candidate.  Identity and explicit
            // replacement preference are considered only among candidates at
            // the same safety level.
            if (score != 0 &&
                (score > bestScore ||
                 (score == bestScore && safetyRank > bestSafetyRank))) {
                bestIdentityRank = identityRank;
                bestSafetyRank = safetyRank;
                bestScore = score;
                bestCandidate = candidateIndex;
                tied = false;
            } else if (score != 0 && safetyRank == bestSafetyRank &&
                       identityRank == bestIdentityRank && score == bestScore) {
                tied = true;
            }
        }

        if (!bestCandidate || bestScore == 0 || tied) {
            bif.messages.push_back(tied
                ? "Referenced BIF file matched multiple selected browser files"
                : "Referenced BIF is missing or ambiguous; use Relocate BIF to select the exact KEY entry and file");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
            continue;
        }

        Candidate& candidate = candidates[*bestCandidate];
        candidate.assigned = true;
        bif.browserBacked = true;
        bif.browserSessionId = candidate.file.sessionId;
        bif.browserFileId = candidate.file.fileId;
        bif.browserRelativePath = candidate.file.relativePath;
        bif.resolvedPath = std::filesystem::path(candidate.file.relativePath);
        bif.actualFileSize = candidate.file.size;
        bif.available = true;
        bif.variableResourceCount = candidate.variableCount;
        bif.fixedResourceCount = candidate.fixedCount;
        bif.tableOffset = candidate.tableOffset;
        bif.valid = candidate.valid;
        if (bif.declaredFileSize != 0u && bif.actualFileSize != bif.declaredFileSize) {
            std::ostringstream message;
            message << "KEY declares " << bif.declaredFileSize << " bytes, file contains "
                    << bif.actualFileSize;
            bif.messages.push_back(message.str());
            appendArchiveIssue(issues_, {IssueSeverity::Warning, message.str(), bifIndex, std::nullopt});
        }
        for (const CandidateIssue& issue : candidate.issues) {
            bif.messages.push_back(issue.message);
            appendArchiveIssue(issues_, {issue.severity, issue.message, bifIndex, issue.resourceId});
        }
        parsedBifs[bifIndex] = std::move(candidate.parsed);
    }

    if (!buildResourceIndex(
            keyEntries, parsedBifs, bifs_, resources_, issues_, lastError_, yield,
            kMaximumBrowserIndexedResources)) {
        return false;
    }
    open_ = true;
    return true;
}

std::size_t KeyBifArchive::extractableResourceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        resources_.begin(), resources_.end(),
        [](const ResourceInfo& resource) { return resource.extractable; }));
}

std::size_t KeyBifArchive::missingBifCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        bifs_.begin(), bifs_.end(),
        [](const BifInfo& bif) { return !bif.available; }));
}

bool KeyBifArchive::streamResource(std::size_t index, const ByteSink& sink,
                                  std::string& error, const JobControl& job) const {
    error.clear();
    if (index >= resources_.size()) { error="Resource index is out of range"; return false; }
    const auto& resource=resources_[index];
    if (!resource.extractable || resource.bifIndex>=bifs_.size()) {
        error="Resource is not extractable: "+resource.status;return false;
    }
    const auto& bif=bifs_[resource.bifIndex];
    if (bif.browserBacked) { error="Use retained browser range access";return false; }
    if (!unchangedInput(keyPath_,keySnapshot_)) { error="KEY changed after indexing; rescan before extracting";return false; }
    if (!streamInputRange(bif.resolvedPath,bif.snapshot,resource.offset,resource.size,sink,error,job))return false;
    if (!unchangedInput(keyPath_,keySnapshot_)) { error="KEY changed during extraction; rescan";return false; }
    return true;
}
bool KeyBifArchive::readResource(std::size_t index, std::vector<std::uint8_t>& bytes, std::string& error) const {
    bytes.clear();
    const bool ok=streamResource(index,[&](const std::uint8_t* data,std::size_t size,std::string&) {
        bytes.insert(bytes.end(),data,data+size);return true;
    },error);
    if(!ok) bytes.clear();
    return ok;
}
std::vector<std::filesystem::path> KeyBifArchive::inputPaths() const {
    std::vector<std::filesystem::path> paths{keyPath_};
    for(const auto& bif:bifs_)if(!bif.resolvedPath.empty())paths.push_back(bif.resolvedPath);
    return paths;
}
namespace {
std::vector<ExportItem> bifExportItems(const KeyBifArchive& archive,const std::vector<std::size_t>& indices,ExtractionLayout layout) {
    const auto paths=archive.outputPaths(indices,layout);std::vector<ExportItem> items;
    for(std::size_t pos=0;pos<indices.size();++pos) {
        const auto index=indices[pos];ExportItem item;
        item.relativePath=paths[pos];item.sourcePaths.push_back(archive.keyPath());
        if(index<archive.resources().size()) {
            item.displayName=archive.resources()[index].fileName();item.expectedSize=archive.resources()[index].size;
            if(archive.resources()[index].bifIndex<archive.bifs().size())item.sourcePaths.push_back(archive.bifs()[archive.resources()[index].bifIndex].resolvedPath);
        }
        item.stream=[&archive,index](const ByteSink& sink,std::string& error,const JobControl& job) {return archive.streamResource(index,sink,error,job);};
        items.push_back(std::move(item));
    }
    return items;
}
}
ExtractionReport KeyBifArchive::extractResources(const std::vector<std::size_t>& indices,
    const std::filesystem::path& root,ExtractionLayout layout,bool overwrite) const {
    ExportOptions options;options.protectedInputs=inputPaths();options.existing=overwrite?ExistingPolicy::Replace:ExistingPolicy::Skip;
    return extractExportItems(bifExportItems(*this,indices,layout),root,options);
}
std::vector<std::filesystem::path> KeyBifArchive::outputPaths(const std::vector<std::size_t>& indices,ExtractionLayout layout) const {
    return uniqueOutputPaths(indices,resources_,bifs_,layout);
}
bool KeyBifArchive::writeZip(const std::vector<std::size_t>& indices,const std::filesystem::path& path,
                             ExtractionLayout layout,std::string& error) const {
    ExportOptions options;options.protectedInputs=inputPaths();
    return writeExportZip(bifExportItems(*this,indices,layout),path,error,options);
}

std::vector<std::filesystem::path> KeyBifArchive::scanForKeyFiles(
    const std::filesystem::path& root,
    std::size_t maximumResults, const JobControl& job) {
    std::vector<std::filesystem::path> results;
    std::error_code ec;
    if (std::filesystem::is_regular_file(root, ec) && !ec) {
        if (lowerAscii(root.filename().string()) == "chitin.key") results.push_back(root);
        return results;
    }
    ec.clear();
    if (!std::filesystem::is_directory(root, ec) || ec) return results;

    const std::filesystem::path direct = root / "chitin.key";
    if (std::filesystem::is_regular_file(direct, ec) && !ec) results.push_back(direct);
    ec.clear();
    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && results.size() < maximumResults; it.increment(ec)) {
        job.check();
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (lowerAscii(it->path().filename().string()) == "chitin.key") {
            if (std::find(results.begin(), results.end(), it->path()) == results.end()) {
                results.push_back(it->path());
            }
        }
    }
    std::sort(results.begin(), results.end(), [](const auto& left, const auto& right) {
        const auto leftDepth = std::distance(left.begin(), left.end());
        const auto rightDepth = std::distance(right.begin(), right.end());
        if (leftDepth != rightDepth) return leftDepth < rightDepth;
        return left.generic_string() < right.generic_string();
    });
    return results;
}

} // namespace neobif
