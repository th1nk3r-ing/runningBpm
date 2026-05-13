/**
 * WAV 资源生成器
 *
 * 编译：cd cpp && g++ -std=c++17 -O2 -I. -o gen_wav gen_wav.cpp && ./gen_wav
 * 输出：../android/app/src/main/assets/sounds/default/tick_hi.wav, tick_lo.wav, chime.wav
 */

#include "core/WavLoader.hpp"
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    constexpr int kSampleRate = 48000;

    auto tickHi = WavGenerator::MakeWoodTick(kSampleRate, true);
    auto wavHi = WavGenerator::Generate(tickHi, kSampleRate);

    auto tickLo = WavGenerator::MakeWoodTick(kSampleRate, false);
    auto wavLo = WavGenerator::Generate(tickLo, kSampleRate);

    // chime：660+880Hz 双音，150ms 更长的延续
    int chimeLen = static_cast<int>(kSampleRate * 0.15f);
    std::vector<float> chime(chimeLen);
    for (int i = 0; i < chimeLen; ++i) {
        float t = static_cast<float>(i) / kSampleRate;
        chime[i] = (std::sin(2.0f * 3.14159265f * 660.0f * t)
                  + std::sin(2.0f * 3.14159265f * 880.0f * t)) * 0.5f
                  * std::exp(-t * 15.0f);
    }
    auto wavChime = WavGenerator::Generate(chime, kSampleRate);

    auto writeFile = [](const std::string& path, const std::vector<uint8_t>& data) {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
        printf("Wrote %s: %zu bytes\n", path.c_str(), data.size());
    };

    std::string outDir = "../android/app/src/main/assets/sounds/default";
    writeFile(outDir + "/tick_hi.wav", wavHi);
    writeFile(outDir + "/tick_lo.wav", wavLo);
    writeFile(outDir + "/chime.wav", wavChime);

    printf("\nDone.\n");
    return 0;
}