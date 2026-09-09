#pragma once

#include "core/InputSnapshot.hpp"
#include <map>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace neobif {

enum class IssueSeverity {
    Warning,
    Error,
};

struct ArchiveIssue {
    IssueSeverity severity{IssueSeverity::Warning};
    std::string message;
    std::optional<std::size_t> bifIndex;
    std::optional<std::uint32_t> resourceId;
};

struct BifInfo {
    std::size_t index{};
    std::uint32_t declaredFileSize{};
    std::string storedPath;
    std::uint16_t driveFlags{};
    std::filesystem::path resolvedPath;
    std::uintmax_t actualFileSize{};
    InputSnapshot snapshot;
    bool browserBacked{};
    std::uint32_t browserSessionId{};
    std::uint32_t browserFileId{};
    std::string browserRelativePath;
    std::uint32_t variableResourceCount{};
    std::uint32_t fixedResourceCount{};
    std::uint32_t tableOffset{};
    bool available{};
    bool valid{};
    std::vector<std::size_t> resourceIndices;
    std::vector<std::string> messages;
};

struct ResourceInfo {
    std::size_t index{};
    std::string resref;
    std::uint16_t type{};
    std::string extension;
    std::uint32_t resourceId{};
    std::uint32_t bifIndex{};
    std::uint32_t tableIndex{};
    std::uint32_t engineIndex{};
    std::uint32_t offset{};
    std::uint32_t size{};
    bool keyed{true};
    bool bifEntryFound{};
    bool idMatches{};
    bool typeMatches{};
    bool boundsValid{};
    bool extractable{};
    std::string status;

    std::string fileName() const;
};

enum class ExtractionLayout {
    BifAndType,
    Bif,
    Type,
    Flat,
};

struct ExtractionReport {
    std::size_t written{};
    std::size_t skipped{};
    std::size_t failed{};
    bool cancelled{};
    std::vector<std::string> messages;
};

struct BrowserArchiveFile {
    std::uint32_t sessionId{};
    std::uint32_t fileId{};
    std::string relativePath;
    std::uint64_t size{};
    bool preferred{};
    std::optional<std::size_t> explicitBifIndex;
};

using BrowserRangeReader = std::function<bool(
    const BrowserArchiveFile& file,
    std::uint64_t offset,
    std::size_t length,
    std::vector<std::uint8_t>& bytes,
    std::string& error)>;

using BrowserYieldCallback = std::function<void()>;

class KeyBifArchive final {
public:
    bool open(const std::filesystem::path& keyPath,
              const std::vector<std::filesystem::path>& supplementaryFiles = {},
              const std::map<std::size_t, std::filesystem::path>& explicitRelocations = {},
              const JobControl& job = {});

    // Opens a KEY/BIF set whose contents remain in browser-owned File objects.
    // The supplied reader is used only while building the archive index; large
    // resource payloads remain range-addressable through BifInfo browser IDs.
    bool openBrowser(const BrowserArchiveFile& keyFile,
                     const std::vector<BrowserArchiveFile>& files,
                     const BrowserRangeReader& reader,
                     const BrowserYieldCallback& yield = {});

    void clear();

    bool isOpen() const noexcept { return open_; }
    const std::filesystem::path& keyPath() const noexcept { return keyPath_; }
    std::uint32_t buildYear() const noexcept { return buildYear_; }
    std::uint32_t buildDay() const noexcept { return buildDay_; }
    const std::vector<BifInfo>& bifs() const noexcept { return bifs_; }
    const std::vector<ResourceInfo>& resources() const noexcept { return resources_; }
    const std::vector<ArchiveIssue>& issues() const noexcept { return issues_; }
    const std::string& lastError() const noexcept { return lastError_; }

    std::size_t extractableResourceCount() const noexcept;
    std::size_t missingBifCount() const noexcept;

    bool readResource(std::size_t resourceIndex,
                      std::vector<std::uint8_t>& bytes,
                      std::string& error) const;

    bool streamResource(std::size_t resourceIndex, const ByteSink& sink,
                        std::string& error, const JobControl& job = {}) const;
    std::vector<std::filesystem::path> inputPaths() const;

    ExtractionReport extractResources(
        const std::vector<std::size_t>& resourceIndices,
        const std::filesystem::path& outputDirectory,
        ExtractionLayout layout,
        bool overwrite) const;

    std::vector<std::filesystem::path> outputPaths(
        const std::vector<std::size_t>& resourceIndices,
        ExtractionLayout layout) const;

    bool writeZip(const std::vector<std::size_t>& resourceIndices,
                  const std::filesystem::path& outputPath,
                  ExtractionLayout layout,
                  std::string& error) const;

    static std::vector<std::filesystem::path> scanForKeyFiles(
        const std::filesystem::path& root,
        std::size_t maximumResults = 64,
        const JobControl& job = {});

private:
    std::filesystem::path keyPath_;
    InputSnapshot keySnapshot_;
    std::uint32_t buildYear_{};
    std::uint32_t buildDay_{};
    std::vector<BifInfo> bifs_;
    std::vector<ResourceInfo> resources_;
    std::vector<ArchiveIssue> issues_;
    std::string lastError_;
    bool open_{};
};

std::string resourceTypeExtension(std::uint16_t type);
std::string resourceTypeLabel(std::uint16_t type);
// The search bar recognizes bare known extensions without a duplicate type table.
bool isKnownResourceExtension(const std::string& extension);
std::string formatByteSize(std::uint64_t bytes);
std::string hexResourceId(std::uint32_t id);
std::string extractionLayoutName(ExtractionLayout layout);

} // namespace neobif
