#pragma once
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif
struct ProcessStats
{
    uint64_t cpu_us = 0, rss_bytes = 0;
};
inline ProcessStats process_stats()
{
    ProcessStats s;
#ifdef _WIN32
    FILETIME created{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user))
    {
        auto value = [](FILETIME t)
        { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        s.cpu_us = (value(kernel) + value(user)) / 10;
    }
    PROCESS_MEMORY_COUNTERS memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        s.rss_bytes = memory.WorkingSetSize;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0)
    {
        s.cpu_us = uint64_t(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 + usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
#ifdef __APPLE__
        s.rss_bytes = usage.ru_maxrss;
#else
        s.rss_bytes = uint64_t(usage.ru_maxrss) * 1024;
#endif
    }
#endif
    return s;
}
