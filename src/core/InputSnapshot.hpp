#pragma once

#include "core/JobControl.hpp"

#include <neoshared/PathUtf8.hpp>
#include <neoshared/erf/Utils.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace neobif {

using InputSnapshot = neoshared::erf::FileIdentity;

inline bool unchangedInput(
    const std::filesystem::path& path,
    const InputSnapshot& snapshot) {
    return neoshared::erf::same_regular_file_revision(path, snapshot);
}

inline bool streamInputRange(
    const std::filesystem::path& path,
    const InputSnapshot& snapshot,
    std::uint64_t offset,
    std::uint64_t size,
    const ByteSink& sink,
    std::string& error,
    const JobControl& job = {}) {
    if (offset > snapshot.size || size > snapshot.size - offset || !sink) {
        error = "Invalid indexed resource range";
        return false;
    }
    if (offset >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        error = "Resource offset is too large";
        return false;
    }

    // Extraction jobs process resources sequentially on one worker thread.
    // Reuse the last archive stream instead of reopening and re-identifying
    // the same BIF for every resource.
    struct CachedInput {
        std::filesystem::path path;
        std::ifstream stream;
    };
    thread_local CachedInput cached;

    if (!cached.stream.is_open() || cached.path != path) {
        cached.stream.close();
        cached.stream.clear();
        cached.path = path;
        cached.stream.open(path, std::ios::binary);
        if (!cached.stream.is_open()) {
            error = "Unable to open archive: " +
                    neoshared::pathToUtf8(path);
            return false;
        }
    }

    cached.stream.clear();
    cached.stream.seekg(
        static_cast<std::streamoff>(offset), std::ios::beg);
    if (!cached.stream) {
        error = "Unable to seek archive resource";
        return false;
    }

    std::array<std::uint8_t, 256u * 1024u> buffer{};
    for (std::uint64_t remaining = size; remaining != 0u;) {
        job.check();
        const std::size_t requested =
            static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, buffer.size()));
        cached.stream.read(
            reinterpret_cast<char*>(buffer.data()),
            static_cast<std::streamsize>(requested));
        const std::streamsize got = cached.stream.gcount();
        if (got <= 0) {
            error = "Unable to read complete resource";
            return false;
        }
        if (!sink(
                buffer.data(),
                static_cast<std::size_t>(got),
                error)) {
            return false;
        }
        remaining -= static_cast<std::uint64_t>(got);
    }

    return true;
}

} // namespace neobif
