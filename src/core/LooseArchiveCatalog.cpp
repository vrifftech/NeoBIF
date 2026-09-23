#include "core/LooseArchiveCatalog.hpp"

#include "core/ArchiveExport.hpp"

#include <neoshared/erf/Archive.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace neobif {
namespace {

constexpr std::size_t kMaximumCatalogResources = 1000000u;
constexpr std::size_t kBrowserYieldInterval = 8u;

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string upperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
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

std::filesystem::path safeRelativeArchivePath(const LooseArchiveInfo& archive) {
    std::filesystem::path input = archive.relativePath;
    if (input.empty()) input = archive.resolvedPath.filename();
    std::filesystem::path output;
    for (const auto& component : input.lexically_normal()) {
        const std::string value = component.generic_string();
        if (value.empty() || value == ".") continue;
        if (value == "..") {
            output /= "_parent_";
        } else {
            output /= sanitizeComponent(value);
        }
    }
    if (output.empty()) {
        output = "archive_" + std::to_string(archive.index) + "." +
                 looseArchiveKindExtension(archive.kind);
    }
    return output;
}

LooseArchiveKind kindFromExtension(const std::filesystem::path& path) {
    const std::string extension = lowerAscii(path.extension().string());
    if (extension == ".erf") return LooseArchiveKind::Erf;
    if (extension == ".mod") return LooseArchiveKind::Mod;
    if (extension == ".sav") return LooseArchiveKind::Sav;
    if (extension == ".hak") return LooseArchiveKind::Hak;
    if (extension == ".nwm") return LooseArchiveKind::Nwm;
    if (extension == ".rim") return LooseArchiveKind::Rim;
    return LooseArchiveKind::Unknown;
}

LooseArchiveKind kindFromFileType(const std::string& fileType) {
    const std::string type = upperAscii(fileType);
    if (type == "ERF") return LooseArchiveKind::Erf;
    if (type == "MOD") return LooseArchiveKind::Mod;
    if (type == "SAV") return LooseArchiveKind::Sav;
    if (type == "HAK") return LooseArchiveKind::Hak;
    if (type == "NWM") return LooseArchiveKind::Nwm;
    if (type == "RIM") return LooseArchiveKind::Rim;
    return LooseArchiveKind::Unknown;
}

std::filesystem::path relativeToRoot(const std::filesystem::path& path,
                                     const std::filesystem::path& root) {
    if (root.empty()) return path.lexically_normal();
    std::error_code ec;
    std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    if (!ec && !relative.empty()) return relative.lexically_normal();
    relative = path.lexically_relative(root);
    if (!relative.empty()) return relative.lexically_normal();
    return path.filename();
}

bool startsWithParentTraversal(const std::filesystem::path& path) {
    const auto begin = path.begin();
    return begin != path.end() && begin->generic_string() == "..";
}

std::uint32_t checkedResourceCount(std::size_t count) {
    if (count > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("Archive resource count is too large to display");
    }
    return static_cast<std::uint32_t>(count);
}

bool headerMatchesExtension(LooseArchiveKind headerKind,
                            LooseArchiveKind extensionKind) {
    if (headerKind == extensionKind) return true;
    // KotOR .sav and .nwm archives use the MOD family header on disk.
    return headerKind == LooseArchiveKind::Mod &&
           (extensionKind == LooseArchiveKind::Sav ||
            extensionKind == LooseArchiveKind::Nwm);
}

bool isNeoBifArchiveFormat(neoshared::erf::ArchiveDiskFormat format) {
    // NeoBIF is a KotOR-style game archive browser. Keep the same V1.0/V1.1
    // ERF-family and RIM scope it had before the parser was centralized.
    return format == neoshared::erf::ArchiveDiskFormat::ErfV1 ||
           format == neoshared::erf::ArchiveDiskFormat::RimV1;
}

void populateFromSharedArchive(
    LooseArchiveInfo& archive,
    std::vector<LooseResourceInfo>& resources,
    const neoshared::erf::ErfArchive& reader) {
    if (!isNeoBifArchiveFormat(reader.disk_format())) {
        throw std::runtime_error(
            "NeoBIF supports ERF-family and RIM V1.0/V1.1 archives. "
            "Open newer ERF V2/V3 archives in NeoERF.");
    }
    if (reader.count() > kMaximumCatalogResources ||
        resources.size() > kMaximumCatalogResources - reader.count()) {
        throw std::runtime_error(
            "Combined standalone-archive resources exceed the catalog safety limit");
    }

    archive.fileType = reader.file_type();
    archive.version = reader.version();
    const LooseArchiveKind extensionKind = kindFromExtension(
        archive.relativePath.empty() ? archive.resolvedPath : archive.relativePath);
    const LooseArchiveKind headerKind = kindFromFileType(archive.fileType);
    archive.kind = extensionKind != LooseArchiveKind::Unknown ? extensionKind : headerKind;
    if (headerKind != LooseArchiveKind::Unknown &&
        extensionKind != LooseArchiveKind::Unknown &&
        !headerMatchesExtension(headerKind, extensionKind)) {
        archive.messages.push_back(
            "Filename extension suggests " + looseArchiveKindName(extensionKind) +
            ", but the archive header identifies " + looseArchiveKindName(headerKind));
    }

    archive.extendedResrefs = reader.extended_resrefs();
    archive.buildYear = reader.build_year();
    archive.buildDay = reader.build_day();
    archive.declaredResourceCount = checkedResourceCount(reader.count());
    archive.resourceIndices.clear();
    archive.resourceIndices.reserve(reader.count());

    const std::size_t initialResourceCount = resources.size();
    try {
        const auto profile = reader.resource_type_profile();
        for (std::size_t localIndex = 0; localIndex < reader.count(); ++localIndex) {
            const auto& source = reader.resource(localIndex);
            LooseResourceInfo resource;
            resource.index = resources.size();
            resource.archiveIndex = archive.index;
            resource.archiveResourceIndex = localIndex;
            resource.resref = source.resref;
            resource.storedName = source.filename;
            resource.type = source.restype;
            resource.extension = source.extension(profile);
            resource.resourceId = source.resid;
            resource.offset = source.data_offset;
            resource.size = source.data_size;
            resource.storedSize =
                (source.packed_size != 0u || source.data_size == 0u)
                    ? source.packed_size
                    : source.data_size;
            resource.boundsValid = true; // Shared parser rejects invalid extents.
            resource.extractable = true;
            resource.directRangeExtractable =
                reader.compression_scheme() == 0u &&
                resource.storedSize == resource.size;
            resource.status = "Ready (NeoShared ERF core)";
            archive.resourceIndices.push_back(resource.index);
            resources.push_back(std::move(resource));
        }
    } catch (...) {
        resources.resize(initialResourceCount);
        archive.resourceIndices.clear();
        throw;
    }

    archive.valid = true;
}

void appendInvalidMessage(std::vector<std::string>& messages,
                          const LooseArchiveInfo& archive,
                          const std::string& reason) {
    const std::string path = archive.relativePath.empty()
        ? archive.resolvedPath.generic_string()
        : archive.relativePath.generic_string();
    messages.push_back((path.empty() ? std::string("<archive>") : path) + ": " + reason);
}

} // namespace

LooseArchiveCatalog::LooseArchiveCatalog() = default;
LooseArchiveCatalog::~LooseArchiveCatalog() = default;
LooseArchiveCatalog::LooseArchiveCatalog(LooseArchiveCatalog&&) noexcept = default;
LooseArchiveCatalog& LooseArchiveCatalog::operator=(LooseArchiveCatalog&&) noexcept = default;

std::string LooseResourceInfo::fileName() const {
    if (!storedName.empty()) {
        std::string normalized = storedName;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        const std::string leaf = std::filesystem::path(normalized).filename().string();
        if (!leaf.empty()) return sanitizeComponent(leaf);
    }

    std::string stem = sanitizeComponent(resref);
    std::string ext = extension.empty() ? resourceTypeExtension(type) : extension;
    ext = sanitizeComponent(ext);
    return ext.empty() ? stem : stem + "." + ext;
}

std::string looseArchiveKindName(LooseArchiveKind kind) {
    switch (kind) {
    case LooseArchiveKind::Erf: return "ERF";
    case LooseArchiveKind::Mod: return "MOD";
    case LooseArchiveKind::Sav: return "SAV";
    case LooseArchiveKind::Hak: return "HAK";
    case LooseArchiveKind::Nwm: return "NWM";
    case LooseArchiveKind::Rim: return "RIM";
    case LooseArchiveKind::Unknown: return "unknown archive";
    }
    return "unknown archive";
}

std::string looseArchiveKindExtension(LooseArchiveKind kind) {
    return lowerAscii(looseArchiveKindName(kind));
}

void LooseArchiveCatalog::clear() {
    archiveReaders_.clear();
    rootPath_.clear();
    archives_.clear();
    resources_.clear();
    messages_.clear();
    lastError_.clear();
    open_ = false;
}

bool LooseArchiveCatalog::hasSupportedExtension(const std::filesystem::path& path) {
    return kindFromExtension(path) != LooseArchiveKind::Unknown;
}

std::vector<std::filesystem::path> LooseArchiveCatalog::scanForArchiveFiles(
    const std::filesystem::path& root,
    std::size_t maximumResults, const JobControl& job) {
    std::vector<std::filesystem::path> results;
    std::error_code ec;
    if (std::filesystem::is_regular_file(root, ec) && !ec) {
        if (hasSupportedExtension(root)) results.push_back(root);
        return results;
    }
    ec.clear();
    if (!std::filesystem::is_directory(root, ec) || ec) return results;

    for (std::filesystem::recursive_directory_iterator it(
             root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end && results.size() < maximumResults; it.increment(ec)) {
        job.check();
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (hasSupportedExtension(it->path())) results.push_back(it->path());
    }
    std::sort(results.begin(), results.end(), [&](const auto& left, const auto& right) {
        return lowerAscii(relativeToRoot(left, root).generic_string()) <
               lowerAscii(relativeToRoot(right, root).generic_string());
    });
    return results;
}

bool LooseArchiveCatalog::scan(const std::filesystem::path& root,
                               std::size_t maximumArchives, const JobControl& job) {
    clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        lastError_ = "Standalone archive scan root is not a readable directory: " + root.string();
        return false;
    }
    const std::size_t probeLimit = maximumArchives ==
            std::numeric_limits<std::size_t>::max()
        ? maximumArchives
        : maximumArchives + 1u;
    const auto absoluteRoot = std::filesystem::absolute(root).lexically_normal();
    auto files = scanForArchiveFiles(absoluteRoot, probeLimit, job);
    const bool reachedLimit = files.size() > maximumArchives;
    if (reachedLimit) files.resize(maximumArchives);
    if (!openFiles(absoluteRoot, files, job)) return false;
    if (reachedLimit) {
        messages_.push_back("Standalone archive scan reached the " +
                            std::to_string(maximumArchives) + "-file safety limit");
    }
    return true;
}

bool LooseArchiveCatalog::openFiles(
    const std::filesystem::path& root,
    const std::vector<std::filesystem::path>& files, const JobControl& job) {
    clear();
    rootPath_ = std::filesystem::absolute(root).lexically_normal();
    open_ = true;

    std::vector<std::filesystem::path> candidates;
    candidates.reserve(files.size());
    for (const auto& candidate : files) {
        if (hasSupportedExtension(candidate)) candidates.push_back(candidate);
    }
    std::sort(candidates.begin(), candidates.end(), [&](const auto& left, const auto& right) {
        return lowerAscii(relativeToRoot(left, root).generic_string()) <
               lowerAscii(relativeToRoot(right, root).generic_string());
    });

    for (const auto& candidate : candidates) {
        job.update(archives_.size(), candidates.size(), "Indexing game-directory archives");
        std::filesystem::path path = candidate;
        if (path.is_relative()) path = rootPath_ / path;

        LooseArchiveInfo archive;
        archive.index = archives_.size();
        archive.resolvedPath = path;
        archive.relativePath = relativeToRoot(path, rootPath_);
        archive.kind = kindFromExtension(archive.relativePath);
        if (startsWithParentTraversal(archive.relativePath)) {
            archive.relativePath = path.filename();
        }

        std::unique_ptr<neoshared::erf::ErfArchive> sharedArchive;
        try {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec) {
                throw std::runtime_error("Archive file is missing or unreadable");
            }
            archive.actualFileSize = std::filesystem::file_size(path, ec);
            if (ec) {
                throw std::runtime_error(
                    "Unable to determine archive file size: " + ec.message());
            }

            archive.snapshot = neoshared::erf::capture_regular_file_identity(path);
            sharedArchive = std::make_unique<neoshared::erf::ErfArchive>();
            sharedArchive->set_resource_type_profile(
                neoshared::erf::ResourceNameProfile::KotOR);
            sharedArchive->load(path);
            populateFromSharedArchive(archive, resources_, *sharedArchive);
            if (!unchangedInput(path, archive.snapshot)) throw std::runtime_error("Archive changed while indexing; rescan");
            // Retain the parsed canonical archive metadata, but release its
            // native descriptor/Windows lock. Resource reads reopen the same
            // file with an identity check and do not reparse the tables.
            sharedArchive->release_input_handle();
        } catch (const std::exception& exception) {
            if (!archive.resourceIndices.empty()) resources_.resize(archive.resourceIndices.front());
            archive.valid = false;
            archive.resourceIndices.clear();
            archive.messages.push_back(exception.what());
            appendInvalidMessage(messages_, archive, exception.what());
            sharedArchive.reset();
        } catch (...) {
            if (!archive.resourceIndices.empty()) resources_.resize(archive.resourceIndices.front());
            const std::string reason = "Unknown error while reading archive";
            archive.valid = false;
            archive.resourceIndices.clear();
            archive.messages.push_back(reason);
            appendInvalidMessage(messages_, archive, reason);
            sharedArchive.reset();
        }

        archives_.push_back(std::move(archive));
        archiveReaders_.push_back(std::move(sharedArchive));
    }
    return true;
}

bool LooseArchiveCatalog::openBrowser(
    const std::filesystem::path& root,
    const std::vector<BrowserArchiveFile>& files,
    const BrowserRangeReader& reader,
    const BrowserYieldCallback& yield) {
    clear();
    rootPath_ = root;
    if (!reader) {
        lastError_ = "Browser archive reader is unavailable";
        return false;
    }
    open_ = true;

    std::vector<BrowserArchiveFile> candidates;
    candidates.reserve(files.size());
    for (const auto& file : files) {
        if (hasSupportedExtension(std::filesystem::path(file.relativePath))) {
            candidates.push_back(file);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return lowerAscii(left.relativePath) < lowerAscii(right.relativePath);
    });

    for (const BrowserArchiveFile& file : candidates) {
        if (yield) yield();
        const std::filesystem::path browserPath(file.relativePath);

        LooseArchiveInfo archive;
        archive.index = archives_.size();
        archive.browserBacked = true;
        archive.browserSessionId = file.sessionId;
        archive.browserFileId = file.fileId;
        archive.browserRelativePath = file.relativePath;
        archive.resolvedPath = browserPath;
        archive.relativePath = root.empty()
            ? browserPath.lexically_normal()
            : browserPath.lexically_relative(root);
        if (archive.relativePath.empty() || startsWithParentTraversal(archive.relativePath)) {
            archive.relativePath = browserPath.filename();
        }
        archive.kind = kindFromExtension(archive.relativePath);

        std::unique_ptr<neoshared::erf::ErfArchive> sharedArchive;
        try {
            if (file.size > static_cast<std::uint64_t>(
                    std::numeric_limits<std::uintmax_t>::max())) {
                throw std::runtime_error("Browser archive size is not representable");
            }
            archive.actualFileSize = static_cast<std::uintmax_t>(file.size);

            const auto readCounter = std::make_shared<std::size_t>(0u);
            neoshared::erf::ArchiveRangeReader scopedReader =
                [reader, file, yield, readCounter](
                    std::uint64_t offset,
                    std::size_t length,
                    std::vector<std::uint8_t>& bytes,
                    std::string& error) {
                    if (offset > file.size ||
                        static_cast<std::uint64_t>(length) > file.size - offset) {
                        bytes.clear();
                        error = "Requested browser archive range lies outside the file";
                        return false;
                    }
                    ++(*readCounter);
                    if (yield && *readCounter % kBrowserYieldInterval == 0u) yield();
                    return reader(file, offset, length, bytes, error);
                };

            sharedArchive = std::make_unique<neoshared::erf::ErfArchive>();
            sharedArchive->set_resource_type_profile(
                neoshared::erf::ResourceNameProfile::KotOR);
            sharedArchive->load_from_reader(browserPath, file.size, std::move(scopedReader));
            populateFromSharedArchive(archive, resources_, *sharedArchive);
        } catch (const std::exception& exception) {
            archive.valid = false;
            archive.resourceIndices.clear();
            archive.messages.push_back(exception.what());
            appendInvalidMessage(messages_, archive, exception.what());
            sharedArchive.reset();
        } catch (...) {
            const std::string reason = "Unknown error while reading browser archive";
            archive.valid = false;
            archive.resourceIndices.clear();
            archive.messages.push_back(reason);
            appendInvalidMessage(messages_, archive, reason);
            sharedArchive.reset();
        }

        archives_.push_back(std::move(archive));
        archiveReaders_.push_back(std::move(sharedArchive));
    }
    return true;
}

std::size_t LooseArchiveCatalog::validArchiveCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        archives_.begin(), archives_.end(),
        [](const LooseArchiveInfo& archive) { return archive.valid; }));
}

std::size_t LooseArchiveCatalog::invalidArchiveCount() const noexcept {
    return archives_.size() - validArchiveCount();
}

std::size_t LooseArchiveCatalog::extractableResourceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        resources_.begin(), resources_.end(),
        [](const LooseResourceInfo& resource) { return resource.extractable; }));
}

bool LooseArchiveCatalog::readResource(
    std::size_t resourceIndex,
    std::vector<std::uint8_t>& bytes,
    std::string& error) const {
    bytes.clear();
    error.clear();
    if (resourceIndex >= resources_.size()) {
        error = "Standalone archive resource index is out of range";
        return false;
    }
    const LooseResourceInfo& resource = resources_[resourceIndex];
    if (!resource.extractable || resource.archiveIndex >= archives_.size() ||
        resource.archiveIndex >= archiveReaders_.size()) {
        error = "Resource is not extractable: " + resource.status;
        return false;
    }
    const LooseArchiveInfo& archive = archives_[resource.archiveIndex];
    neoshared::erf::ErfArchive* activeReader =
        archiveReaders_[resource.archiveIndex].get();
    if (activeReader == nullptr) {
        error = archive.browserBacked
            ? "The retained browser archive reader is unavailable"
            : "The retained NeoShared archive metadata is unavailable";
        return false;
    }

    try {
        if (!isNeoBifArchiveFormat(activeReader->disk_format()) ||
            resource.archiveResourceIndex >= activeReader->count()) {
            error = "Archive resource index is no longer available";
            return false;
        }

        bytes = activeReader->read_resource(resource.archiveResourceIndex);
        if (!archive.browserBacked) activeReader->release_input_handle();
        if (bytes.size() != resource.size) {
            error = "Shared ERF reader returned " + std::to_string(bytes.size()) +
                    " byte(s), expected " + std::to_string(resource.size);
            bytes.clear();
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (!archive.browserBacked) activeReader->release_input_handle();
        error = exception.what();
        bytes.clear();
        return false;
    } catch (...) {
        if (!archive.browserBacked) activeReader->release_input_handle();
        error = "Unknown error while extracting archive resource";
        bytes.clear();
        return false;
    }
}

bool LooseArchiveCatalog::streamResource(std::size_t index, const ByteSink& sink,
                                         std::string& error, const JobControl& job) const {
    if (index >= resources_.size()) { error="Invalid resource index";return false; }
    const auto& resource=resources_[index];
    if (!resource.extractable || resource.archiveIndex>=archives_.size()) { error="Resource is unavailable";return false; }
    const auto& archive=archives_[resource.archiveIndex];
    if (!archive.browserBacked && resource.directRangeExtractable) {
        return streamInputRange(archive.resolvedPath,archive.snapshot,resource.offset,resource.size,sink,error,job);
    }
    job.check();
    std::vector<std::uint8_t> bytes;
    if (!readResource(index,bytes,error)) return false;
    for(std::size_t offset=0;offset<bytes.size();) {
        job.check();const auto count=std::min<std::size_t>(256u*1024u,bytes.size()-offset);
        if(!sink(bytes.data()+offset,count,error))return false;
        offset+=count;
    }
    return true;
}

std::filesystem::path LooseArchiveCatalog::outputPath(
    std::size_t resourceIndex,
    ExtractionLayout layout) const {
    if (resourceIndex >= resources_.size()) return {};
    const LooseResourceInfo& resource = resources_[resourceIndex];
    if (resource.archiveIndex >= archives_.size()) return {};
    const LooseArchiveInfo& archive = archives_[resource.archiveIndex];
    const std::filesystem::path archivePath = safeRelativeArchivePath(archive);
    const std::string file = resource.fileName();
    const std::string type = sanitizeComponent(
        resource.extension.empty() ? resourceTypeExtension(resource.type)
                                   : resource.extension);
    switch (layout) {
    case ExtractionLayout::BifAndType:
        return archivePath / type / file;
    case ExtractionLayout::Bif:
        return archivePath / file;
    case ExtractionLayout::Type:
        return std::filesystem::path(type) / file;
    case ExtractionLayout::Flat:
        return std::filesystem::path(file);
    }
    return std::filesystem::path(file);
}

std::vector<std::filesystem::path> LooseArchiveCatalog::outputPaths(
    const std::vector<std::size_t>& resourceIndices,
    ExtractionLayout layout) const {
    std::vector<std::filesystem::path> paths;
    std::vector<std::string> suffixes;
    paths.reserve(resourceIndices.size());
    suffixes.reserve(resourceIndices.size());
    for (const std::size_t index : resourceIndices) {
        paths.push_back(outputPath(index, layout));
        if (index < resources_.size()) {
            std::ostringstream suffix;
            suffix << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                   << resources_[index].resourceId;
            suffixes.push_back(suffix.str());
        } else {
            suffixes.emplace_back();
        }
    }
    return paths; // Renaming is an explicit export policy, never a reader side effect.
}

} // namespace neobif
