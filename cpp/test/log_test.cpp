/** Log.hpp 验收测试：文件开关、级别过滤、Shutdown 后重用。 */

#include "../core/Log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool FileExists(const std::string& path) {
    std::ifstream in(path);
    return in.good();
}

std::size_t FileSize(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    return in ? static_cast<std::size_t>(in.tellg()) : 0;
}

std::string NewDir() {
    char dirTemplate[] = "/tmp/runbeat-log-test-XXXXXX";
    const char* dir = mkdtemp(dirTemplate);
    return dir != nullptr ? std::string(dir) : std::string();
}

} // namespace

int main() {
    char dirTemplate[] = "/tmp/runbeat-log-test-XXXXXX";
    const char* dir = mkdtemp(dirTemplate);
    if (dir == nullptr) {
        printf("mkdtemp failed\n");
        return 1;
    }
    const std::string logDir = dir;
    const std::string logPath = logDir + "/runbeat.log";

    beat::log::Init({logDir, spdlog::level::trace, std::size_t(1) << 20, 3, false});
    beat::log::Get("RunBeat-Native")->info("disabled-start");
    beat::log::Shutdown();
    const bool noFileCreated = !FileExists(logPath);
    printf("disabled start creates no file: %s\n", noFileCreated ? "yes" : "no");

    beat::log::Init({logDir, spdlog::level::trace, std::size_t(1) << 20, 3, true});
    beat::log::Get("RunBeat-Native")->info("first");
    beat::log::SetFileEnabled(false);
    beat::log::Get("RunBeat-Native")->info("second");
    beat::log::SetFileEnabled(true);
    beat::log::Get("RunBeat-Native")->info("third");
    beat::log::Shutdown();

    const std::string content = ReadFile(logPath);
    const bool firstKept = Contains(content, "first");
    const bool secondDropped = !Contains(content, "second");
    const bool thirdKept = Contains(content, "third");
    const bool patternOk = Contains(content, "[RunBeat-Native]");
    printf("toggle: first kept=%s, disabled dropped=%s, re-enabled kept=%s, pattern=%s\n",
           firstKept ? "yes" : "no", secondDropped ? "yes" : "no", thirdKept ? "yes" : "no",
           patternOk ? "yes" : "no");

    beat::log::Init({logDir, spdlog::level::warn, std::size_t(1) << 20, 3, true});
    beat::log::Get("RunBeat-Native")->info("filtered-info");
    beat::log::Get("RunBeat-Native")->warn("kept-warn");
    beat::log::Shutdown();
    const std::string filtered = ReadFile(logPath);
    const bool levelOk = !Contains(filtered, "filtered-info") && Contains(filtered, "kept-warn");
    printf("level filter: %s\n", levelOk ? "yes" : "no");

    // Shutdown 后 spdlog 无 default logger，取用与再次 Init 都不得崩溃
    beat::log::Get("RunBeat-Native")->info("after-shutdown");
    beat::log::Init({logDir, spdlog::level::trace, std::size_t(1) << 20, 3});
    beat::log::Get("New-Tag")->info("after-reinit");
    beat::log::Shutdown();
    const bool reinitOk = Contains(ReadFile(logPath), "after-reinit");
    printf("re-init after shutdown: %s\n", reinitOk ? "yes" : "no");

    // 轮转：8 KiB × 3，写入远超容量
    const std::string rotDir = NewDir();
    const std::size_t rotBytes = std::size_t(8) << 10;
    beat::log::Init({rotDir, spdlog::level::trace, rotBytes, 3, true});
    for (int i = 0; i < 2000; ++i) {
        beat::log::Get("Rotate")->info("rotate-{:05d}-padding-padding-padding-padding", i);
    }
    beat::log::Shutdown();
    const std::size_t cap = rotBytes + 256;
    const bool rotated = FileExists(rotDir + "/runbeat.1.log") && FileExists(rotDir + "/runbeat.2.log");
    const bool capped = FileSize(rotDir + "/runbeat.log") <= cap
                        && FileSize(rotDir + "/runbeat.1.log") <= cap;
    const bool newestKept = Contains(ReadFile(rotDir + "/runbeat.log"), "rotate-01999-");
    const bool oldestDropped = !Contains(ReadFile(rotDir + "/runbeat.2.log"), "rotate-00000-");
    printf("rotate: files=%s capped=%s newest=%s oldest-dropped=%s\n",
           rotated ? "yes" : "no", capped ? "yes" : "no", newestKept ? "yes" : "no",
           oldestDropped ? "yes" : "no");

    // 关开关时先 flush，不依赖 Shutdown
    const std::string flushDir = NewDir();
    beat::log::Init({flushDir, spdlog::level::trace, std::size_t(1) << 20, 3, true});
    beat::log::Get("Flush")->info("flush-before-unbind");
    beat::log::SetFileEnabled(false);
    const bool flushed = Contains(ReadFile(flushDir + "/runbeat.log"), "flush-before-unbind");
    beat::log::Shutdown();
    printf("flush on unbind: %s\n", flushed ? "yes" : "no");

    // 关闭不建文件；打开即产出空文件
    const std::string emptyDir = NewDir();
    beat::log::Init({emptyDir, spdlog::level::trace, std::size_t(1) << 20, 3, false});
    const bool noFileWhileOff = !FileExists(emptyDir + "/runbeat.log");
    beat::log::SetFileEnabled(true);
    const bool emptyFileOnEnable = FileExists(emptyDir + "/runbeat.log")
                                   && FileSize(emptyDir + "/runbeat.log") == 0;
    beat::log::Get("Empty")->info("after-enable");
    beat::log::SetFileEnabled(false);
    const bool wroteAfterEnable = FileSize(emptyDir + "/runbeat.log") > 0;
    beat::log::Shutdown();
    printf("toggle: off-creates-nothing=%s, enable-creates-empty-file=%s, writes-after=%s\n",
           noFileWhileOff ? "yes" : "no", emptyFileOnEnable ? "yes" : "no",
           wroteAfterEnable ? "yes" : "no");

    // 并发写 + 并发开关：行完整性与线程内顺序
    const std::string stressDir = NewDir();
    beat::log::Init({stressDir, spdlog::level::trace, std::size_t(32) << 20, 3, true});
    std::atomic<bool> stopToggle{false};
    std::atomic<int> toggleCount{0};
    std::thread toggler([&stopToggle, &toggleCount] {
        while (!stopToggle.load(std::memory_order_relaxed)) {
            beat::log::SetFileEnabled(false);
            beat::log::SetFileEnabled(true);
            toggleCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    constexpr int kThreads = 4;
    constexpr int kLines = 20000;
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([t] {
            for (int i = 0; i < kLines; ++i) beat::log::Get("Stress")->info("stress-{}-{}", t, i);
        });
    }
    for (auto& writer : writers) writer.join();
    stopToggle.store(true);
    toggler.join();
    beat::log::SetFileEnabled(true);
    beat::log::Get("Stress")->info("stress-{}", "final");
    beat::log::Shutdown();

    int validLines = 0;
    int malformed = 0;
    int finalMarkers = 0;
    int lastSeq[kThreads];
    for (int& v : lastSeq) v = -1;
    bool monotonic = true;
    {
        std::istringstream reader(ReadFile(stressDir + "/runbeat.log"));
        const std::string marker = "] [Stress] stress-";
        std::string line;
        while (std::getline(reader, line)) {
            if (line.empty()) continue;
            const auto pos = line.find(marker);
            if (line.front() != '[' || pos == std::string::npos) {
                ++malformed;
                continue;
            }
            const std::string payload = line.substr(pos + marker.size());
            if (payload == "final") {
                ++finalMarkers;
                continue;
            }
            const auto dash = payload.find('-');
            int thread = -1;
            int seq = -1;
            try {
                thread = std::stoi(payload.substr(0, dash));
                seq = std::stoi(payload.substr(dash + 1));
            } catch (...) {
                ++malformed;
                continue;
            }
            if (dash == std::string::npos || thread < 0 || thread >= kThreads || seq < 0) {
                ++malformed;
                continue;
            }
            if (seq <= lastSeq[thread]) monotonic = false;
            lastSeq[thread] = seq;
            ++validLines;
        }
    }
    const bool stressOk = malformed == 0 && finalMarkers == 1
                          && validLines > kThreads * kLines / 2 && monotonic;
    printf("concurrent: toggles=%d kept=%d malformed=%d final=%d ordering=%s -> %s\n",
           toggleCount.load(), validLines, malformed, finalMarkers, monotonic ? "ok" : "broken",
           stressOk ? "yes" : "no");

    // 每拍经无锁队列落盘，溢出计数也进文件
    const std::string tickDir = NewDir();
    beat::log::Init({tickDir, spdlog::level::trace, std::size_t(4) << 20, 3, true});
    beat::log::Get("Tick")->info("file-probe");
    spdlog::logger* tick = beat::log::TickLogger();
    const bool tickLoggerReady = tick != nullptr;
    if (tick != nullptr) {
        tick->log(spdlog::level::trace, "beat={} frame={} sr={}", 42ULL, 7680ULL, 48000);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (tick != nullptr) {
        // 队列 256 槽：一口气灌 4000 条必然溢出，验证丢弃计数能落盘
        for (int i = 0; i < 4000; ++i) tick->log(spdlog::level::trace, "burst={}", i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    beat::log::Shutdown();
    const std::string tickLog = ReadFile(tickDir + "/runbeat.log");
    const bool beatLanded = Contains(tickLog, "[RunBeat-Beat]")
                            && Contains(tickLog, "beat=42 frame=7680 sr=48000");
    const bool dropReported = Contains(tickLog, "beat log dropped=");
    printf("tick log: ready=%s landed=%s dropped-reported=%s\n", tickLoggerReady ? "yes" : "no",
           beatLanded ? "yes" : "no", dropReported ? "yes" : "no");

    // 目录不可写时降级，不抛不崩
    beat::log::Init({"/dev/null/nope", spdlog::level::trace, std::size_t(1) << 20, 3, true});
    beat::log::Get("BadDir")->info("survived-bad-dir");
    beat::log::Shutdown();
    const bool degraded = !FileExists("/dev/null/nope/runbeat.log");
    printf("unwritable dir degraded: %s\n", degraded ? "yes" : "no");

    // 生命周期边界
    beat::log::Get("PreInit")->info("pre-init");  // Init 前取用
    beat::log::SetFileEnabled(true);              // 未 Init，不得建文件
    beat::log::Shutdown();                        // 未 Init，应为 no-op
    const std::string cycleDir = NewDir();
    beat::log::Init({cycleDir, spdlog::level::trace, std::size_t(1) << 20, 3, true});
    beat::log::Get("Cycle")->info("cycle-line");
    beat::log::Get("Cycle")->trace("cycle-trace");
    beat::log::Shutdown();
    beat::log::Shutdown();  // 重复 Shutdown
    beat::log::Init({cycleDir, spdlog::level::trace, std::size_t(1) << 20, 3, true});
    beat::log::Get("Cycle2")->info("cycle-reinit");
    beat::log::Get("PreInit")->info("late-bound");  // Init 前创建的 logger 也应挂上文件
    beat::log::Shutdown();
    const std::string cycleLog = ReadFile(cycleDir + "/runbeat.log");
    const bool cycleTrace = Contains(cycleLog, "cycle-trace");  // release 构建同样保留 trace
    const bool cycleOk = Contains(cycleLog, "cycle-line") && Contains(cycleLog, "cycle-reinit")
                         && Contains(cycleLog, "late-bound") && cycleTrace;
    printf("lifecycle edges: %s (trace-in-release=%s)\n", cycleOk ? "yes" : "no",
           cycleTrace ? "yes" : "no");

    // Shutdown 后开开关不得按旧目录重开；换目录后必须写新目录
    const std::string reopenDir = NewDir();
    beat::log::Init({reopenDir, spdlog::level::trace, std::size_t(1) << 20, 3, false});
    beat::log::Shutdown();
    beat::log::SetFileEnabled(true);
    const bool noReopen = !FileExists(reopenDir + "/runbeat.log");

    const std::string dirB = NewDir();
    beat::log::Init({dirB, spdlog::level::trace, std::size_t(1) << 20, 3, true});
    beat::log::Get("Cycle3")->info("dir-b");
    beat::log::Shutdown();
    const bool newDirUsed = Contains(ReadFile(dirB + "/runbeat.log"), "dir-b");
    printf("reopen after shutdown blocked=%s, new dir honored=%s\n", noReopen ? "yes" : "no",
           newDirUsed ? "yes" : "no");

    const bool pass = noFileCreated && firstKept && secondDropped && thirdKept && patternOk
                      && levelOk && reinitOk && rotated && capped && newestKept && oldestDropped
                      && flushed && noFileWhileOff && emptyFileOnEnable && wroteAfterEnable
                      && stressOk && tickLoggerReady && beatLanded && dropReported && degraded && cycleOk
                      && noReopen && newDirUsed;
    printf("\n=== %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
