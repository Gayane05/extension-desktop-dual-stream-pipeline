#include "core/wav.h"

#include <cstring>
#include <fstream>

namespace dsp {

std::optional<WavData> readWavPcm16Mono(const std::string& path, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        error = "cannot open " + path;
        return std::nullopt;
    }
    char riff[4], wave[4];
    uint32_t riffSize = 0;
    f.read(riff, 4);
    f.read(reinterpret_cast<char*>(&riffSize), 4);
    f.read(wave, 4);
    if (!f || std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0)
    {
        error = "not a RIFF/WAVE file";
        return std::nullopt;
    }
    WavData out;
    uint16_t channels = 0, bits = 0, fmt = 0;
    while (f)
    {
        char id[4];
        uint32_t size = 0;
        f.read(id, 4);
        f.read(reinterpret_cast<char*>(&size), 4);
        if (!f)
        {
            break;
        }
        if (std::strncmp(id, "fmt ", 4) == 0)
        {
            // A canonical fmt chunk is 16 bytes (bits-per-sample sits at
            // offset 14); a shorter chunk would make the memcpy below read
            // past the end of buf. Reject it explicitly instead of reading
            // out of bounds.
            if (size < 16)
            {
                error = "fmt chunk too small";
                return std::nullopt;
            }
            std::vector<char> buf(size);
            f.read(buf.data(), size);
            if (!f)
            {
                error = "truncated fmt chunk";
                return std::nullopt;
            }
            std::memcpy(&fmt, buf.data(), 2);
            std::memcpy(&channels, buf.data() + 2, 2);
            uint32_t rate;
            std::memcpy(&rate, buf.data() + 4, 4);
            out.sampleRate = static_cast<int>(rate);
            std::memcpy(&bits, buf.data() + 14, 2);
        }
        else if (std::strncmp(id, "data", 4) == 0)
        {
            if (fmt != 1 || channels != 1 || bits != 16)
            {
                error = "need PCM16 mono (got fmt=" + std::to_string(fmt) +
                        " ch=" + std::to_string(channels) + " bits=" + std::to_string(bits) + ")";
                return std::nullopt;
            }
            out.samples.resize(size / 2);
            f.read(reinterpret_cast<char*>(out.samples.data()), size);
            if (!f)
            {
                error = "truncated data chunk";
                return std::nullopt;
            }
            return out;
        }
        else
        {
            f.seekg(size + (size % 2), std::ios::cur);  // chunks are word-aligned
        }
    }
    error = "no data chunk";
    return std::nullopt;
}

}  // namespace dsp
