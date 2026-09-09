#pragma once
#include "core/JobControl.hpp"
#include <neoshared/erf/Utils.hpp>
#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace neobif {
using InputSnapshot = neoshared::erf::FileIdentity;
inline bool unchangedInput(const std::filesystem::path& path, const InputSnapshot& snapshot) {
    return neoshared::erf::same_regular_file_revision(path, snapshot);
}
inline bool streamInputRange(const std::filesystem::path& path, const InputSnapshot& snapshot,
                             std::uint64_t offset, std::uint64_t size, const ByteSink& sink,
                             std::string& error, const JobControl& job = {}) {
    if (!unchangedInput(path, snapshot)) {
        error = "Archive changed after indexing; rescan before extracting: " + path.string();
        return false;
    }
    if (offset > snapshot.size || size > snapshot.size - offset || !sink) {
        error = "Invalid indexed resource range"; return false;
    }
    job.check();
#if defined(_WIN32)
    // Deny writers and renames while this particular range is being streamed.
    struct File { HANDLE value; ~File(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);} } file{
        CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    if(file.value==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file.value,&info) ||
       GetFileType(file.value)!=FILE_TYPE_DISK || (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
       info.dwVolumeSerialNumber!=snapshot.volume_serial || info.nFileIndexHigh!=snapshot.file_index_high || info.nFileIndexLow!=snapshot.file_index_low) {
        error="Archive changed, is in use, or could not be reopened; rescan: "+path.string();return false;
    }
    LARGE_INTEGER position{};position.QuadPart=static_cast<LONGLONG>(offset);
    if(!SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN)){error="Unable to seek archive resource";return false;}
#else
    // Nonblocking open prevents an exchanged FIFO from hanging a worker.
    struct File {int value;~File(){if(value>=0)::close(value);}} file{
        ::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC)};
    struct stat info{};
    if(file.value<0 || ::fstat(file.value,&info) || !S_ISREG(info.st_mode) || info.st_size<0 ||
       static_cast<std::uint64_t>(info.st_dev)!=snapshot.device || static_cast<std::uint64_t>(info.st_ino)!=snapshot.inode ||
       static_cast<std::uint64_t>(info.st_size)!=snapshot.size) {
        error="Archive changed or could not be reopened; rescan: "+path.string();return false;
    }
    if(::lseek(file.value,static_cast<off_t>(offset),SEEK_SET)<0){error="Unable to seek archive resource";return false;}
#endif
    if(!unchangedInput(path,snapshot)){error="Archive changed while reopening; rescan";return false;}
    std::array<std::uint8_t,256u*1024u> buffer{};
    for(std::uint64_t remaining=size;remaining;) {
        job.check();const auto count=static_cast<std::size_t>(std::min<std::uint64_t>(remaining,buffer.size()));
#if defined(_WIN32)
        DWORD got=0;if(!ReadFile(file.value,buffer.data(),static_cast<DWORD>(count),&got,nullptr) || got==0){error="Unable to read complete resource";return false;}
#else
        const auto got=::read(file.value,buffer.data(),count);
        if(got<0 && errno==EINTR)continue;
        if(got<=0){error="Unable to read complete resource";return false;}
#endif
        if(!sink(buffer.data(),static_cast<std::size_t>(got),error))return false;
        remaining-=static_cast<std::uint64_t>(got);
    }
    if (!unchangedInput(path, snapshot)) {
        error = "Archive changed during extraction; output was not committed. Rescan: " + path.string();
        return false;
    }
    return true;
}
} // namespace neobif
