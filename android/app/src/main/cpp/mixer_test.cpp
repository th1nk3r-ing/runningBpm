/**
 * Mixer.hpp 验收测试
 *
 * 编译：g++ -std=c++17 -O2 -I. -o mixer_test mixer_test.cpp && ./mixer_test
 */

#include "core/Mixer.hpp"
#include <cstdio>
#include <cmath>

int main() {
    constexpr int kFrames = 8;
    float tick[kFrames]   = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f, 0.8f, 0.4f, 0.2f};
    float chime[kFrames]  = {1.0f, 0.5f, 0.0f, 0.5f, 1.0f, 0.5f, 0.0f, 0.5f};
    float out[kFrames];

    // Test 1: tickGain=1.0, chimeGain=0.0 → 仅输出 tick
    Mixer::Process(tick, chime, out, kFrames, 1.0f, 0.0f);
    bool test1 = true;
    for (int i = 0; i < kFrames; ++i) {
        if (std::abs(out[i] - tick[i]) > 1e-7f) test1 = false;
    }
    printf("Test 1 (chime mute, tick only): %s\n", test1 ? "PASS" : "FAIL");

    // Test 2: tickGain=0.0, chimeGain=1.0 → 仅输出 chime
    Mixer::Process(tick, chime, out, kFrames, 0.0f, 1.0f);
    bool test2 = true;
    for (int i = 0; i < kFrames; ++i) {
        if (std::abs(out[i] - chime[i]) > 1e-7f) test2 = false;
    }
    printf("Test 2 (tick mute, chime only): %s\n", test2 ? "PASS" : "FAIL");

    // Test 3: 各 0.5 增益 → out = tick*0.5 + chime*0.5
    Mixer::Process(tick, chime, out, kFrames, 0.5f, 0.5f);
    bool test3 = true;
    for (int i = 0; i < kFrames; ++i) {
        float expected = tick[i] * 0.5f + chime[i] * 0.5f;
        if (std::abs(out[i] - expected) > 1e-7f) test3 = false;
    }
    printf("Test 3 (both gain 0.5):         %s\n", test3 ? "PASS" : "FAIL");

    // Test 4: 不同增益
    Mixer::Process(tick, chime, out, kFrames, 0.3f, 0.7f);
    bool test4 = true;
    for (int i = 0; i < kFrames; ++i) {
        float expected = tick[i] * 0.3f + chime[i] * 0.7f;
        if (std::abs(out[i] - expected) > 1e-7f) test4 = false;
    }
    printf("Test 4 (tick=0.3, chime=0.7):  %s\n", test4 ? "PASS" : "FAIL");

    // Test 5: 全零输入
    float zero[kFrames] = {};
    Mixer::Process(zero, zero, out, kFrames, 1.0f, 1.0f);
    bool test5 = true;
    for (int i = 0; i < kFrames; ++i) if (out[i] != 0.0f) test5 = false;
    printf("Test 5 (all zero input):        %s\n", test5 ? "PASS" : "FAIL");

    bool allPass = test1 && test2 && test3 && test4 && test5;
    printf("\n=== %s ===\n", allPass ? "ALL PASS" : "FAILURES DETECTED");
    return allPass ? 0 : 1;
}
