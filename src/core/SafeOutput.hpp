#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace neobif {
// An exclusively owned sibling staging file. No caller ever truncates/removes
// the old target. Native POSIX commits are relative to held directory handles;
// Windows directory handles deny delete/rename and reject reparse points.
class SafeOutput final {
public:
    SafeOutput(const std::filesystem::path& root, const std::filesystem::path& relative,
               bool replace, const std::vector<std::filesystem::path>& protectedInputs);
    ~SafeOutput();
    SafeOutput(const SafeOutput&) = delete;
    SafeOutput& operator=(const SafeOutput&) = delete;
    std::ostream& stream();
    void commit();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool validExportRelativePath(const std::filesystem::path& path);
// Validates without creating any filesystem objects, returns existing casing.
std::filesystem::path checkedExportDestination(
    const std::filesystem::path& root, const std::filesystem::path& relative,
    const std::vector<std::filesystem::path>& protectedInputs);
} // namespace neobif
