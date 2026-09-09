#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace neobif {
struct JobCancelled : std::runtime_error {
    JobCancelled() : std::runtime_error("Cancelled") {}
};
struct JobControl {
    std::function<bool()> cancelled;
    std::function<void(std::size_t, std::size_t, const std::string&)> progress;
    void check() const { if (cancelled && cancelled()) throw JobCancelled(); }
    void update(std::size_t done, std::size_t total, const std::string& detail) const {
        check();
        if (progress) progress(done, total, detail);
    }
};
using ByteSink = std::function<bool(const std::uint8_t*, std::size_t, std::string&)>;
} // namespace neobif
