/**
 * SamplePlayer.hpp 验收测试
 *
 * 编译：g++ -std=c++17 -O2 -I. -o sample_player_test sample_player_test.cpp && ./sample_player_test
 */

#include "../core/SamplePlayer.hpp"
#include <cstdio>
#include <cmath>

int main() {
    // 生成一个短脉冲样本：440Hz 正弦波 100 帧
    constexpr int kSampleLen = 100;
    float pulse[kSampleLen];
    for (int i = 0; i < kSampleLen; ++i) {
        pulse[i] = std::sin(2.0f * 3.14159f * 440.0f * i / 48000.0f);
    }

    SamplePlayer player;
    player.Load(pulse, kSampleLen);

    // Test 1：播放前 Render 输出全零
    float buf1[50] = {1.0f};  // 非零初始值
    player.Render(buf1, 50, 1.0f);
    bool allZero = true;
    for (int i = 0; i < 50; ++i) if (buf1[i] != 0.0f) allZero = false;
    printf("Test 1 (not playing → silence): %s\n", allZero ? "PASS" : "FAIL");

    // Test 2：Play 后 Render 输出非零
    player.Play();
    float buf2[50];
    player.Render(buf2, 50, 1.0f);
    bool hasSignal = false;
    for (int i = 0; i < 50; ++i) if (buf2[i] != 0.0f) hasSignal = true;
    printf("Test 2 (playing → has signal):  %s\n", hasSignal ? "PASS" : "FAIL");

    // Test 3：播放完毕后 Render 输出全零
    float buf3[100];
    player.Render(buf3, 100, 1.0f);
    // 样本长度 100，已读 50 + 此处 readPos 从 50 开始
    // 前 50 帧应非零，后 50 帧应全零
    bool firstHalfSignal = false;
    bool secondHalfZero = true;
    for (int i = 0; i < 50; ++i) if (buf3[i] != 0.0f) firstHalfSignal = true;
    for (int i = 50; i < 100; ++i) if (buf3[i] != 0.0f) secondHalfZero = false;
    printf("Test 3 (end → auto silence):     %s\n",
           (firstHalfSignal && secondHalfZero) ? "PASS" : "FAIL");

    // Test 4：播放完毕后再 Render 全零
    float buf4[10];
    player.Render(buf4, 10, 1.0f);
    bool finalZero = true;
    for (int i = 0; i < 10; ++i) if (buf4[i] != 0.0f) finalZero = false;
    printf("Test 4 (finished → silence):     %s\n", finalZero ? "PASS" : "FAIL");

    // Test 5：gain 系数生效
    player.Play();
    float buf5a[10], buf5b[10];
    player.Render(buf5a, 10, 1.0f);
    float buf5c[10];
    player.Render(buf5c, 10, 1.0f);  // 继续消耗到 readPos=20
    // 重新播放测试 gain
    player.Play();
    player.Render(buf5b, 10, 0.5f);
    bool gainWorks = true;
    for (int i = 0; i < 10; ++i) {
        if (std::abs(buf5b[i] - buf5a[i] * 0.5f) > 1e-7f) gainWorks = false;
    }
    printf("Test 5 (gain scaling):           %s\n", gainWorks ? "PASS" : "FAIL");

    printf("\n=== %s ===\n",
           (allZero && hasSignal && firstHalfSignal && secondHalfZero && finalZero && gainWorks)
           ? "ALL PASS" : "FAILURES DETECTED");
    return 0;
}
