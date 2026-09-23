#include "core/SafeOutput.hpp"

#if defined(__EMSCRIPTEN__)

#include <stdexcept>

namespace neobif {

struct SafeOutput::Impl {};

bool validExportRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        if (part == ".." || part == ".") return false;
    }
    return true;
}

std::filesystem::path checkedExportDestination(
    const std::filesystem::path&,
    const std::filesystem::path&,
    const std::vector<std::filesystem::path>&) {
    throw std::runtime_error(
        "Use the retained browser-file exporter, not native filesystem output");
}

SafeOutput::SafeOutput(
    const std::filesystem::path&,
    const std::filesystem::path&,
    bool,
    const std::vector<std::filesystem::path>&) {
    throw std::runtime_error(
        "Native filesystem output is unavailable in the browser");
}

SafeOutput::~SafeOutput() = default;

std::ostream& SafeOutput::stream() {
    throw std::runtime_error(
        "Native filesystem output is unavailable in the browser");
}

void SafeOutput::commit() {
    throw std::runtime_error(
        "Native filesystem output is unavailable in the browser");
}

} // namespace neobif

#else

#include <neoshared/PathUtf8.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

std::string temporarySuffix() {
    static std::atomic<std::uint64_t> serial{0u};
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return ".neobif-" + std::to_string(ticks) + "-" +
           std::to_string(++serial) + ".tmp";
}

std::runtime_error filesystemFailure(
    const std::string& action,
    const fs::path& path,
    const std::error_code& error) {
    return std::runtime_error(
        action + ": " + neoshared::pathToUtf8(path) + ": " + error.message());
}

#if defined(_WIN32)
std::runtime_error windowsFailure(
    const std::string& action,
    const fs::path& path) {
    return std::runtime_error(
        action + ": " + neoshared::pathToUtf8(path) + ": " +
        std::system_category().message(
            static_cast<int>(GetLastError())));
}
#endif

} // namespace

bool validExportRelativePath(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;

    for (const auto& part : path) {
        const std::string value = neoshared::pathToUtf8(part);
        if (value.empty() || value == "." || value == ".." ||
            value.back() == '.' || value.back() == ' ') {
            return false;
        }

        for (const unsigned char ch : value) {
            if (ch < 32u || ch == '\\' || ch == ':' || ch == '*' ||
                ch == '?' || ch == '"' || ch == '<' || ch == '>' ||
                ch == '|') {
                return false;
            }
        }

        const std::string base =
            foldedAscii(value.substr(0u, value.find('.')));
        if (base == "con" || base == "prn" || base == "aux" ||
            base == "nul" ||
            (base.size() == 4u &&
             (base.substr(0u, 3u) == "com" ||
              base.substr(0u, 3u) == "lpt") &&
             base[3] >= '1' && base[3] <= '9')) {
            return false;
        }
    }

    return true;
}

fs::path checkedExportDestination(
    const fs::path& root,
    const fs::path& relative,
    const std::vector<fs::path>& protectedInputs) {
    if (!validExportRelativePath(relative)) {
        throw std::runtime_error(
            "Unsafe or empty export path: " +
            neoshared::pathToUtf8(relative));
    }

    std::error_code error;
    fs::path base = fs::absolute(
        root.empty() ? fs::path(".") : root, error);
    if (error) throw filesystemFailure("Resolve output directory", root, error);
    base = base.lexically_normal();

    const fs::path target = (base / relative).lexically_normal();
    const std::string targetKey = locationKey(target);
    for (const fs::path& input : protectedInputs) {
        if (!input.empty() && locationKey(input) == targetKey) {
            throw std::runtime_error(
                "Output is an input archive: " +
                neoshared::pathToUtf8(input));
        }
    }

    const fs::file_status status = fs::symlink_status(target, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw filesystemFailure("Inspect output", target, error);
    }
    if (!error && fs::exists(status) && !fs::is_regular_file(status)) {
        throw std::runtime_error(
            "Output path is not a regular file: " +
            neoshared::pathToUtf8(target));
    }

    return target;
}

struct SafeOutput::Impl {
    fs::path target;
    fs::path temporary;
    std::ofstream output;
    bool replace{};
    bool committed{};

    ~Impl() {
        output.close();
        if (!committed && !temporary.empty()) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
        }
    }
};

SafeOutput::SafeOutput(
    const fs::path& root,
    const fs::path& relative,
    bool replace,
    const std::vector<fs::path>& protectedInputs)
    : impl_(std::make_unique<Impl>()) {
    Impl& state = *impl_;
    state.replace = replace;
    state.target =
        checkedExportDestination(root, relative, protectedInputs);

    std::error_code error;
    if (!replace && fs::exists(state.target, error)) {
        if (error) {
            throw filesystemFailure(
                "Inspect output", state.target, error);
        }
        throw std::runtime_error(
            "Output already exists: " +
            neoshared::pathToUtf8(state.target));
    }
    if (error) {
        throw filesystemFailure(
            "Inspect output", state.target, error);
    }

    const fs::path parent = state.target.parent_path();
    fs::create_directories(parent, error);
    if (error) {
        throw filesystemFailure(
            "Create output directory", parent, error);
    }
    if (!fs::is_directory(parent, error) || error) {
        if (error) {
            throw filesystemFailure(
                "Inspect output directory", parent, error);
        }
        throw std::runtime_error(
            "Output parent is not a directory: " +
            neoshared::pathToUtf8(parent));
    }

    for (int attempt = 0; attempt < 32; ++attempt) {
        fs::path temporaryName = state.target.filename();
        temporaryName += temporarySuffix();
        state.temporary = parent / temporaryName;

        if (fs::exists(state.temporary, error)) {
            if (error) {
                throw filesystemFailure(
                    "Inspect temporary output", state.temporary, error);
            }
            continue;
        }
        if (error) {
            throw filesystemFailure(
                "Inspect temporary output", state.temporary, error);
        }

        state.output.open(
            state.temporary,
            std::ios::binary | std::ios::out | std::ios::trunc);
        if (state.output.is_open()) break;
        state.output.clear();
    }

    if (!state.output.is_open()) {
        throw std::runtime_error(
            "Unable to create temporary output beside: " +
            neoshared::pathToUtf8(state.target));
    }
}

SafeOutput::~SafeOutput() = default;

std::ostream& SafeOutput::stream() {
    return impl_->output;
}

void SafeOutput::commit() {
    Impl& state = *impl_;
    state.output.flush();
    if (!state.output) {
        throw std::runtime_error(
            "Unable to write complete output; destination was not changed");
    }
    state.output.close();

    if (!state.replace) {
        std::error_code error;
        if (fs::exists(state.target, error)) {
            if (error) {
                throw filesystemFailure(
                    "Inspect output", state.target, error);
            }
            throw std::runtime_error(
                "Output already exists: " +
                neoshared::pathToUtf8(state.target));
        }
        if (error) {
            throw filesystemFailure(
                "Inspect output", state.target, error);
        }
    }

#if defined(_WIN32)
    const DWORD flags =
        state.replace ? MOVEFILE_REPLACE_EXISTING : 0u;
    if (!MoveFileExW(
            state.temporary.c_str(), state.target.c_str(), flags)) {
        throw windowsFailure("Commit output", state.target);
    }
#else
    std::error_code error;
    fs::rename(state.temporary, state.target, error);
    if (error) {
        throw filesystemFailure("Commit output", state.target, error);
    }
#endif

    state.committed = true;
}

} // namespace neobif

#endif
