/** Log.hpp 验收测试：文件开关、级别过滤、Shutdown 后重用。 */

#include "../core/Log.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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
    const bool noFileCreated = ReadFile(logPath).empty();
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

    const bool pass = noFileCreated && firstKept && secondDropped && thirdKept && patternOk
                      && levelOk && reinitOk;
    printf("\n=== %s ===\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
