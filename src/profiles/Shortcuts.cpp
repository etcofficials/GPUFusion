#include "profiles/Shortcuts.h"

#include "profiles/ProfileStore.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace gf::profiles {

namespace {

struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComInit()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};

QString hresultText(HRESULT hr) { return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0')); }

} // namespace

QString shortcutName(const AppProfile& profile)
{
    QString name = QStringLiteral("%1 (%2)").arg(displayName(profile), modeText(profile.mode));
    static const QRegularExpression invalid(QStringLiteral(R"([<>:"/\\|?*\x00-\x1F])"));
    name.replace(invalid, QStringLiteral("_"));
    return name.trimmed();
}

QStringList shortcutArguments(const AppProfile& profile, const QString& dataDir)
{
    QStringList args{ QStringLiteral("--launch-profile"), profile.id, QStringLiteral("--interactive") };
    if (!dataDir.isEmpty())
        args << QStringLiteral("--data-dir") << QDir::toNativeSeparators(dataDir);
    return args;
}

QString joinArguments(const QStringList& arguments)
{
    QStringList out;
    for (const QString& a : arguments)
        out << (a.isEmpty() || a.contains(u' ') || a.contains(u'\t') ? QStringLiteral("\"%1\"").arg(a) : a);
    return out.join(u' ');
}

ShortcutSpec profileShortcut(const AppProfile& profile, const QString& gpuFusionExe, const QString& folder, const QString& dataDir)
{
    ShortcutSpec s;
    s.path = QDir::toNativeSeparators(QDir(folder).filePath(shortcutName(profile) + QStringLiteral(".lnk")));
    s.target = QDir::toNativeSeparators(gpuFusionExe);
    s.arguments = shortcutArguments(profile, dataDir);
    s.workingDirectory = QDir::toNativeSeparators(QFileInfo(resolveCurrentExecutable(profile.exePath)).absolutePath());
    s.iconPath = QDir::toNativeSeparators(profile.exePath);
    s.description = QStringLiteral("Starts %1 with GPU mode \"%2\" through GPU Fusion").arg(displayName(profile), modeText(profile.mode));
    return s;
}

bool writeShortcut(const ShortcutSpec& spec, QString* error)
{
    ComInit com;
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("Cannot create a shortcut object (%1).").arg(hresultText(hr));
        return false;
    }
    link->SetPath(reinterpret_cast<LPCWSTR>(spec.target.utf16()));
    link->SetArguments(reinterpret_cast<LPCWSTR>(joinArguments(spec.arguments).utf16()));
    link->SetWorkingDirectory(reinterpret_cast<LPCWSTR>(spec.workingDirectory.utf16()));
    link->SetIconLocation(reinterpret_cast<LPCWSTR>(spec.iconPath.utf16()), spec.iconIndex);
    link->SetDescription(reinterpret_cast<LPCWSTR>(spec.description.left(259).utf16()));
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) {
        hr = file->Save(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(spec.path).utf16()), TRUE);
        file->Release();
    }
    link->Release();
    if (FAILED(hr)) {
        if (error) *error = QStringLiteral("Cannot save %1 (%2).").arg(spec.path, hresultText(hr));
        return false;
    }
    return true;
}

std::optional<ShortcutSpec> readShortcut(const QString& path, QString* error)
{
    ComInit com;
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) {
        if (error) *error = hresultText(hr);
        return std::nullopt;
    }
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) {
        hr = file->Load(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), STGM_READ);
        file->Release();
    }
    std::optional<ShortcutSpec> out;
    if (SUCCEEDED(hr)) {
        wchar_t buf[INFOTIPSIZE] = {};
        ShortcutSpec s;
        s.path = path;
        if (SUCCEEDED(link->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH)))
            s.target = QString::fromWCharArray(buf);
        if (SUCCEEDED(link->GetArguments(buf, INFOTIPSIZE)))
            s.arguments = QProcess::splitCommand(QString::fromWCharArray(buf));
        if (SUCCEEDED(link->GetWorkingDirectory(buf, MAX_PATH)))
            s.workingDirectory = QString::fromWCharArray(buf);
        int icon = 0;
        if (SUCCEEDED(link->GetIconLocation(buf, MAX_PATH, &icon))) {
            s.iconPath = QString::fromWCharArray(buf);
            s.iconIndex = icon;
        }
        if (SUCCEEDED(link->GetDescription(buf, INFOTIPSIZE)))
            s.description = QString::fromWCharArray(buf);
        out = s;
    } else if (error) {
        *error = QStringLiteral("Cannot read %1 (%2).").arg(path, hresultText(hr));
    }
    link->Release();
    return out;
}

QString desktopFolder()
{
    PWSTR p = nullptr;
    QString out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p)))
        out = QString::fromWCharArray(p);
    CoTaskMemFree(p);
    return out;
}

} // namespace gf::profiles
