#include "core/KeyBifArchive.hpp"
#include "core/ArchiveExport.hpp"
#include <csignal>
#include <map>
#include <stdexcept>
#include "core/Version.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
volatile std::sig_atomic_t cancelled=0;
void cancelSignal(int) {cancelled=1;}


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
        << "  --overwrite          Replace existing outputs (never input archives)\n"
        << "  --keep-both          Explicitly suffix conflicting extraction names\n"
        << "  --relocate INDEX BIF Explicitly map a KEY BIF-table index to a file\n";
}

neobif::ExtractionLayout parseLayout(const std::string& value) {
    const std::string lower = lowerAscii(value);
    if (lower == "bif") return neobif::ExtractionLayout::Bif;
    if (lower == "type") return neobif::ExtractionLayout::Type;
    if (lower == "flat") return neobif::ExtractionLayout::Flat;
    if(lower=="bif-type") return neobif::ExtractionLayout::BifAndType;
    throw std::runtime_error("Unknown output layout: "+value);
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
                 const std::vector<std::filesystem::path>& bifs,
                 const std::map<std::size_t,std::filesystem::path>& relocations, const neobif::JobControl& job) {
    if (archive.open(key, bifs,relocations,job)) return true;
    std::cerr << "error: " << archive.lastError() << '\n';
    return false;
}

} // namespace

int run(int argc, char** argv) {
    neobif::JobControl job;job.cancelled=[]{return cancelled!=0;};
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
        const auto keys = neobif::KeyBifArchive::scanForKeyFiles(argv[2],64,job);
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
    bool overwrite = false,keepBoth=false;
    std::map<std::size_t,std::filesystem::path> relocations;
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
        } else if (argument == "--keep-both") {
            keepBoth=true;
        } else if (argument == "--relocate") {
            if(index+1>=argc)throw std::runtime_error("--relocate needs INDEX and BIF path");
            std::string text=argv[index++];std::size_t used=0;
            const auto number=std::stoull(text,&used);
            if(used!=text.size()||text.empty()||text[0]=='-')throw std::runtime_error("Invalid KEY BIF-table index");
            relocations[static_cast<std::size_t>(number)]=argv[index++];
        } else if(argument.rfind("--",0)==0) {
            throw std::runtime_error("Unknown option: "+argument);
        } else {
            supplementary.emplace_back(argument);
        }
    }

    if(overwrite&&keepBoth)throw std::runtime_error("Choose either --overwrite or --keep-both");
    neobif::KeyBifArchive archive;
    if (!openArchive(archive, keyPath, supplementary,relocations,job)) return 1;

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

    neobif::ExportOptions options;options.job=job;options.protectedInputs=archive.inputPaths();
    options.existing=overwrite?neobif::ExistingPolicy::Replace:(keepBoth?neobif::ExistingPolicy::KeepBoth:neobif::ExistingPolicy::Skip);
    const auto paths=archive.outputPaths(selected,layout);
    std::vector<neobif::ExportItem> items;
    for(std::size_t position=0;position<selected.size();++position) {
        const auto index=selected[position];const auto& resource=archive.resources().at(index);
        if(!resource.extractable) {std::cerr<<"Held unavailable resource: "<<resource.fileName()<<" ("<<resource.status<<")\n";continue;}
        neobif::ExportItem item;item.relativePath=paths.at(position);item.displayName=resource.fileName();
        item.identitySuffix=neobif::hexResourceId(resource.resourceId);item.expectedSize=resource.size;
        item.sourcePaths={archive.keyPath(),archive.bifs().at(resource.bifIndex).resolvedPath};
        item.stream=[&archive,index](const neobif::ByteSink& sink,std::string& error,const neobif::JobControl& control){return archive.streamResource(index,sink,error,control);};
        items.push_back(std::move(item));
    }
    const auto unavailable=selected.size()-items.size();
    if(items.empty())throw std::runtime_error("No extractable matching resources");
    if(command=="zip") {
        if(unavailable)throw std::runtime_error("ZIP selection contains unavailable resources; narrow the selection or repair the inputs first");
        std::string error;
        if(!neobif::writeExportZip(items,outputPath,error,options))throw std::runtime_error(error);
        std::cout<<"Wrote "<<outputPath.string()<<" with "<<items.size()<<" resources\n";
        if(keepBoth)std::cout<<"Keep-both naming was explicitly requested.\n";
        return 0;
    }
    auto report=neobif::extractExportItems(items,outputPath,options);
    report.failed+=unavailable;
    std::cout << "Written: " << report.written << "\nSkipped: " << report.skipped
              << "\nFailed: " << report.failed << '\n';
    for (const std::string& message : report.messages) std::cerr << message << '\n';
    return report.cancelled ? 130 : (report.failed == 0u ? 0 : 1);
}

int main(int argc,char** argv) {
    std::signal(SIGINT,cancelSignal);
    try {return run(argc,argv);}
    catch(const neobif::JobCancelled&) {std::cerr<<"Cancelled; completed files retained.\n";return 130;}
    catch(const std::exception& ex) {std::cerr<<"error: "<<ex.what()<<'\n';return 1;}
}
