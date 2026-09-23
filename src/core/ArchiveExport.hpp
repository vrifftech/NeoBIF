#pragma once
#include "core/KeyBifArchive.hpp"
#include "core/JobControl.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace neobif {
struct ExportItem {
    std::filesystem::path relativePath;
    std::string displayName;
    std::string identitySuffix;
    std::uint64_t expectedSize{};
    std::function<bool(std::vector<std::uint8_t>&, std::string&)> read;
    std::vector<std::filesystem::path> sourcePaths;
    std::function<bool(const ByteSink&, std::string&, const JobControl&)> stream;
};
enum class ExistingPolicy { Skip, Replace, KeepBoth };
struct ExportOptions {
    ExistingPolicy existing{ExistingPolicy::Skip};
    bool keepDuplicateNames{};
    std::vector<std::filesystem::path> protectedInputs;
    JobControl job;
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
