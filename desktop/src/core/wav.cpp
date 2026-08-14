// desktop/src/core/wav.cpp
#include "core/wav.h"

#include <cstring>
#include <fstream>

namespace dsp {

// RIFF files are a flat sequence of [4-byte id][4-byte size][payload]
// chunks after the 12-byte "RIFF"+size+"WAVE" header. We only care about
// "fmt " (codec/rate/bits) and "data" (samples); every other chunk (e.g.
// "LIST", "fact") is skipped by its declared size so this walk works
// regardless of which optional chunks a given WAV writer included.
std::optional<WavData> readWavPcm16Mono(const std::string& path, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "cannot open " + path;
        return std::nullopt;
    }
    char riff[4], wave[4];
    uint32_t riffSize = 0;
    file.read(riff, 4);
    file.read(reinterpret_cast<char*>(&riffSize), 4);
    file.read(wave, 4);
    if (!file || std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0)
    {
        error = "not a RIFF/WAVE file";
        return std::nullopt;
    }
    WavData out;
    uint16_t channels = 0, bits = 0, fmt = 0;
    while (file)
    {
        char id[4];
        uint32_t size = 0;
        file.read(id, 4);
        file.read(reinterpret_cast<char*>(&size), 4);
        if (!file)
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
            file.read(buf.data(), size);
            if (!file)
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
            file.read(reinterpret_cast<char*>(out.samples.data()), size);
            if (!file)
            {
                error = "truncated data chunk";
                return std::nullopt;
            }
            return out;
        }
        else
        {
            // RIFF pads odd-sized chunk payloads with one extra byte so the
            // next chunk header always starts on a 2-byte boundary; that pad
            // byte isn't part of `size`, so skip size+1 (not size) to land on
            // the next chunk's id rather than one byte short of it.
            file.seekg(size + (size % 2), std::ios::cur);  // chunks are word-aligned
        }
    }
    error = "no data chunk";
    return std::nullopt;
}

}  // namespace dsp
