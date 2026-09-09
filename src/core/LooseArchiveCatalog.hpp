#pragma once

#include "core/KeyBifArchive.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace neoshared::erf {
class ErfArchive;
}

namespace neobif {

enum class LooseArchiveKind {
    Erf,
    Mod,
    Sav,
    Hak,
    Nwm,
    Rim,
    Unknown,
};

struct LooseArchiveInfo {
    std::size_t index{};
    LooseArchiveKind kind{LooseArchiveKind::Unknown};
    std::string fileType;
    std::string version;
    std::filesystem::path relativePath;
    std::filesystem::path resolvedPath;
    std::uintmax_t actualFileSize{};
    InputSnapshot snapshot;
    bool browserBacked{};
    std::uint32_t browserSessionId{};
    std::uint32_t browserFileId{};
    std::string browserRelativePath;
    bool extendedResrefs{};
    std::uint32_t buildYear{};
    std::uint32_t buildDay{};
    std::uint32_t declaredResourceCount{};
    bool valid{};
    std::vector<std::size_t> resourceIndices;
    std::vector<std::string> messages;
};

struct LooseResourceInfo {
    std::size_t index{};
    std::size_t archiveIndex{};
    // Index in the canonical neoshared::erf::ErfArchive resource table.
    std::size_t archiveResourceIndex{};
    std::string resref;
    // Filename-keyed ERF variants use this full archive leaf name. It is empty
    // for KotOR/NWN-style ResRef + type archives.
    std::string storedName;
    std::uint16_t type{};
    std::string extension;
    std::uint32_t resourceId{};
    std::uint32_t offset{};
    std::uint32_t size{};
    std::uint32_t storedSize{};
    bool boundsValid{};
    bool extractable{};
    // True when a browser build may export this resource as a direct source
    // byte range. Compressed resources must be decoded through the shared core.
    bool directRangeExtractable{};
    std::string status;

    std::string fileName() const;
};

class LooseArchiveCatalog final {
public:
    LooseArchiveCatalog();
    ~LooseArchiveCatalog();

    LooseArchiveCatalog(const LooseArchiveCatalog&) = delete;
    LooseArchiveCatalog& operator=(const LooseArchiveCatalog&) = delete;
    LooseArchiveCatalog(LooseArchiveCatalog&&) noexcept;
    LooseArchiveCatalog& operator=(LooseArchiveCatalog&&) noexcept;

    bool scan(const std::filesystem::path& root,
              std::size_t maximumArchives = 8192u, const JobControl& job = {});

    bool openFiles(const std::filesystem::path& root,
                   const std::vector<std::filesystem::path>& files, const JobControl& job = {});

    bool openBrowser(const std::filesystem::path& root,
                     const std::vector<BrowserArchiveFile>& files,
                     const BrowserRangeReader& reader,
                     const BrowserYieldCallback& yield = {});

    void clear();

    bool isOpen() const noexcept { return open_; }
    const std::filesystem::path& rootPath() const noexcept { return rootPath_; }
    const std::vector<LooseArchiveInfo>& archives() const noexcept { return archives_; }
    const std::vector<LooseResourceInfo>& resources() const noexcept { return resources_; }
    const std::vector<std::string>& messages() const noexcept { return messages_; }
    const std::string& lastError() const noexcept { return lastError_; }

    std::size_t validArchiveCount() const noexcept;
    std::size_t invalidArchiveCount() const noexcept;
    std::size_t extractableResourceCount() const noexcept;

    bool readResource(std::size_t resourceIndex,
                      std::vector<std::uint8_t>& bytes,
                      std::string& error) const;

    bool streamResource(std::size_t index, const ByteSink& sink,
                        std::string& error, const JobControl& job = {}) const;

    std::filesystem::path outputPath(std::size_t resourceIndex,
                                     ExtractionLayout layout) const;

    std::vector<std::filesystem::path> outputPaths(
        const std::vector<std::size_t>& resourceIndices,
        ExtractionLayout layout) const;

    static bool hasSupportedExtension(const std::filesystem::path& path);

    static std::vector<std::filesystem::path> scanForArchiveFiles(
        const std::filesystem::path& root,
        std::size_t maximumResults = 8192u, const JobControl& job = {});

private:
    std::filesystem::path rootPath_;
    std::vector<LooseArchiveInfo> archives_;
    std::vector<LooseResourceInfo> resources_;
    // Every valid archive retains the canonical parsed NeoShared object.
    // Native objects release their descriptor/Windows lock after indexing and
    // after each read; browser objects retain only the range callback. Invalid
    // entries keep a null slot so archive/resource indices remain stable.
    std::vector<std::unique_ptr<neoshared::erf::ErfArchive>> archiveReaders_;
    std::vector<std::string> messages_;
    std::string lastError_;
    bool open_{};
};

std::string looseArchiveKindName(LooseArchiveKind kind);
std::string looseArchiveKindExtension(LooseArchiveKind kind);

} // namespace neobif
