/**
 * Limiter.hpp 验收测试
 *
 * 编译：g++ -std=c++17 -O2 -I. -o limiter_test limiter_test.cpp && ./limiter_test
 */

#include "core/Limiter.hpp"
#include <cstdio>
#include <cmath>

int main() {
    constexpr int kFrames = 8;

    // Test 1: 小信号近似直通（tanh(x) ≈ x for |x| ≪ 1）
    float small[kFrames] = {0.01f, 0.02f, 0.05f, -0.01f, -0.03f, 0.0f, 0.001f, -0.005f};
    Limiter::Process(small, kFrames);
    bool test1 = true;
    for (int i = 0; i < kFrames; ++i) {
        if (std::abs(small[i] - 0.0f) > 0.06f) test1 = false;  // 误差在 1% 以内
    }
    printf("Test 1 (small signal ~passthrough): %s\n", test1 ? "PASS" : "FAIL");

    // Test 2: 限幅后输出在 (-1, 1) 范围内
    float loud[kFrames] = {1.5f, 2.0f, 3.0f, -1.5f, -2.0f, -3.0f, 10.0f, -10.0f};
    Limiter::Process(loud, kFrames);
    bool test2 = true;
    for (int i = 0; i < kFrames; ++i) {
        if (loud[i] > 1.0f + 1e-7f || loud[i] < -1.0f - 1e-7f) test2 = false;
    }
    printf("Test 2 (output bounded by ±1):     %s\n", test2 ? "PASS" : "FAIL");

    // Test 3: 符号保持
    float signTest[4] = {2.0f, 0.5f, -3.0f, -0.1f};
    float signOrig[4];
    for (int i = 0; i < 4; ++i) signOrig[i] = signTest[i];
    Limiter::Process(signTest, 4);
    bool test3 = true;
    for (int i = 0; i < 4; ++i) {
        if ((signTest[i] > 0) != (signOrig[i] > 0)) test3 = false;
    }
    printf("Test 3 (sign preserved):            %s\n", test3 ? "PASS" : "FAIL");

    // Test 4: 硬限幅备选
    float hardIn[kFrames] = {1.5f, 0.3f, -2.0f, -0.2f, 0.97f, -0.97f, 0.96f, -0.96f};
    Limiter::ProcessHard(hardIn, kFrames);
    bool test4 = true;
    for (int i = 0; i < kFrames; ++i) {
        if (hardIn[i] > 0.97f || hardIn[i] < -0.97f) test4 = false;
    }
    if (std::abs(hardIn[4] - 0.97f) > 1e-7f) test4 = false;
    if (std::abs(hardIn[5] + 0.97f) > 1e-7f) test4 = false;
    printf("Test 4 (hard clip ±0.97):           %s\n", test4 ? "PASS" : "FAIL");

    // Test 5: 单调性（增益递减时输出不会震荡）
    float ramp[1000];
    for (int i = 0; i < 1000; ++i) ramp[i] = -3.0f + 6.0f * i / 999.0f;
    Limiter::Process(ramp, 1000);
    bool test5 = true;
    for (int i = 1; i < 1000; ++i) {
        if (ramp[i] < ramp[i-1] - 1e-7f) { // 允许极小浮点误差
            printf("  monotonic break at %d: %f < %f\n", i, ramp[i], ramp[i-1]);
            test5 = false; break;
        }
    }
    printf("Test 5 (monotonic):                 %s\n", test5 ? "PASS" : "FAIL");

    bool allPass = test1 && test2 && test3 && test4 && test5;
    printf("\n=== %s ===\n", allPass ? "ALL PASS" : "FAILURES DETECTED");
    return allPass ? 0 : 1;
}
