#include "platform/windows/OpenClInfo.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>

#include <vector>

namespace gf::diag::win {

OpenClInfo readOpenClInfo()
{
    OpenClInfo info;
    wchar_t system[MAX_PATH] = {};
    if (GetSystemDirectoryW(system, MAX_PATH))
        info.loaderPresent = QFileInfo::exists(QDir(QString::fromWCharArray(system)).filePath(QStringLiteral("OpenCL.dll")));

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenCL\\Vendors", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return info;
    std::vector<wchar_t> name(1024);
    for (DWORD i = 0;; ++i) {
        DWORD len = DWORD(name.size());
        const LSTATUS s = RegEnumValueW(key, i, name.data(), &len, nullptr, nullptr, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS)
            break;
        if (s == ERROR_SUCCESS)
            info.icds << QFileInfo(QString::fromWCharArray(name.data(), int(len))).fileName(); // file name only
    }
    RegCloseKey(key);
    return info;
}

} // namespace gf::diag::win
