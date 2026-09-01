#include "core/KeyBifArchive.hpp"

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
constexpr std::uint16_t kZipDosTime = 0x0000u;
constexpr std::uint16_t kZipDosDate = 0x0021u; // 1980-01-01

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
        for (std::filesystem::directory_iterator it(
                 current, std::filesystem::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (lowerAscii(it->path().filename().generic_string()) == wanted) {
                current = it->path();
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(current, ec) || ec) return std::nullopt;
    return current;
}

bool hasUsableBifStructure(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamoff length = stream.tellg();
    if (length < static_cast<std::streamoff>(kBifHeaderSize)) return false;
    stream.seekg(0, std::ios::beg);
    std::array<std::uint8_t, kBifHeaderSize> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    if (!stream || std::memcmp(header.data(), "BIFF", 4) != 0) return false;
    if (std::memcmp(header.data() + 4, "V1  ", 4) != 0 &&
        std::memcmp(header.data() + 4, "V1.0", 4) != 0) {
        return false;
    }
    const std::uint32_t variableCount = readU32(header.data() + 8);
    const std::uint32_t tableOffset = readU32(header.data() + 16);
    if (variableCount > kMaximumTableRecords ||
        !checkedRange(
            tableOffset,
            static_cast<std::uint64_t>(variableCount) * kBifVariableEntrySize,
            static_cast<std::uint64_t>(length))) {
        return false;
    }

    constexpr std::size_t kValidationChunkRecords = 4096u;
    std::vector<std::uint8_t> bytes(kValidationChunkRecords * kBifVariableEntrySize);
    std::uint32_t parsed = 0u;
    while (parsed < variableCount) {
        const std::uint32_t count = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(kValidationChunkRecords), variableCount - parsed);
        const std::size_t byteCount = static_cast<std::size_t>(count) * kBifVariableEntrySize;
        stream.seekg(static_cast<std::streamoff>(
            static_cast<std::uint64_t>(tableOffset) +
            static_cast<std::uint64_t>(parsed) * kBifVariableEntrySize), std::ios::beg);
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(byteCount));
        if (!stream) return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uint8_t* entry = bytes.data() +
                static_cast<std::size_t>(index) * kBifVariableEntrySize;
            if (!checkedRange(readU32(entry + 4), readU32(entry + 8),
                              static_cast<std::uint64_t>(length))) {
                return false;
            }
        }
        parsed += count;
    }
    return true;
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
    const std::filesystem::path& keyPath,
    const std::string& storedPath,
    std::uint32_t declaredSize,
    const std::vector<std::filesystem::path>& supplementaryFiles) {
    const std::filesystem::path relative = portableRelativePath(storedPath);
    const std::filesystem::path leafName = relative.filename();
    const std::string relativeKey = pathKey(relative);
    const std::string leafKey = pathKey(leafName);
    std::vector<std::filesystem::path> explicitMatches;
    std::vector<std::filesystem::path> automaticMatches;
    std::vector<std::filesystem::path> sizeMatches;

    // Newest explicit selection wins and explicit relocation outranks a stale
    // KEY-relative file. Structurally valid candidates are preferred so a bad
    // replacement cannot discard a working archive mapping.
    for (auto it = supplementaryFiles.rbegin(); it != supplementaryFiles.rend(); ++it) {
        const std::filesystem::path& file = *it;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec) || ec) continue;
        const std::string fileKey = pathKey(file);
        const std::string fileLeafKey = pathKey(file.filename());
        if ((!relativeKey.empty() && fileKey == relativeKey) ||
            (!leafKey.empty() && fileLeafKey == leafKey)) {
            appendUniquePath(explicitMatches, file);
            continue;
        }
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        if (!ec && declaredSize != 0u && size == declaredSize) {
            appendUniquePath(sizeMatches, file);
        }
    }

    if (!relative.empty()) {
        const std::filesystem::path direct = keyPath.parent_path() / relative;
        std::error_code ec;
        if (std::filesystem::is_regular_file(direct, ec) && !ec) {
            appendUniquePath(automaticMatches, direct);
        }
        if (auto resolved = resolveCaseInsensitive(keyPath.parent_path(), relative)) {
            appendUniquePath(automaticMatches, *resolved);
        }
    }
    if (!leafName.empty()) {
        const std::filesystem::path direct = keyPath.parent_path() / leafName;
        std::error_code ec;
        if (std::filesystem::is_regular_file(direct, ec) && !ec) {
            appendUniquePath(automaticMatches, direct);
        }
    }

    std::vector<std::filesystem::path> candidates;
    for (const auto& path : explicitMatches) appendUniquePath(candidates, path);
    for (const auto& path : automaticMatches) appendUniquePath(candidates, path);
    if (sizeMatches.size() == 1u) appendUniquePath(candidates, sizeMatches.front());
    for (const auto& path : candidates) {
        if (hasUsableBifStructure(path)) return path;
    }
    return candidates.empty() ? std::filesystem::path{} : candidates.front();
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
    const std::vector<std::size_t>& indices,
    const std::vector<ResourceInfo>& resources,
    const std::vector<BifInfo>& bifs,
    ExtractionLayout layout) {
    std::vector<std::filesystem::path> result;
    result.reserve(indices.size());
    std::unordered_set<std::string> used;
    for (const std::size_t index : indices) {
        if (index >= resources.size()) {
            result.emplace_back();
            continue;
        }
        const ResourceInfo& resource = resources[index];
        if (resource.bifIndex >= bifs.size()) {
            result.emplace_back();
            continue;
        }
        std::filesystem::path candidate = relativeOutputPath(resource, bifs[resource.bifIndex], layout);
        std::string key = lowerAscii(candidate.generic_string());
        if (used.insert(key).second) {
            result.push_back(candidate);
            continue;
        }

        const std::string suffix = "__" + hexResourceId(resource.resourceId).substr(2);
        const std::filesystem::path parent = candidate.parent_path();
        const std::string stem = candidate.stem().string();
        const std::string extension = candidate.extension().string();
        candidate = parent / (stem + suffix + extension);
        key = lowerAscii(candidate.generic_string());
        unsigned ordinal = 2u;
        while (!used.insert(key).second) {
            candidate = parent / (stem + suffix + "_" + std::to_string(ordinal++) + extension);
            key = lowerAscii(candidate.generic_string());
        }
        result.push_back(candidate);
    }
    return result;
}

bool writeAtomic(const std::filesystem::path& output,
                 const std::vector<std::uint8_t>& bytes,
                 bool overwrite,
                 std::string& error) {
    std::error_code ec;
    if (!overwrite && std::filesystem::exists(output, ec) && !ec) {
        error = "Output already exists";
        return false;
    }
    std::filesystem::create_directories(output.parent_path(), ec);
    if (ec) {
        error = "Unable to create output directory: " + ec.message();
        return false;
    }
    const std::filesystem::path temporary = output.parent_path() /
        ("." + output.filename().string() + ".neobif.tmp");
    std::filesystem::remove(temporary, ec);
    ec.clear();
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "Unable to create temporary output";
            return false;
        }
        if (!bytes.empty()) {
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        stream.flush();
        if (!stream) {
            error = "Unable to write output bytes";
            stream.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    if (overwrite) {
        std::filesystem::remove(output, ec);
        ec.clear();
    }
    std::filesystem::rename(temporary, output, ec);
    if (ec) {
        error = "Unable to finalize output: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
            }
            values[i] = value;
        }
        return values;
    }();

    std::uint32_t value = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        value = table[(value ^ data[i]) & 0xFFu] ^ (value >> 8u);
    }
    return value ^ 0xFFFFFFFFu;
}

void writeU16(std::ostream& stream, std::uint16_t value) {
    const char bytes[2] = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8u) & 0xFFu)};
    stream.write(bytes, 2);
}

void writeU32(std::ostream& stream, std::uint32_t value) {
    const char bytes[4] = {
        static_cast<char>(value & 0xFFu),
        static_cast<char>((value >> 8u) & 0xFFu),
        static_cast<char>((value >> 16u) & 0xFFu),
        static_cast<char>((value >> 24u) & 0xFFu)};
    stream.write(bytes, 4);
}

struct ZipCentralEntry {
    std::string name;
    std::uint32_t crc{};
    std::uint32_t size{};
    std::uint32_t localOffset{};
};

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
                resource.extractable = bif.available && bif.valid && resource.boundsValid;
                referencedIds[resource.bifIndex].insert(selected->id);
                if (selectedIndex && *selectedIndex != resource.tableIndex) {
                    status.push_back("Resource ID was located at BIF table index " +
                                     std::to_string(*selectedIndex) +
                                     " instead of encoded index " +
                                     std::to_string(resource.tableIndex));
                    appendArchiveIssue(issues, {IssueSeverity::Warning, status.back(),
                                      resource.bifIndex, key.id});
                }
                if (!resource.typeMatches) status.push_back("BIF type does not match KEY type");
                if (!resource.boundsValid) status.push_back("Payload lies outside BIF bounds");
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
    buildYear_ = 0;
    buildDay_ = 0;
    bifs_.clear();
    resources_.clear();
    issues_.clear();
    lastError_.clear();
    open_ = false;
}

bool KeyBifArchive::open(const std::filesystem::path& keyPath,
                         const std::vector<std::filesystem::path>& supplementaryFiles) {
    clear();
    keyPath_ = keyPath;

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
        bif.resolvedPath = resolveBifPath(
            keyPath, bif.storedPath, bif.declaredFileSize, supplementaryFiles);
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
        BifInfo& bif = bifs_[bifIndex];
        if (bif.resolvedPath.empty()) {
            bif.messages.push_back("Referenced BIF file was not found");
            appendArchiveIssue(issues_, {IssueSeverity::Error, bif.messages.back(), bifIndex, std::nullopt});
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
        if (bif.variableResourceCount > kMaximumTableRecords) {
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
        if (!checkedRange(bif.tableOffset,
                          static_cast<std::uint64_t>(bif.variableResourceCount) *
                              kBifVariableEntrySize,
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
            entry.boundsValid = checkedRange(entry.offset, entry.size, bif.actualFileSize);
            if (!entry.boundsValid) {
                appendArchiveIssue(issues_, {IssueSeverity::Error,
                                   "BIF resource payload extends beyond the file",
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

    if (!buildResourceIndex(keyEntries, parsedBifs, bifs_, resources_, issues_, lastError_)) {
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
        if (candidate.variableCount > kMaximumTableRecords) {
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
        if (!checkedRange(candidate.tableOffset, tableLength, file.size)) {
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
                entry.boundsValid = checkedRange(entry.offset, entry.size, file.size);
                if (!entry.boundsValid) {
                    payloadBoundsValid = false;
                    appendCandidateIssue(candidate, {IssueSeverity::Error,
                        "BIF resource payload extends beyond the file", entry.id});
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
        const std::string storedStem = lowerAscii(stored.stem().string());
        const std::string storedExtension = lowerAscii(stored.extension().string());

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
            const std::string candidateStem = lowerAscii(candidatePath.stem().string());
            const std::string candidateExtension = lowerAscii(candidatePath.extension().string());
            if (!storedStem.empty() && candidateExtension == storedExtension &&
                candidateStem.size() > storedStem.size() &&
                candidateStem.compare(0, storedStem.size(), storedStem) == 0 &&
                candidateStem[storedStem.size()] == '(') {
                identityScore += 4000;
            }
            if (candidate.embeddedBifIndex && *candidate.embeddedBifIndex == bifIndex) {
                identityScore += 3000;
            }
            int score = identityScore;
            if (bif.declaredFileSize != 0u && candidate.file.size == bif.declaredFileSize) {
                score += 1000;
            }
            // A BIF explicitly added through "Add or Relocate" should replace
            // an older match when it has an archive identity signal of its own.
            // Do not boost a file that matches by size alone.
            if (candidate.file.preferred && identityScore >= 3000) score += 50000;
            const int identityRank = identityScore != 0 ? 1 : 0;
            const int safetyRank = candidate.replacementSafe ? 2 : (candidate.valid ? 1 : 0);
            // Archive validity is the primary ordering criterion.  An exact
            // filename match must never displace a structurally safe BIF with
            // a malformed or out-of-bounds candidate.  Identity and explicit
            // replacement preference are considered only among candidates at
            // the same safety level.
            if (score != 0 &&
                (safetyRank > bestSafetyRank ||
                 (safetyRank == bestSafetyRank && identityRank > bestIdentityRank) ||
                 (safetyRank == bestSafetyRank && identityRank == bestIdentityRank &&
                  score > bestScore))) {
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
                : "Referenced BIF file was not found");
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

bool KeyBifArchive::readResource(std::size_t resourceIndex,
                                 std::vector<std::uint8_t>& bytes,
                                 std::string& error) const {
    bytes.clear();
    error.clear();
    if (resourceIndex >= resources_.size()) {
        error = "Resource index is out of range";
        return false;
    }
    const ResourceInfo& resource = resources_[resourceIndex];
    if (!resource.extractable || resource.bifIndex >= bifs_.size()) {
        error = "Resource is not extractable: " + resource.status;
        return false;
    }
    const BifInfo& bif = bifs_[resource.bifIndex];
    if (bif.browserBacked) {
        error = "Browser-backed resources must be exported through retained range access";
        return false;
    }
    std::ifstream stream(bif.resolvedPath, std::ios::binary);
    if (!stream) {
        error = "Unable to reopen BIF file: " + bif.resolvedPath.string();
        return false;
    }
    stream.seekg(static_cast<std::streamoff>(resource.offset), std::ios::beg);
    if (!stream) {
        error = "Unable to seek to resource payload";
        return false;
    }
    bytes.resize(resource.size);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            bytes.clear();
            error = "Unable to read complete resource payload";
            return false;
        }
    }
    return true;
}

ExtractionReport KeyBifArchive::extractResources(
    const std::vector<std::size_t>& resourceIndices,
    const std::filesystem::path& outputDirectory,
    ExtractionLayout layout,
    bool overwrite) const {
    ExtractionReport report;
    const auto paths = uniqueOutputPaths(resourceIndices, resources_, bifs_, layout);
    for (std::size_t position = 0; position < resourceIndices.size(); ++position) {
        const std::size_t index = resourceIndices[position];
        if (index >= resources_.size() || position >= paths.size() || paths[position].empty()) {
            ++report.failed;
            report.messages.push_back("Invalid resource selection");
            continue;
        }
        const ResourceInfo& resource = resources_[index];
        if (!resource.extractable) {
            ++report.skipped;
            report.messages.push_back(resource.fileName() + ": " + resource.status);
            continue;
        }
        const std::filesystem::path output = outputDirectory / paths[position];
        std::error_code ec;
        if (!overwrite && std::filesystem::exists(output, ec) && !ec) {
            ++report.skipped;
            report.messages.push_back(output.string() + ": already exists");
            continue;
        }
        std::vector<std::uint8_t> bytes;
        std::string error;
        if (!readResource(index, bytes, error) || !writeAtomic(output, bytes, overwrite, error)) {
            ++report.failed;
            report.messages.push_back(resource.fileName() + ": " + error);
            continue;
        }
        ++report.written;
    }
    return report;
}

std::vector<std::filesystem::path> KeyBifArchive::outputPaths(
    const std::vector<std::size_t>& resourceIndices,
    ExtractionLayout layout) const {
    return uniqueOutputPaths(resourceIndices, resources_, bifs_, layout);
}

bool KeyBifArchive::writeZip(const std::vector<std::size_t>& resourceIndices,
                             const std::filesystem::path& outputPath,
                             ExtractionLayout layout,
                             std::string& error) const {
    error.clear();
    std::vector<std::size_t> extractable;
    extractable.reserve(resourceIndices.size());
    for (const std::size_t index : resourceIndices) {
        if (index < resources_.size() && resources_[index].extractable) {
            extractable.push_back(index);
        }
    }
    if (extractable.empty()) {
        error = "No extractable resources were selected";
        return false;
    }
    if (extractable.size() > std::numeric_limits<std::uint16_t>::max()) {
        error = "ZIP output would exceed the classic ZIP entry limit";
        return false;
    }

    const auto paths = uniqueOutputPaths(extractable, resources_, bifs_, layout);
    std::error_code ec;
    if (!outputPath.parent_path().empty()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }
    if (ec) {
        error = "Unable to create ZIP output directory: " + ec.message();
        return false;
    }
    const std::filesystem::path temporary = outputPath.parent_path() /
        ("." + outputPath.filename().string() + ".neobif.tmp");
    std::filesystem::remove(temporary, ec);

    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "Unable to create ZIP output";
        return false;
    }
    std::vector<ZipCentralEntry> central;
    central.reserve(extractable.size());

    for (std::size_t position = 0; position < extractable.size(); ++position) {
        const std::size_t index = extractable[position];
        std::vector<std::uint8_t> bytes;
        if (!readResource(index, bytes, error)) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        std::string name = paths[position].generic_string();
        if (name.size() > std::numeric_limits<std::uint16_t>::max()) {
            error = "ZIP entry name is too long";
            stream.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        const std::streamoff offset = stream.tellp();
        if (offset < 0 || static_cast<std::uint64_t>(offset) >
                              std::numeric_limits<std::uint32_t>::max()) {
            error = "ZIP output exceeds the classic ZIP size limit";
            stream.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        const std::uint32_t checksum = crc32(bytes.data(), bytes.size());
        writeU32(stream, 0x04034B50u);
        writeU16(stream, 20u);
        writeU16(stream, 0x0800u);
        writeU16(stream, 0u);
        writeU16(stream, kZipDosTime);
        writeU16(stream, kZipDosDate);
        writeU32(stream, checksum);
        writeU32(stream, static_cast<std::uint32_t>(bytes.size()));
        writeU32(stream, static_cast<std::uint32_t>(bytes.size()));
        writeU16(stream, static_cast<std::uint16_t>(name.size()));
        writeU16(stream, 0u);
        stream.write(name.data(), static_cast<std::streamsize>(name.size()));
        if (!bytes.empty()) {
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        if (!stream) {
            error = "Unable to write ZIP payload";
            stream.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
        central.push_back({name, checksum, static_cast<std::uint32_t>(bytes.size()),
                           static_cast<std::uint32_t>(offset)});
    }

    const std::streamoff centralOffsetRaw = stream.tellp();
    if (centralOffsetRaw < 0 || static_cast<std::uint64_t>(centralOffsetRaw) >
                                    std::numeric_limits<std::uint32_t>::max()) {
        error = "ZIP central directory exceeds the classic ZIP size limit";
        stream.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    const std::uint32_t centralOffset = static_cast<std::uint32_t>(centralOffsetRaw);
    for (const ZipCentralEntry& entry : central) {
        writeU32(stream, 0x02014B50u);
        writeU16(stream, 20u);
        writeU16(stream, 20u);
        writeU16(stream, 0x0800u);
        writeU16(stream, 0u);
        writeU16(stream, kZipDosTime);
        writeU16(stream, kZipDosDate);
        writeU32(stream, entry.crc);
        writeU32(stream, entry.size);
        writeU32(stream, entry.size);
        writeU16(stream, static_cast<std::uint16_t>(entry.name.size()));
        writeU16(stream, 0u);
        writeU16(stream, 0u);
        writeU16(stream, 0u);
        writeU16(stream, 0u);
        writeU32(stream, 0u);
        writeU32(stream, entry.localOffset);
        stream.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
    }
    const std::streamoff endOffsetRaw = stream.tellp();
    if (endOffsetRaw < 0 || endOffsetRaw - centralOffsetRaw >
                                static_cast<std::streamoff>(std::numeric_limits<std::uint32_t>::max())) {
        error = "ZIP central directory is too large";
        stream.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    const std::uint32_t centralSize = static_cast<std::uint32_t>(endOffsetRaw - centralOffsetRaw);
    writeU32(stream, 0x06054B50u);
    writeU16(stream, 0u);
    writeU16(stream, 0u);
    writeU16(stream, static_cast<std::uint16_t>(central.size()));
    writeU16(stream, static_cast<std::uint16_t>(central.size()));
    writeU32(stream, centralSize);
    writeU32(stream, centralOffset);
    writeU16(stream, 0u);
    stream.flush();
    if (!stream) {
        error = "Unable to finalize ZIP output";
        stream.close();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    stream.close();
    std::filesystem::remove(outputPath, ec);
    ec.clear();
    std::filesystem::rename(temporary, outputPath, ec);
    if (ec) {
        error = "Unable to finalize ZIP file: " + ec.message();
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> KeyBifArchive::scanForKeyFiles(
    const std::filesystem::path& root,
    std::size_t maximumResults) {
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
