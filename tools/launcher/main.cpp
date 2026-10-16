// GPU Fusion launcher: a tiny, dependency-free GPUFusion.exe placed in the release folder next to
// the portable app folder ("GPUFusion\"). Double-clicking it starts GPUFusion\GPUFusion.exe with the
// same command-line arguments and exits. It has the application's icon and version information.
#include <windows.h>

#include <string>

namespace {

std::wstring directoryOf(const std::wstring& path)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

// The arguments after the program name, exactly as typed (quoting preserved).
const wchar_t* argumentsTail(const wchar_t* commandLine)
{
    const wchar_t* p = commandLine;
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"')
            ++p;
        if (*p == L'"')
            ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t')
            ++p;
    }
    while (*p == L' ' || *p == L'\t')
        ++p;
    return p;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    std::wstring self(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, self.data(), DWORD(self.size()));
        if (n == 0)
            return 1;
        if (n < self.size()) {
            self.resize(n);
            break;
        }
        self.resize(self.size() * 2);
    }
    const std::wstring dir = directoryOf(self);
    const std::wstring app = dir + L"\\GPUFusion\\GPUFusion.exe";
    if (GetFileAttributesW(app.c_str()) == INVALID_FILE_ATTRIBUTES) {
        const std::wstring message = L"GPU Fusion could not be found at\n\n" + app +
                                     L"\n\nKeep this launcher next to the \"GPUFusion\" folder it came with.";
        MessageBoxW(nullptr, message.c_str(), L"GPU Fusion", MB_ICONERROR | MB_OK);
        return 2;
    }
    std::wstring commandLine = L"\"" + app + L"\"";
    if (const wchar_t* tail = argumentsTail(GetCommandLineW()); *tail)
        commandLine += std::wstring(L" ") + tail;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring workDir = dir + L"\\GPUFusion";
    if (!CreateProcessW(app.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, workDir.c_str(), &si, &pi)) {
        const std::wstring message = L"GPU Fusion could not be started (error " + std::to_wstring(GetLastError()) + L").";
        MessageBoxW(nullptr, message.c_str(), L"GPU Fusion", MB_ICONERROR | MB_OK);
        return 3;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
