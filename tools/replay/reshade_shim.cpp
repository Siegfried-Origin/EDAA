// The add-on sources call reshade::log::message. Built with RESHADE_API_LIBRARY
// they import ReShadeLogMessage instead of looking ReShade up at runtime: the
// replay defines it here (this file must not include reshade.hpp, whose
// declaration is dllimport) and prints to the console.

#include <cstdio>
#include <string>
#include <Windows.h>

extern "C" void ReShadeLogMessage(void*, int level, const char* message)
{
    static const char* const names[] = { "", "error", "warning", "info", "debug" };
    std::printf("  [%s] %s\n", level >= 1 && level <= 4 ? names[level] : "log", message);
}

namespace edaa
{
    // dlss.cpp passes it to NGX as the engine version.
    extern const char* const version = "replay";

    // dlss.cpp loads nvngx_dlss.dll from here: the replay's own folder.
    std::wstring addon_folder()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring folder = path;
        return folder.substr(0, folder.find_last_of(L"\\/"));
    }
}
