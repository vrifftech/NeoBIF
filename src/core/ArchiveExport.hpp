#pragma once
#include <neoshared/bif/KeyBifArchive.hpp>
#include <neoshared/bif/LooseArchiveCatalog.hpp>
#include <neoshared/bif/JobControl.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neobif {
enum class ExtractionLayout { BifAndType, Bif, Type, Flat };
std::vector<std::filesystem::path> archiveOutputPaths(
    const neoshared::bif::KeyBifArchive& archive,
    const std::vector<std::size_t>& indices, ExtractionLayout layout);
std::filesystem::path archiveOutputPath(
    const neoshared::bif::LooseArchiveCatalog& catalog,
    std::size_t resourceIndex, ExtractionLayout layout);

struct ExtractionReport {
    std::size_t written{};
    std::size_t skipped{};
    std::size_t failed{};
    bool cancelled{};
    std::vector<std::string> messages;
};


struct ExportItem {
    std::filesystem::path relativePath;
    std::string displayName;
    std::string identitySuffix;
    std::uint64_t expectedSize{};
    std::function<bool(std::vector<std::uint8_t>&, std::string&)> read;
    std::vector<std::filesystem::path> sourcePaths;
    std::function<bool(const neoshared::bif::ByteSink&, std::string&, const neoshared::bif::JobControl&)> stream;
};
enum class ExistingPolicy { Skip, Replace, KeepBoth };
struct ExportOptions {
    ExistingPolicy existing{ExistingPolicy::Skip};
    bool keepDuplicateNames{};
    std::vector<std::filesystem::path> protectedInputs;
    neoshared::bif::JobControl job;
};
enum class ExportAction { Write, Skip, Conflict };
struct PlannedExport {
    std::size_t itemIndex{};
    std::filesystem::path relativePath;
    ExportAction action{ExportAction::Conflict};
    std::string message;
};
struct ExportPlan {
    std::vector<PlannedExport> entries;
    std::size_t writes{}, skipped{}, conflicts{};
};
ExportPlan planExportItems(const std::vector<ExportItem>& items,
                          const std::filesystem::path& outputDirectory,
                          const ExportOptions& options = {});
// Used only by the explicit Keep both policy. Default paths are never renamed.
std::vector<std::filesystem::path> makeUniqueExportPaths(
    const std::vector<std::filesystem::path>& paths,
    const std::vector<std::string>& identitySuffixes = {});
// Executes an already-reviewed plan without scanning destinations again.
ExtractionReport extractPlannedExportItems(
    const std::vector<ExportItem>& items,
    const ExportPlan& plan,
    const std::filesystem::path& outputDirectory,
    const ExportOptions& options);
ExtractionReport extractExportItems(const std::vector<ExportItem>& items,
    const std::filesystem::path& outputDirectory, const ExportOptions& options);
ExtractionReport extractExportItems(const std::vector<ExportItem>& items,
    const std::filesystem::path& outputDirectory, bool overwrite);
bool writeExportZip(const std::vector<ExportItem>& items,
                    const std::filesystem::path& outputPath,
                    std::string& error, const ExportOptions& options = {});
} // namespace neobif
