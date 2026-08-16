// desktop/src/ui/save_transcript.cpp
//
// CRT-level implementation of the contract described in save_transcript.h;
// see there for why every failure mode (open/short-write/close) must be
// caught explicitly rather than assumed to succeed.
#include "ui/save_transcript.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>
#endif

namespace dsp
{

// Opens `path` for binary writing. The narrow CRT open uses the ANSI code
// page, which mangles UTF-8 paths from the Save As dialog (e.g. a non-ASCII
// user or folder name); on Windows, convert to a wide path and _wfopen_s.
static errno_t openForWrite(const std::string& path, FILE** file)
{
#ifdef _WIN32
    const int wideLen =
        ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), nullptr, 0);
    if (wideLen > 0)
    {
        std::vector<wchar_t> widePath(static_cast<size_t>(wideLen) + 1, L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                              widePath.data(), wideLen);
        return _wfopen_s(file, widePath.data(), L"wb");
    }
#endif
    return fopen_s(file, path.c_str(), "wb");
}

bool saveTranscriptFile(const std::string& path, const std::string& text, std::string& error)
{
    FILE* file = nullptr;
    const errno_t openErr = openForWrite(path, &file);
    if (openErr != 0 || file == nullptr)
    {
        char errMsg[256] = {};
        strerror_s(errMsg, sizeof(errMsg), openErr);
        error = std::string("open failed: ") + errMsg;
        return false;
    }

    const size_t written = fwrite(text.data(), 1, text.size(), file);
    if (written != text.size())
    {
        std::fclose(file);
        error = "short write (disk full?)";
        return false;
    }

    if (std::fclose(file) != 0)
    {
        error = "close failed";
        return false;
    }

    return true;
}

}  // namespace dsp
