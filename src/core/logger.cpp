#include "core/logger.h"
#include <Windows.h>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>

static FILE* g_logFile = nullptr;
static std::string g_logPath;
static std::string g_gameDir;
static uint64_t g_startTick = 0;
// Serializes every fprintf/fflush against g_logFile. Multiple threads (game thread,
// hotkey poll thread, deferred-init thread) call Log::Write — concurrent fprintf into
// a shared FILE* corrupts the libc stdio buffer struct itself, not just the output
// text, and can crash *outside* the logger when the buffer is later flushed.
static std::mutex g_logMutex;

static const char* LOG_PREFIX = "FFXII-Screen-Reader";

static bool ParseSessionTimestamp(const char* firstLine, char* outBuf, size_t outBufSize) {
    const char* prefix = "Session started: ";
    size_t prefixLen = strlen(prefix);
    if (strncmp(firstLine, prefix, prefixLen) != 0) return false;

    const char* ts = firstLine + prefixLen;
    if (strlen(ts) < 19) return false;

    if (ts[4] != '-' || ts[7] != '-' || ts[10] != ' ' || ts[13] != ':' || ts[16] != ':')
        return false;

    snprintf(outBuf, outBufSize, "%.4s-%.2s-%.2s_%.2s-%.2s-%.2s",
             ts, ts + 5, ts + 8, ts + 11, ts + 14, ts + 17);
    return true;
}

static bool GetFileModTimeStamp(const char* path, char* outBuf, size_t outBufSize) {
    struct _stat st;
    if (_stat(path, &st) != 0) return false;

    struct tm tm;
    localtime_s(&tm, &st.st_mtime);
    snprintf(outBuf, outBufSize, "%04d-%02d-%02d_%02d-%02d-%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return true;
}

static void CleanupOldLogs(const std::string& logsDir, int maxKeep = 20) {
    std::string searchPattern = logsDir + "\\" + LOG_PREFIX + "-*.log";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPattern.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::string> files;
    do {
        files.push_back(findData.cFileName);
    } while (FindNextFileA(hFind, &findData));
    FindClose(hFind);

    if (static_cast<int>(files.size()) <= maxKeep) return;

    std::sort(files.begin(), files.end());
    int toDelete = static_cast<int>(files.size()) - maxKeep;
    for (int i = 0; i < toDelete; i++) {
        std::string fullPath = logsDir + "\\" + files[i];
        DeleteFileA(fullPath.c_str());
    }
}

static void RotateExistingLog(const std::string& latestPath, const std::string& baseDir) {
    FILE* old = fopen(latestPath.c_str(), "r");
    if (!old) return;

    char firstLine[256] = {};
    if (!fgets(firstLine, sizeof(firstLine), old)) {
        fclose(old);
        remove(latestPath.c_str());
        return;
    }
    fclose(old);

    size_t len = strlen(firstLine);
    while (len > 0 && (firstLine[len - 1] == '\n' || firstLine[len - 1] == '\r'))
        firstLine[--len] = '\0';

    char tsBuf[64] = {};
    bool gotTs = ParseSessionTimestamp(firstLine, tsBuf, sizeof(tsBuf));

    if (!gotTs) {
        gotTs = GetFileModTimeStamp(latestPath.c_str(), tsBuf, sizeof(tsBuf));
    }

    if (!gotTs) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_s(&tm, &now);
        snprintf(tsBuf, sizeof(tsBuf), "%04d-%02d-%02d_%02d-%02d-%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    std::string logsDir = baseDir + "\\logs";
    CreateDirectoryA(logsDir.c_str(), nullptr);

    std::string target = logsDir + "\\" + LOG_PREFIX + "-" + tsBuf + ".log";

    if (GetFileAttributesA(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (int i = 1; i < 100; i++) {
            char suffix[16];
            snprintf(suffix, sizeof(suffix), "-%d", i);
            std::string alt = logsDir + "\\" + LOG_PREFIX + "-" + tsBuf + suffix + ".log";
            if (GetFileAttributesA(alt.c_str()) == INVALID_FILE_ATTRIBUTES) {
                target = alt;
                break;
            }
        }
    }

    rename(latestPath.c_str(), target.c_str());

    CleanupOldLogs(logsDir);
}

namespace Log {

void Init(const std::string& gameDir) {
    g_gameDir = gameDir;
    g_startTick = GetTickCount64();
    g_logPath = gameDir + "\\" + LOG_PREFIX + "-Latest.log";

    RotateExistingLog(g_logPath, gameDir);

    g_logFile = fopen(g_logPath.c_str(), "w");

    if (g_logFile) {
        time_t now = time(nullptr);
        struct tm tm;
        localtime_s(&tm, &now);
        fprintf(g_logFile, "Session started: %04d-%02d-%02d %02d:%02d:%02d\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
        fflush(g_logFile);

        Write("INIT", "=== FFXII Screen Reader Mod Starting ===");

        char msg[512];
        snprintf(msg, sizeof(msg), "Game directory: %s", gameDir.c_str());
        Write("INIT", msg);
    }
}

void Write(const char* category, const char* message) {
    if (!g_logFile) return;

    uint64_t now = GetTickCount64();
    uint64_t elapsed = now - g_startTick;
    uint64_t dayMs = now % 86400000ULL;
    int h = (int)(dayMs / 3600000);
    int m = (int)((dayMs % 3600000) / 60000);
    int s = (int)((dayMs % 60000) / 1000);
    int ms = (int)(dayMs % 1000);

    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile) return;

    fprintf(g_logFile, "[%02d:%02d:%02d.%03d +%llums] [%s] %s\n",
            h, m, s, ms, (unsigned long long)elapsed, category, message);

    // Flush categories whose loss would hide a bug. PARTY and COMBAT were added after a diagnostic
    // line sat in an unflushed stdio buffer and vanished on a hard exit -- which is what made the
    // silent 4/5/6 keys look like an input-path failure for two sessions.
    if (category && (strcmp(category, "ERROR") == 0 ||
                     strcmp(category, "INIT") == 0 ||
                     strcmp(category, "HOOK_HEALTH") == 0 ||
                     strcmp(category, "PARTY") == 0 ||
                     strcmp(category, "COMBAT") == 0)) {
        fflush(g_logFile);
    }
}

uint64_t GetStartTick() {
    return g_startTick;
}

const std::string& GetGameDir() {
    return g_gameDir;
}

void Shutdown() {
    if (g_logFile) {
        Write("INIT", "=== FFXII Screen Reader Mod Shutting Down ===");
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

} // namespace Log
