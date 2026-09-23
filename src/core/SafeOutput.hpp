#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace neobif {
// Lightweight sibling staging output. It avoids partial final files but does
// not pin directories, compare file identities, or attempt TOCTOU hardening.
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
// Performs basic relative-path, destination-type, and exact input-path checks.
std::filesystem::path checkedExportDestination(
    const std::filesystem::path& root, const std::filesystem::path& relative,
    const std::vector<std::filesystem::path>& protectedInputs);
} // namespace neobif
