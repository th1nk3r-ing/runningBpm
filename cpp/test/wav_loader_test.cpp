#include "../core/WavLoader.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void AppendFourCC(std::vector<uint8_t>& out, const char* value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value[i]));
}

void Append16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void Append32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void Set32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

std::vector<uint8_t> MakeOddChunkWav() {
    std::vector<uint8_t> wav;
    AppendFourCC(wav, "RIFF");
    Append32(wav, 0); // Filled below.
    AppendFourCC(wav, "WAVE");

    AppendFourCC(wav, "JUNK");
    Append32(wav, 3);
    wav.insert(wav.end(), {0x11, 0x22, 0x33, 0x00}); // Odd payload + RIFF padding.

    AppendFourCC(wav, "fmt ");
    Append32(wav, 16);
    Append16(wav, 1);       // PCM
    Append16(wav, 1);       // mono
    Append32(wav, 48000);
    Append32(wav, 96000);
    Append16(wav, 2);
    Append16(wav, 16);

    AppendFourCC(wav, "data");
    Append32(wav, 2);
    Append16(wav, 32767);
    Set32(wav, 4, static_cast<uint32_t>(wav.size() - 8));
    return wav;
}

std::vector<uint8_t> MakeTruncatedFormatWav() {
    std::vector<uint8_t> wav;
    AppendFourCC(wav, "RIFF");
    Append32(wav, 44); // Declares an end at byte 52.
    AppendFourCC(wav, "WAVE");

    AppendFourCC(wav, "JUNK");
    Append32(wav, 12);
    wav.insert(wav.end(), 12, 0);

    AppendFourCC(wav, "fmt ");
    Append32(wav, 16);
    wav.insert(wav.end(), 12, 0); // Only 12 of the declared 16 fmt bytes exist.
    return wav;
}

} // namespace

int main() {
    WavLoader loader;
    const auto oddChunkWav = MakeOddChunkWav();
    const bool oddChunkAccepted = loader.Load(oddChunkWav.data(), oddChunkWav.size());
    const bool sampleDecoded = oddChunkAccepted && loader.GetSamples().size() == 1 &&
            loader.GetSamples()[0] > 0.99f;

    const auto truncatedWav = MakeTruncatedFormatWav();
    const bool truncatedRejected = !loader.Load(truncatedWav.data(), truncatedWav.size());

    std::printf("Odd RIFF chunk padding: %s\n", oddChunkAccepted ? "PASS" : "FAIL");
    std::printf("Truncated fmt chunk rejected: %s\n", truncatedRejected ? "PASS" : "FAIL");
    return sampleDecoded && truncatedRejected ? 0 : 1;
}
