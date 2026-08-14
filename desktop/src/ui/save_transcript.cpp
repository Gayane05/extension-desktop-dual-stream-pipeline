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
    FILE* f = nullptr;
    const errno_t openErr = fopen_s(&f, path.c_str(), "wb");
    if (openErr != 0 || f == nullptr)
    {
        char msg[256] = {};
        strerror_s(msg, sizeof(msg), openErr);
        error = std::string("open failed: ") + msg;
        return false;
    }

    const size_t written = fwrite(text.data(), 1, text.size(), f);
    if (written != text.size())
    {
        std::fclose(f);
        error = "short write (disk full?)";
        return false;
    }

    if (std::fclose(f) != 0)
    {
        error = "close failed";
        return false;
    }

    return true;
}

}  // namespace dsp
