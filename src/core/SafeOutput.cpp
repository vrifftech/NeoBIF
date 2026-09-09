#include "core/SafeOutput.hpp"
#if defined(__EMSCRIPTEN__)
#include <stdexcept>
namespace neobif {
struct SafeOutput::Impl {};
bool validExportRelativePath(const std::filesystem::path& path) {
    if(path.empty() || path.is_absolute() || path.has_root_name())return false;
    for(const auto& part:path)if(part==".."||part==".")return false;
    return true;
}
std::filesystem::path checkedExportDestination(const std::filesystem::path&,const std::filesystem::path&,
                                               const std::vector<std::filesystem::path>&) {
    throw std::runtime_error("Use the retained browser-file exporter, not native filesystem output");
}
SafeOutput::SafeOutput(const std::filesystem::path&,const std::filesystem::path&,bool,const std::vector<std::filesystem::path>&) {
    throw std::runtime_error("Native filesystem output is unavailable in the browser");
}
SafeOutput::~SafeOutput()=default;
std::ostream& SafeOutput::stream(){throw std::runtime_error("Native filesystem output is unavailable in the browser");}
void SafeOutput::commit(){throw std::runtime_error("Native filesystem output is unavailable in the browser");}
} // namespace neobif
#else
#include <neoshared/erf/Utils.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neobif {
namespace {
namespace fs = std::filesystem;
std::string folded(std::string s) {
    for (auto& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    return s;
}
[[noreturn]] void fail(const std::string& what) {
#if defined(_WIN32)
    throw std::runtime_error(what + ": " + std::system_category().message(GetLastError()));
#else
    throw std::runtime_error(what + ": " + std::generic_category().message(errno));
#endif
}
bool existsNoFollow(const fs::path& path) {
    std::error_code ec;
    const auto type = fs::symlink_status(path, ec).type();
    if (ec && ec != std::errc::no_such_file_or_directory) throw fs::filesystem_error("Inspect output", path, ec);
    return type != fs::file_type::not_found && type != fs::file_type::none;
}
void rejectLink(const fs::path& path) {
    if (fs::is_symlink(fs::symlink_status(path))) throw std::runtime_error("Output traverses a symbolic link: " + path.string());
#if defined(_WIN32)
    const auto flags = GetFileAttributesW(path.c_str());
    if (flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Output traverses a reparse point: " + path.string());
#endif
}
fs::path rootLocation(const fs::path& root) {
    // The user's chosen root is the boundary. Canonicalize its ancestors (e.g.
    // macOS /var) but never follow an output component beneath that boundary.
    const auto absolute = fs::absolute(root.empty() ? fs::path(".") : root).lexically_normal();
    if (existsNoFollow(absolute)) rejectLink(absolute);
    return fs::weakly_canonical(absolute);
}
void protect(const fs::path& destination, const std::vector<fs::path>& inputs) {
    for (const auto& input : inputs) {
        if (input.empty()) continue;
        if (neoshared::erf::paths_refer_to_same_existing_file_or_location(destination, input))
            throw std::runtime_error("Output is an input archive (or an alias of it): " + input.string());
    }
}
std::string newTempName() {
    static std::atomic<std::uint64_t> serial{0};
    std::random_device random;
    return ".neobif-" + std::to_string(static_cast<std::uint64_t>(random()) << 32u | random()) +
           "-" + std::to_string(++serial) + ".tmp";
}
#if defined(_WIN32)
using Native = HANDLE;
const Native invalidNative = INVALID_HANDLE_VALUE;
void closeNative(Native h) { if (h != invalidNative) CloseHandle(h); }
#else
using Native = int;
constexpr Native invalidNative = -1;
void closeNative(Native h) { if (h >= 0) ::close(h); }
#endif

class OutputBuffer final : public std::streambuf {
public:
    explicit OutputBuffer(Native file) : file_(file) { setp(buffer_.data(), buffer_.data()+buffer_.size()); }
    int sync() override {
        std::size_t size = static_cast<std::size_t>(pptr() - pbase());
        std::size_t done = 0;
        while (done < size) {
#if defined(_WIN32)
            DWORD count = 0;
            if (!WriteFile(file_, buffer_.data()+done, static_cast<DWORD>(size-done), &count, nullptr) || !count) return -1;
#else
            const auto count = ::write(file_, buffer_.data()+done, size-done);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) return -1;
#endif
            done += static_cast<std::size_t>(count);
        }
        position_ += size;
        setp(buffer_.data(), buffer_.data()+buffer_.size());
        return 0;
    }
    int_type overflow(int_type ch) override {
        if (sync() != 0) return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) { *pptr()=traits_type::to_char_type(ch); pbump(1); }
        return traits_type::not_eof(ch);
    }
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode mode) override {
        if (off == 0 && dir == std::ios_base::cur && (mode & std::ios_base::out))
            return pos_type(position_ + static_cast<std::uint64_t>(pptr()-pbase()));
        return pos_type(off_type(-1));
    }
private:
    Native file_;
    std::array<char, 65536> buffer_{};
    std::uint64_t position_{};
};
} // namespace

bool validExportRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;
    for (const auto& part : path) {
        const auto value = part.u8string();
        if (value.empty() || value == "." || value == ".." || value.back()=='.' || value.back()==' ') return false;
        for (unsigned char ch : value) if (ch < 32 || ch=='\\' || ch==':' || ch=='*' || ch=='?' || ch=='"' || ch=='<' || ch=='>' || ch=='|') return false;
        const auto base = folded(value.substr(0, value.find('.')));
        if (base=="con" || base=="prn" || base=="aux" || base=="nul" ||
            (base.size()==4 && (base.substr(0,3)=="com" || base.substr(0,3)=="lpt") && base[3]>='1' && base[3]<='9')) return false;
    }
    return true;
}

std::filesystem::path checkedExportDestination(const std::filesystem::path& root, const std::filesystem::path& relative,
                                               const std::vector<std::filesystem::path>& inputs) {
    if (!validExportRelativePath(relative)) throw std::runtime_error("Unsafe or empty export path: " + relative.string());
    auto current = rootLocation(root);
    for (auto it = relative.begin(); it != relative.end(); ++it) {
        fs::path component = *it;
        if (existsNoFollow(current)) {
            rejectLink(current);
            if (!fs::is_directory(current)) throw std::runtime_error("Output parent is not a directory: " + current.string());
            std::vector<fs::path> matches;
            const auto wanted = folded(component.u8string());
            for (const auto& child : fs::directory_iterator(current))
                if (folded(child.path().filename().u8string()) == wanted) matches.push_back(child.path().filename());
            if (matches.size() > 1) throw std::runtime_error("Ambiguous case variants in output: " + (current/component).string());
            if (!matches.empty()) component = matches.front();
        }
        current /= component;
        if (existsNoFollow(current)) {
            rejectLink(current);
            const bool final = std::next(it) == relative.end();
            if (final ? !fs::is_regular_file(current) : !fs::is_directory(current))
                throw std::runtime_error("Output path conflicts with a directory or nonregular file: " + current.string());
        }
    }
    protect(current, inputs);
    return current;
}

struct SafeOutput::Impl {
    fs::path root, target, temporary;
    std::string tempName;
    std::vector<fs::path> inputs;
    std::vector<Native> directories;
#if !defined(_WIN32)
    std::vector<fs::path> directoryNames;
#endif
    Native file{invalidNative};
    bool replace{}, committed{}, existed{};
    neoshared::erf::FileIdentity previous{}, owned{};
    std::unique_ptr<OutputBuffer> buffer;
    std::unique_ptr<std::ostream> output;
    ~Impl() {
        output.reset(); buffer.reset(); closeNative(file);
        if (!committed && !tempName.empty()) {
#if defined(_WIN32)
            if (owned.valid && neoshared::erf::same_regular_file_identity(temporary, owned)) DeleteFileW(temporary.c_str());
#else
            if (!directories.empty()) {
                struct stat named{};
                if(owned.valid && !::fstatat(directories.back(),tempName.c_str(),&named,AT_SYMLINK_NOFOLLOW) &&
                   static_cast<std::uint64_t>(named.st_dev)==owned.device && static_cast<std::uint64_t>(named.st_ino)==owned.inode)
                    ::unlinkat(directories.back(),tempName.c_str(),0);
            }
#endif
        }
        for (auto it=directories.rbegin(); it!=directories.rend(); ++it) closeNative(*it);
    }
    void validateParents() {
#if !defined(_WIN32)
        for (std::size_t i=1; i<directories.size(); ++i) {
            struct stat held{}, named{};
            if (::fstat(directories[i], &held) || ::fstatat(directories[i-1], directoryNames[i].c_str(), &named, AT_SYMLINK_NOFOLLOW) ||
                !S_ISDIR(named.st_mode) || held.st_dev!=named.st_dev || held.st_ino!=named.st_ino)
                throw std::runtime_error("Output directory changed during extraction; no file committed");
        }
#endif
        const auto checked = checkedExportDestination(root, target.lexically_relative(root), inputs);
        if (checked != target) throw std::runtime_error("Output path changed during extraction");
        const bool present = existsNoFollow(target);
        if (present != existed || (present && !neoshared::erf::same_regular_file_revision(target, previous)))
            throw std::runtime_error("Output changed during extraction; original/new destination was not replaced");
    }
};

SafeOutput::SafeOutput(const std::filesystem::path& root, const std::filesystem::path& relative, bool replace,
                       const std::vector<std::filesystem::path>& protectedInputs) : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    s.root = rootLocation(root); s.inputs = protectedInputs; s.replace = replace;
    s.target = checkedExportDestination(root, relative, protectedInputs);
    s.existed = existsNoFollow(s.target);
    if (s.existed && !replace) throw std::runtime_error("Output already exists: " + s.target.string());
    if (s.existed) s.previous = neoshared::erf::capture_regular_file_identity(s.target);
    const auto parent = s.target.parent_path();
#if defined(_WIN32)
    auto current = parent.root_path();
    for (const auto& part : parent.relative_path()) {
        current /= part;
        if (!existsNoFollow(current) && !CreateDirectoryW(current.c_str(), nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)
            fail("Unable to create output directory");
        HANDLE h=CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if(h==INVALID_HANDLE_VALUE) fail("Unable to pin output directory");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(h,&info) || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(h); throw std::runtime_error("Unsafe output directory/reparse point");
        }
        s.directories.push_back(h);
    }
#else
    const int first=::open(parent.root_path().c_str(), O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(first<0) fail("Unable to open filesystem root");
    s.directories.push_back(first); s.directoryNames.emplace_back();
    for (const auto& part : parent.relative_path()) {
        if (::mkdirat(s.directories.back(), part.c_str(), 0777) && errno!=EEXIST) fail("Unable to create output directory");
        const int fd=::openat(s.directories.back(), part.c_str(), O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0) fail("Unsafe output directory (links are not followed)");
        s.directories.push_back(fd); s.directoryNames.push_back(part);
    }
#endif
    for(int attempt=0; attempt<64; ++attempt) {
        const auto name = newTempName();
        const auto path = parent/name;
#if defined(_WIN32)
        s.file=CreateFileW(path.c_str(), GENERIC_WRITE|FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                           nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if(s.file==INVALID_HANDLE_VALUE) { if(GetLastError()==ERROR_FILE_EXISTS) continue; fail("Create staging file"); }
#else
        s.file=::openat(s.directories.back(),name.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
        if(s.file<0) { if(errno==EEXIST) continue; fail("Create staging file"); }
#endif
        s.tempName=name; s.temporary=path;
        #if defined(_WIN32)
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(s.file,&info))fail("Inspect owned staging file");
        s.owned.valid=true;s.owned.volume_serial=info.dwVolumeSerialNumber;
        s.owned.file_index_high=info.nFileIndexHigh;s.owned.file_index_low=info.nFileIndexLow;
#else
        struct stat info{};if(::fstat(s.file,&info))fail("Inspect owned staging file");
        s.owned.valid=true;s.owned.device=static_cast<std::uint64_t>(info.st_dev);s.owned.inode=static_cast<std::uint64_t>(info.st_ino);
#endif
        break;
    }
    if(s.file==invalidNative) throw std::runtime_error("Unable to allocate a unique staging file");
    s.buffer=std::make_unique<OutputBuffer>(s.file);
    s.output=std::make_unique<std::ostream>(s.buffer.get());
    s.validateParents();
}
SafeOutput::~SafeOutput() = default;
std::ostream& SafeOutput::stream() { return *impl_->output; }
void SafeOutput::commit() {
    auto& s=*impl_;
    s.output->flush();
    if(!*s.output) throw std::runtime_error("Unable to write complete output; previous destination preserved");
#if defined(_WIN32)
    if(!FlushFileBuffers(s.file)) fail("Flush staged output");
#else
    if(::fsync(s.file)) fail("Flush staged output");
#endif
    s.validateParents();
#if !defined(_WIN32)
    struct stat staged{};
    if(::fstatat(s.directories.back(),s.tempName.c_str(),&staged,AT_SYMLINK_NOFOLLOW) ||
       static_cast<std::uint64_t>(staged.st_dev)!=s.owned.device || static_cast<std::uint64_t>(staged.st_ino)!=s.owned.inode)
        throw std::runtime_error("Staging filename was replaced; no output committed");
#endif
#if defined(_WIN32)
    closeNative(s.file); s.file=invalidNative;
    const DWORD flags=MOVEFILE_WRITE_THROUGH|(s.replace?MOVEFILE_REPLACE_EXISTING:0);
    if(!MoveFileExW(s.temporary.c_str(),s.target.c_str(),flags)) fail("Commit output");
#else
    const int parent=s.directories.back();
    const auto leaf=s.target.filename();
    if(s.replace) {
        if(::renameat(parent,s.tempName.c_str(),parent,leaf.c_str())) fail("Commit output");
    } else {
        if(::linkat(parent,s.tempName.c_str(),parent,leaf.c_str(),0)) fail("Publish new output");
        ::unlinkat(parent,s.tempName.c_str(),0);
    }
#endif
    s.committed=true;
}
} // namespace neobif

#endif
