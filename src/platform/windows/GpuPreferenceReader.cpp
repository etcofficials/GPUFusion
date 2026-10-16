#include "platform/windows/GpuPreferenceReader.h"

#include <QDir>

#include <windows.h>

#include <vector>

namespace gf::mon::win {

QString preferenceKey(const QString& exePath)
{
    return QDir::toNativeSeparators(exePath).toLower();
}

GpuPreferences readUserGpuPreferences()
{
    GpuPreferences result;
    HKEY key = nullptr;
    const LSTATUS open = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\DirectX\\UserGpuPreferences", 0, KEY_READ, &key);
    if (open == ERROR_FILE_NOT_FOUND)
        return result; // nothing configured
    if (open != ERROR_SUCCESS) {
        result.error = QStringLiteral("Could not read graphics preferences (error %1)").arg(open);
        return result;
    }
    DWORD maxName = 0, maxData = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxName, &maxData, nullptr, nullptr);
    std::vector<wchar_t> name(maxName + 2);
    std::vector<BYTE> data(maxData + 4);
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = DWORD(name.size()), dataLen = DWORD(data.size()), type = 0;
        const LSTATUS s = RegEnumValueW(key, i, name.data(), &nameLen, nullptr, &type, data.data(), &dataLen);
        if (s == ERROR_NO_MORE_ITEMS)
            break;
        if (s != ERROR_SUCCESS || type != REG_SZ)
            continue;
        const QString value = QString::fromWCharArray(reinterpret_cast<const wchar_t*>(data.data()), int(dataLen / sizeof(wchar_t))).remove(QChar(0));
        const QString path = QString::fromWCharArray(name.data(), int(nameLen));
        if (path.compare(QLatin1StringView("DirectXUserGlobalSettings"), Qt::CaseInsensitive) == 0)
            continue; // global switches, not an application
        result.byPath.insert(preferenceKey(path), parseGpuPreference(value));
    }
    RegCloseKey(key);
    return result;
}

} // namespace gf::mon::win
