#include "core/KeyBifArchive.hpp"
#include "core/Version.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lowerAscii(haystack).find(lowerAscii(needle)) != std::string::npos;
}

void usage() {
    std::cout
        << "NeoBIF " << neobif::kVersion << "\n"
        << "usage:\n"
        << "  neobif-cli info <chitin.key> [bif-files...]\n"
        << "  neobif-cli list <chitin.key> [--filter text] [bif-files...]\n"
        << "  neobif-cli scan <game-directory>\n"
        << "  neobif-cli extract <chitin.key> <output-directory> [options] [bif-files...]\n"
        << "  neobif-cli zip <chitin.key> <output.zip> [options] [bif-files...]\n\n"
        << "options:\n"
        << "  --filter TEXT        Extract/list matching resource names, types, or BIF paths\n"
        << "  --layout MODE        bif-type (default), bif, type, or flat\n"
        << "  --overwrite          Replace existing extracted files\n";
}

neobif::ExtractionLayout parseLayout(const std::string& value) {
    const std::string lower = lowerAscii(value);
    if (lower == "bif") return neobif::ExtractionLayout::Bif;
    if (lower == "type") return neobif::ExtractionLayout::Type;
    if (lower == "flat") return neobif::ExtractionLayout::Flat;
    return neobif::ExtractionLayout::BifAndType;
}

std::vector<std::size_t> selectResources(const neobif::KeyBifArchive& archive,
                                         const std::string& filter) {
    std::vector<std::size_t> selected;
    for (const auto& resource : archive.resources()) {
        const std::string bifPath = resource.bifIndex < archive.bifs().size()
            ? archive.bifs()[resource.bifIndex].storedPath : std::string{};
        if (containsInsensitive(resource.fileName(), filter) ||
            containsInsensitive(resource.extension, filter) ||
            containsInsensitive(bifPath, filter) ||
            containsInsensitive(neobif::hexResourceId(resource.resourceId), filter)) {
            selected.push_back(resource.index);
        }
    }
    return selected;
}

bool openArchive(neobif::KeyBifArchive& archive,
                 const std::filesystem::path& key,
                 const std::vector<std::filesystem::path>& bifs) {
    if (archive.open(key, bifs)) return true;
    std::cerr << "error: " << archive.lastError() << '\n';
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command = lowerAscii(argv[1]);
    if (command == "-h" || command == "--help" || command == "help") {
        usage();
        return 0;
    }
    if (command == "--version" || command == "version") {
        std::cout << neobif::kVersion << '\n';
        return 0;
    }

    if (command == "scan") {
        if (argc != 3) {
            usage();
            return 2;
        }
        const auto keys = neobif::KeyBifArchive::scanForKeyFiles(argv[2]);
        for (const auto& key : keys) std::cout << key.string() << '\n';
        return keys.empty() ? 1 : 0;
    }

    if (command != "info" && command != "list" && command != "extract" &&
        command != "zip") {
        std::cerr << "error: unknown command: " << argv[1] << '\n';
        usage();
        return 2;
    }

    const bool producesOutput = command == "extract" || command == "zip";
    const int required = producesOutput ? 4 : 3;
    if (argc < required) {
        usage();
        return 2;
    }

    const std::filesystem::path keyPath = argv[2];
    std::filesystem::path outputPath;
    int index = 3;
    if (producesOutput) outputPath = argv[index++];

    std::string filter;
    neobif::ExtractionLayout layout = neobif::ExtractionLayout::BifAndType;
    bool overwrite = false;
    std::vector<std::filesystem::path> supplementary;
    while (index < argc) {
        const std::string argument = argv[index++];
        if (argument == "--filter") {
            if (index >= argc) {
                std::cerr << "error: --filter requires a value\n";
                return 2;
            }
            filter = argv[index++];
        } else if (argument == "--layout") {
            if (index >= argc) {
                std::cerr << "error: --layout requires a value\n";
                return 2;
            }
            layout = parseLayout(argv[index++]);
        } else if (argument == "--overwrite") {
            overwrite = true;
        } else {
            supplementary.emplace_back(argument);
        }
    }

    neobif::KeyBifArchive archive;
    if (!openArchive(archive, keyPath, supplementary)) return 1;

    if (command == "info") {
        std::cout << "KEY: " << archive.keyPath().string() << '\n'
                  << "Build date fields: year=" << archive.buildYear()
                  << " day=" << archive.buildDay() << '\n'
                  << "BIFs: " << archive.bifs().size() << '\n'
                  << "Missing BIFs: " << archive.missingBifCount() << '\n'
                  << "Resources: " << archive.resources().size() << '\n'
                  << "Extractable resources: " << archive.extractableResourceCount() << '\n'
                  << "Issues: " << archive.issues().size() << "\n\n";
        for (const auto& bif : archive.bifs()) {
            std::cout << '[' << bif.index << "] " << bif.storedPath << " | "
                      << (bif.available ? bif.resolvedPath.string() : "missing") << " | "
                      << bif.resourceIndices.size() << " indexed resources\n";
        }
        if (!archive.issues().empty()) {
            std::cout << "\nIssues:\n";
            for (const auto& issue : archive.issues()) {
                std::cout << (issue.severity == neobif::IssueSeverity::Error ? "ERROR" : "WARN")
                          << ": " << issue.message;
                if (issue.bifIndex) std::cout << " [BIF " << *issue.bifIndex << ']';
                if (issue.resourceId) std::cout << " [" << neobif::hexResourceId(*issue.resourceId) << ']';
                std::cout << '\n';
            }
        }
        return 0;
    }

    const auto selected = selectResources(archive, filter);
    if (command == "list") {
        std::cout << "bif\tresource\ttype\tid\toffset\tsize\tstatus\n";
        for (const std::size_t resourceIndex : selected) {
            const auto& resource = archive.resources()[resourceIndex];
            const std::string bifPath = resource.bifIndex < archive.bifs().size()
                ? archive.bifs()[resource.bifIndex].storedPath : "<invalid>";
            std::cout << bifPath << '\t' << resource.fileName() << '\t'
                      << neobif::resourceTypeLabel(resource.type) << '\t'
                      << neobif::hexResourceId(resource.resourceId) << '\t'
                      << resource.offset << '\t' << resource.size << '\t'
                      << resource.status << '\n';
        }
        return 0;
    }

    if (command == "zip") {
        std::string error;
        if (!archive.writeZip(selected, outputPath, layout, error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        const std::size_t included = static_cast<std::size_t>(std::count_if(
            selected.begin(), selected.end(), [&archive](std::size_t resourceIndex) {
                return resourceIndex < archive.resources().size() &&
                       archive.resources()[resourceIndex].extractable;
            }));
        std::cout << "Wrote " << outputPath.string() << " with " << included
                  << " resource(s)";
        if (included != selected.size()) {
            std::cout << "; omitted " << (selected.size() - included)
                      << " unavailable resource(s)";
        }
        std::cout << '\n';
        return 0;
    }

    const auto report = archive.extractResources(selected, outputPath, layout, overwrite);
    std::cout << "Written: " << report.written << "\nSkipped: " << report.skipped
              << "\nFailed: " << report.failed << '\n';
    for (const std::string& message : report.messages) std::cerr << message << '\n';
    return report.failed == 0u ? 0 : 1;
}
