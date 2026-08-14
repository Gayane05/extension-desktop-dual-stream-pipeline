// desktop/src/ui/save_transcript.cpp
//
// CRT-level implementation of the contract described in save_transcript.h;
// see there for why every failure mode (open/short-write/close) must be
// caught explicitly rather than assumed to succeed.
#include "ui/save_transcript.h"

#include <cstdio>
#include <cstring>

namespace dsp {

bool saveTranscriptFile(const std::string& path, const std::string& text, std::string& error)
{
    FILE* file = nullptr;
    const errno_t openErr = fopen_s(&file, path.c_str(), "wb");
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
