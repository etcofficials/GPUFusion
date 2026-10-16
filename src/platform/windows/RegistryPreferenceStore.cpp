#include "platform/windows/RegistryPreferenceStore.h"

#include <windows.h>

#include <vector>

namespace gf::pref::win {

namespace {

constexpr const wchar_t* kKey = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";
constexpr const wchar_t* kGlobalSettings = L"DirectXUserGlobalSettings"; // not an application; never touched

const wchar_t* wide(const QString& s) { return reinterpret_cast<const wchar_t*>(s.utf16()); }

QString errorText(LSTATUS s)
{
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, DWORD(s), 0,
                   reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    const QString text = buffer ? QString::fromWCharArray(buffer).trimmed() : QString();
    LocalFree(buffer);
    return QStringLiteral("%1 (error %2)").arg(text).arg(s);
}

bool isApplicationValue(const QString& name)
{
    return name.compare(QString::fromWCharArray(kGlobalSettings), Qt::CaseInsensitive) != 0;
}

} // namespace

QStringList RegistryPreferenceStore::names() const
{
    QStringList out;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return out;
    DWORD maxName = 0;
    RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxName, nullptr, nullptr, nullptr);
    std::vector<wchar_t> buffer(maxName + 2);
    for (DWORD i = 0;; ++i) {
        DWORD len = DWORD(buffer.size()), type = 0;
        const LSTATUS s = RegEnumValueW(key, i, buffer.data(), &len, nullptr, &type, nullptr, nullptr);
        if (s == ERROR_NO_MORE_ITEMS)
            break;
        if (s == ERROR_SUCCESS && type == REG_SZ) {
            const QString name = QString::fromWCharArray(buffer.data(), int(len));
            if (isApplicationValue(name))
                out << name;
        }
    }
    RegCloseKey(key);
    return out;
}

std::optional<QString> RegistryPreferenceStore::read(const QString& name) const
{
    DWORD type = 0, bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, wide(name), RRF_RT_REG_SZ, &type, nullptr, &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 1);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, wide(name), RRF_RT_REG_SZ, &type, data.data(), &bytes) != ERROR_SUCCESS)
        return std::nullopt;
    return QString::fromWCharArray(data.data());
}

bool RegistryPreferenceStore::write(const QString& name, const QString& data, QString* error)
{
    if (!isApplicationValue(name)) {
        if (error) *error = QStringLiteral("Refusing to change Windows' global graphics settings.");
        return false;
    }
    HKEY key = nullptr;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (s == ERROR_SUCCESS) {
        s = RegSetValueExW(key, wide(name), 0, REG_SZ, reinterpret_cast<const BYTE*>(data.utf16()),
                           DWORD((data.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    if (s != ERROR_SUCCESS && error)
        *error = errorText(s);
    return s == ERROR_SUCCESS;
}

bool RegistryPreferenceStore::remove(const QString& name, QString* error)
{
    if (!isApplicationValue(name)) {
        if (error) *error = QStringLiteral("Refusing to change Windows' global graphics settings.");
        return false;
    }
    HKEY key = nullptr;
    LSTATUS s = RegOpenKeyExW(HKEY_CURRENT_USER, kKey, 0, KEY_SET_VALUE, &key);
    if (s == ERROR_FILE_NOT_FOUND)
        return true; // nothing to remove
    if (s == ERROR_SUCCESS) {
        s = RegDeleteValueW(key, wide(name));
        RegCloseKey(key);
        if (s == ERROR_FILE_NOT_FOUND)
            s = ERROR_SUCCESS;
    }
    if (s != ERROR_SUCCESS && error)
        *error = errorText(s);
    return s == ERROR_SUCCESS;
}

} // namespace gf::pref::win
