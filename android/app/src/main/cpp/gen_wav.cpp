/**
 * WAV 资源生成器
 *
 * 编译：g++ -std=c++17 -O2 -I. -o gen_wav gen_wav.cpp && ./gen_wav
 * 输出：../res/raw/tick_hi.wav, tick_lo.wav, chime.wav
 */

#include "core/WavLoader.hpp"
#include <cstdio>
#include <fstream>

int main() {
    constexpr int kSampleRate = 48000;

    // tick_hi：1000Hz 强拍，较长的延续感
    auto tickHi = WavGenerator::MakeTick(kSampleRate, 1000.0f, 0.04f, 70.0f);
    auto wavHi = WavGenerator::Generate(tickHi, kSampleRate);

    // tick_lo：600Hz 弱拍
    auto tickLo = WavGenerator::MakeTick(kSampleRate, 600.0f, 0.03f, 80.0f);
    auto wavLo = WavGenerator::Generate(tickLo, kSampleRate);

    // chime：660+880Hz 双音，较长
    int chimeLen = static_cast<int>(kSampleRate * 0.15f);
    std::vector<float> chime(chimeLen);
    for (int i = 0; i < chimeLen; ++i) {
        float t = static_cast<float>(i) / kSampleRate;
        chime[i] = (std::sin(2.0f * 3.14159265f * 660.0f * t)
                  + std::sin(2.0f * 3.14159265f * 880.0f * t)) * 0.5f
                  * std::exp(-t * 20.0f);
    }
    auto wavChime = WavGenerator::Generate(chime, kSampleRate);

    // 写入
    auto writeFile = [](const std::string& path, const std::vector<uint8_t>& data) {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), data.size());
        printf("Wrote %s: %zu bytes\n", path.c_str(), data.size());
    };

    std::string outDir = "/Users/thinker/Desktop/work/runningBpm/android/app/src/main/res/raw";
    writeFile(outDir + "/tick_hi.wav", wavHi);
    writeFile(outDir + "/tick_lo.wav", wavLo);
    writeFile(outDir + "/chime.wav", wavChime);

    printf("\nDone.\n");
    return 0;
}
