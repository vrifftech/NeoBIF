#include "core/ArchiveExport.hpp"
#include "core/SafeOutput.hpp"

#include <neoshared/PathUtf8.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace neobif {
namespace {

namespace fs = std::filesystem;

std::string foldedAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return value;
}

std::string nameKey(const fs::path& path) {
    return foldedAscii(
        neoshared::genericPathToUtf8(path.lexically_normal()));
}

std::string locationKey(const fs::path& path) {
    std::error_code error;
    fs::path absolute = fs::absolute(path, error);
    if (error) absolute = path;
    std::string value =
        neoshared::genericPathToUtf8(absolute.lexically_normal());
#if defined(_WIN32)
    value = foldedAscii(std::move(value));
#endif
    return value;
}

std::vector<fs::path> protectedInputs(
    const std::vector<ExportItem>& items,
    const ExportOptions& options) {
    std::vector<fs::path> result;
    result.reserve(options.protectedInputs.size() + items.size());

    std::unordered_set<std::string> seen;
    const auto append = [&](const fs::path& path) {
        if (path.empty()) return;
        const std::string pathKey = locationKey(path);
        if (seen.insert(pathKey).second) result.push_back(path);
    };

    for (const fs::path& path : options.protectedInputs) append(path);
    for (const ExportItem& item : items) {
        for (const fs::path& path : item.sourcePaths) append(path);
    }
    return result;
}

std::unordered_set<std::string> protectedInputKeys(
    const std::vector<ExportItem>& items,
    const ExportOptions& options) {
    std::unordered_set<std::string> keys;
    keys.reserve(options.protectedInputs.size() + items.size());
    for (const fs::path& path : options.protectedInputs) {
        if (!path.empty()) keys.insert(locationKey(path));
    }
    for (const ExportItem& item : items) {
        for (const fs::path& path : item.sourcePaths) {
            if (!path.empty()) keys.insert(locationKey(path));
        }
    }
    return keys;
}

fs::path outputRoot(const fs::path& root) {
    std::error_code error;
    fs::path result =
        fs::absolute(root.empty() ? fs::path(".") : root, error);
    if (error) {
        throw std::runtime_error(
            "Unable to resolve output directory: " + error.message());
    }
    result = result.lexically_normal();

    const fs::file_status status = fs::status(result, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(
            "Unable to inspect output directory: " + error.message());
    }
    if (!error && fs::exists(status) && !fs::is_directory(status)) {
        throw std::runtime_error("Output location is not a directory");
    }
    return result;
}

fs::path numbered(const fs::path& path, std::size_t ordinal) {
    fs::path name = path.stem();
    name += "__";
    name += std::to_string(ordinal);
    name += path.extension();
    return path.parent_path() / name;
}

bool targetExists(
    const fs::path& target,
    bool& regularFile,
    std::string& errorText) {
    std::error_code error;
    const fs::file_status status = fs::symlink_status(target, error);
    if (error == std::errc::no_such_file_or_directory) {
        regularFile = false;
        return false;
    }
    if (error) {
        errorText = "Unable to inspect output: " + error.message();
        return false;
    }
    if (!fs::exists(status)) {
        regularFile = false;
        return false;
    }
    regularFile = fs::is_regular_file(status);
    return true;
}

bool consume(
    const ExportItem& item,
    const ByteSink& sink,
    std::string& error,
    const JobControl& job) {
    std::uint64_t count = 0u;
    const ByteSink checked =
        [&](const std::uint8_t* bytes,
            std::size_t size,
            std::string& reason) {
            job.check();
            if (count > item.expectedSize ||
                size > item.expectedSize - count) {
                reason = "Reader exceeded the indexed byte count";
                return false;
            }
            if (!sink(bytes, size, reason)) return false;
            count += size;
            return true;
        };

    if (item.stream) {
        if (!item.stream(checked, error, job)) return false;
    } else {
        std::vector<std::uint8_t> bytes;
        if (!item.read || !item.read(bytes, error)) return false;
        constexpr std::size_t chunk = 256u * 1024u;
        for (std::size_t offset = 0u; offset < bytes.size();) {
            const std::size_t size =
                std::min(chunk, bytes.size() - offset);
            if (!checked(bytes.data() + offset, size, error)) return false;
            offset += size;
        }
    }

    if (count != item.expectedSize) {
        error = "Reader returned an unexpected byte count";
        return false;
    }
    return true;
}

void u16(std::ostream& stream, std::uint16_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8u)};
    stream.write(bytes, 2);
}

void u32(std::ostream& stream, std::uint32_t value) {
    const char bytes[] = {
        static_cast<char>(value),
        static_cast<char>(value >> 8u),
        static_cast<char>(value >> 16u),
        static_cast<char>(value >> 24u)};
    stream.write(bytes, 4);
}

std::uint32_t crcUpdate(
    std::uint32_t crc,
    const std::uint8_t* data,
    std::size_t size) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0u; i < result.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u)
                    ? 0xEDB88320u ^ (value >> 1u)
                    : value >> 1u;
            }
            result[i] = value;
        }
        return result;
    }();

    for (std::size_t i = 0u; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 255u] ^ (crc >> 8u);
    }
    return crc;
}

} // namespace

std::vector<fs::path> makeUniqueExportPaths(
    const std::vector<fs::path>& paths,
    const std::vector<std::string>&) {
    std::vector<fs::path> output;
    output.reserve(paths.size());

    std::set<std::string> used;
    std::unordered_map<std::string, std::size_t> nextOrdinal;
    for (const fs::path& path : paths) {
        fs::path candidate = path;
        const std::string baseKey = nameKey(path);
        std::size_t& ordinal = nextOrdinal[baseKey];
        if (ordinal < 2u) ordinal = 2u;
        while (!used.insert(nameKey(candidate)).second) {
            candidate = numbered(path, ordinal++);
        }
        output.push_back(std::move(candidate));
    }
    return output;
}

ExportPlan planExportItems(
    const std::vector<ExportItem>& items,
    const fs::path& root,
    const ExportOptions& options) {
    ExportPlan plan;
    plan.entries.reserve(items.size());

    const fs::path base = outputRoot(root);
    const std::unordered_set<std::string> protectedKeys =
        protectedInputKeys(items, options);

    std::unordered_map<std::string, std::vector<std::size_t>> owners;
    owners.reserve(items.size() * 2u + 1u);
    for (std::size_t i = 0u; i < items.size(); ++i) {
        owners[nameKey(items[i].relativePath)].push_back(i);
    }

    std::unordered_set<std::size_t> clashes;
    for (const auto& [pathName, indices] : owners) {
        if (indices.size() > 1u &&
            options.existing != ExistingPolicy::KeepBoth) {
            clashes.insert(indices.begin(), indices.end());
        }

        fs::path parent =
            fs::path(pathName).parent_path();
        while (!parent.empty()) {
            const auto found = owners.find(nameKey(parent));
            if (found != owners.end()) {
                clashes.insert(indices.begin(), indices.end());
                clashes.insert(found->second.begin(), found->second.end());
            }
            parent = parent.parent_path();
        }
    }

    std::unordered_set<std::string> used;
    used.reserve(items.size() * 2u + 1u);
    std::unordered_map<std::string, std::size_t> nextOrdinal;
    nextOrdinal.reserve(items.size());

    for (std::size_t i = 0u; i < items.size(); ++i) {
        const ExportItem& item = items[i];
        options.job.update(i, items.size(), item.displayName);

        PlannedExport entry{
            i, item.relativePath, ExportAction::Conflict, {}};

        try {
            if ((!item.read && !item.stream) ||
                !validExportRelativePath(item.relativePath)) {
                throw std::runtime_error("Invalid export item or path");
            }
            if (clashes.count(i) != 0u) {
                throw std::runtime_error(
                    "Several resources claim this output; choose Keep both "
                    "or preserve archive hierarchy");
            }

            fs::path candidate = item.relativePath;
            const std::string baseNameKey = nameKey(item.relativePath);
            std::size_t& ordinal = nextOrdinal[baseNameKey];
            if (ordinal < 2u) ordinal = 2u;

            for (;;) {
                const std::string candidateKey = nameKey(candidate);
                const fs::path target = (base / candidate).lexically_normal();

                if (protectedKeys.count(locationKey(target)) != 0u) {
                    throw std::runtime_error(
                        "Output is an input archive");
                }

                bool regularFile = false;
                std::string inspectError;
                const bool exists =
                    targetExists(target, regularFile, inspectError);
                if (!inspectError.empty()) {
                    throw std::runtime_error(inspectError);
                }
                if (exists && !regularFile) {
                    throw std::runtime_error(
                        "Output path is not a regular file");
                }

                const bool reserved =
                    used.count(candidateKey) != 0u;
                if (options.existing == ExistingPolicy::KeepBoth &&
                    (reserved || exists)) {
                    candidate = numbered(item.relativePath, ordinal++);
                    continue;
                }

                if (reserved) {
                    throw std::runtime_error(
                        "Several resources claim this output");
                }

                used.insert(candidateKey);
                entry.relativePath = candidate;
                if (exists &&
                    options.existing == ExistingPolicy::Skip) {
                    entry.action = ExportAction::Skip;
                    entry.message = "Already exists; skipped";
                } else {
                    entry.action = ExportAction::Write;
                    if (candidate != item.relativePath) {
                        entry.message =
                            "Output name: " +
                            neoshared::genericPathToUtf8(candidate);
                    }
                }
                break;
            }
        } catch (const std::exception& exception) {
            entry.message = exception.what();
        }

        if (entry.action == ExportAction::Write) {
            ++plan.writes;
        } else if (entry.action == ExportAction::Skip) {
            ++plan.skipped;
        } else {
            ++plan.conflicts;
        }
        plan.entries.push_back(std::move(entry));
    }

    options.job.update(
        items.size(), items.size(), "Destination check complete");
    return plan;
}

ExtractionReport extractPlannedExportItems(
    const std::vector<ExportItem>& items,
    const ExportPlan& plan,
    const fs::path& root,
    const ExportOptions& options) {
    ExtractionReport report;
    try {
        std::size_t done = 0u;
        for (const PlannedExport& entry : plan.entries) {
            if (entry.itemIndex >= items.size()) {
                ++report.failed;
                report.messages.push_back(
                    "Extraction plan refers to an invalid item");
                continue;
            }

            const ExportItem& item = items[entry.itemIndex];
            options.job.update(
                done, plan.entries.size(), item.displayName);

            if (entry.action == ExportAction::Skip) {
                ++report.skipped;
            } else if (entry.action == ExportAction::Conflict) {
                ++report.failed;
            } else {
                try {
                    const bool replace =
                        options.existing == ExistingPolicy::Replace;
                    // The plan already performed path and input checks. The
                    // lightweight writer only guards against partial final files.
                    SafeOutput output(root, entry.relativePath, replace, {});
                    std::string error;
                    if (!consume(
                            item,
                            [&](const std::uint8_t* data,
                                std::size_t count,
                                std::string& reason) {
                                output.stream().write(
                                    reinterpret_cast<const char*>(data),
                                    static_cast<std::streamsize>(count));
                                if (!output.stream()) {
                                    reason =
                                        "Unable to write temporary output";
                                    return false;
                                }
                                return true;
                            },
                            error,
                            options.job)) {
                        throw std::runtime_error(error);
                    }

                    options.job.check();
                    output.commit();
                    ++report.written;
                } catch (const JobCancelled&) {
                    throw;
                } catch (const std::exception& exception) {
                    ++report.failed;
                    report.messages.push_back(
                        item.displayName + ": " + exception.what());
                }
            }

            if (!entry.message.empty()) {
                report.messages.push_back(
                    item.displayName + ": " + entry.message);
            }

            ++done;
            options.job.update(
                done, plan.entries.size(), item.displayName);
        }
    } catch (const JobCancelled&) {
        report.cancelled = true;
        report.messages.push_back(
            "Cancelled; completed files retained, unfinished output discarded");
    } catch (const std::exception& exception) {
        ++report.failed;
        report.messages.push_back(exception.what());
    }
    return report;
}

ExtractionReport extractExportItems(
    const std::vector<ExportItem>& items,
    const fs::path& root,
    const ExportOptions& options) {
    try {
        const ExportPlan plan = planExportItems(items, root, options);
        return extractPlannedExportItems(items, plan, root, options);
    } catch (const JobCancelled&) {
        ExtractionReport report;
        report.cancelled = true;
        return report;
    } catch (const std::exception& exception) {
        ExtractionReport report;
        ++report.failed;
        report.messages.push_back(exception.what());
        return report;
    }
}

ExtractionReport extractExportItems(
    const std::vector<ExportItem>& items,
    const fs::path& root,
    bool overwrite) {
    ExportOptions options;
    options.existing =
        overwrite ? ExistingPolicy::Replace : ExistingPolicy::Skip;
    return extractExportItems(items, root, options);
}

bool writeExportZip(
    const std::vector<ExportItem>& items,
    const fs::path& path,
    std::string& error,
    const ExportOptions& options) {
    error.clear();
    try {
        options.job.check();
        if (items.empty()) {
            throw std::runtime_error("No resources selected");
        }
        if (nameKey(path.extension()) != ".zip") {
            throw std::runtime_error(
                "ZIP output must have a .zip extension; "
                "input archives cannot be ZIP destinations");
        }
        if (items.size() > 65535u) {
            throw std::runtime_error(
                "Classic ZIP supports at most 65,535 entries; "
                "extract to a folder or select fewer resources");
        }

        std::vector<fs::path> paths;
        paths.reserve(items.size());
        for (const auto& item : items) paths.push_back(item.relativePath);
        if (options.existing == ExistingPolicy::KeepBoth ||
            options.keepDuplicateNames) {
            paths = makeUniqueExportPaths(paths);
        }

        std::set<std::string> names;
        std::uint64_t estimated = 22u;
        for (std::size_t i = 0u; i < items.size(); ++i) {
            const ExportItem& item = items[i];
            const std::string name =
                neoshared::genericPathToUtf8(paths[i]);
            if (!validExportRelativePath(paths[i]) ||
                name.size() > 65535u ||
                (!item.read && !item.stream)) {
                throw std::runtime_error(
                    "Invalid ZIP entry: " + item.displayName);
            }
            if (!names.insert(nameKey(paths[i])).second) {
                throw std::runtime_error(
                    "Duplicate ZIP resource name; select Keep both "
                    "or preserve archive hierarchy");
            }
            estimated +=
                30u + name.size() + item.expectedSize +
                16u + 46u + name.size();
            if (item.expectedSize > 0xFFFFFFFFull ||
                estimated > 0xFFFFFFFFull) {
                throw std::runtime_error(
                    "Classic ZIP exceeds 4 GiB; "
                    "extract to a folder instead");
            }
        }

        for (const std::string& name : names) {
            for (fs::path parent = fs::path(name).parent_path();
                 !parent.empty();
                 parent = parent.parent_path()) {
                if (names.count(nameKey(parent)) != 0u) {
                    throw std::runtime_error(
                        "ZIP file/directory name collision");
                }
            }
        }

        const std::vector<fs::path> inputs =
            protectedInputs(items, options);
        SafeOutput output(
            path.parent_path(),
            path.filename(),
            options.existing == ExistingPolicy::Replace,
            inputs);
        std::ostream& stream = output.stream();

        struct Central {
            std::string name;
            std::uint32_t crc;
            std::uint32_t size;
            std::uint32_t offset;
        };
        std::vector<Central> central;
        central.reserve(items.size());

        for (std::size_t i = 0u; i < items.size(); ++i) {
            const ExportItem& item = items[i];
            options.job.update(i, items.size(), item.displayName);

            const std::string name =
                neoshared::genericPathToUtf8(paths[i]);
            const auto offset =
                static_cast<std::uint32_t>(stream.tellp());

            u32(stream, 0x04034B50u);
            u16(stream, 20u);
            u16(stream, 0x0808u);
            u16(stream, 0u);
            u16(stream, 0u);
            u16(stream, 0x21u);
            u32(stream, 0u);
            u32(stream, 0u);
            u32(stream, 0u);
            u16(stream, static_cast<std::uint16_t>(name.size()));
            u16(stream, 0u);
            stream.write(
                name.data(),
                static_cast<std::streamsize>(name.size()));

            std::uint32_t crc = 0xFFFFFFFFu;
            if (!consume(
                    item,
                    [&](const std::uint8_t* bytes,
                        std::size_t size,
                        std::string& reason) {
                        crc = crcUpdate(crc, bytes, size);
                        stream.write(
                            reinterpret_cast<const char*>(bytes),
                            static_cast<std::streamsize>(size));
                        if (!stream) {
                            reason = "Unable to write ZIP payload";
                            return false;
                        }
                        return true;
                    },
                    error,
                    options.job)) {
                throw std::runtime_error(
                    item.displayName + ": " + error);
            }

            crc ^= 0xFFFFFFFFu;
            const auto size =
                static_cast<std::uint32_t>(item.expectedSize);
            u32(stream, 0x08074b50u);
            u32(stream, crc);
            u32(stream, size);
            u32(stream, size);
            central.push_back({name, crc, size, offset});
            options.job.update(
                i + 1u, items.size(), item.displayName);
        }

        const auto centralOffset =
            static_cast<std::uint32_t>(stream.tellp());
        for (const Central& entry : central) {
            options.job.check();
            u32(stream, 0x02014B50u);
            u16(stream, 20u);
            u16(stream, 20u);
            u16(stream, 0x0808u);
            u16(stream, 0u);
            u16(stream, 0u);
            u16(stream, 0x21u);
            u32(stream, entry.crc);
            u32(stream, entry.size);
            u32(stream, entry.size);
            u16(stream, static_cast<std::uint16_t>(entry.name.size()));
            u16(stream, 0u);
            u16(stream, 0u);
            u16(stream, 0u);
            u16(stream, 0u);
            u32(stream, 0u);
            u32(stream, entry.offset);
            stream.write(
                entry.name.data(),
                static_cast<std::streamsize>(entry.name.size()));
        }

        const auto centralSize =
            static_cast<std::uint32_t>(stream.tellp()) -
            centralOffset;
        u32(stream, 0x06054B50u);
        u16(stream, 0u);
        u16(stream, 0u);
        u16(stream, static_cast<std::uint16_t>(central.size()));
        u16(stream, static_cast<std::uint16_t>(central.size()));
        u32(stream, centralSize);
        u32(stream, centralOffset);
        u16(stream, 0u);

        options.job.check();
        output.commit();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

} // namespace neobif
