#include "profiles/AppDiscovery.h"

#include "platform/windows/ProcessList.h"
#include "profiles/AppProfile.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSettings>

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace gf::profiles {

bool looksLikeHelperExecutable(const QString& exePath)
{
    static const QRegularExpression helper(
        QStringLiteral(R"(^(unins\d*|uninst(all)?\w*|setup\w*|install\w*|update|updater|crashpad_handler|crashreporter|helper|vc_redist.*|dxsetup)$)"),
        QRegularExpression::CaseInsensitiveOption);
    return helper.match(QFileInfo(exePath).completeBaseName()).hasMatch();
}

QString executableFromRegistryValue(const QString& value)
{
    QString v = value.trimmed();
    if (v.startsWith(u'"')) {
        const int end = v.indexOf(u'"', 1);
        v = end > 0 ? v.mid(1, end - 1) : v.mid(1);
    } else {
        const int exe = v.indexOf(QLatin1StringView(".exe"), 0, Qt::CaseInsensitive);
        if (exe >= 0)
            v = v.left(exe + 4);
    }
    return v.endsWith(QLatin1StringView(".exe"), Qt::CaseInsensitive) ? QDir::toNativeSeparators(v) : QString();
}

namespace {

QString knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR path = nullptr;
    QString out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
        out = QString::fromWCharArray(path);
    CoTaskMemFree(path);
    return out;
}

QString shortcutTarget(const QString& lnk)
{
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))))
        return {};
    QString target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
        // Load only: no Resolve() (which could search the disk or show UI).
        if (SUCCEEDED(file->Load(reinterpret_cast<LPCOLESTR>(QDir::toNativeSeparators(lnk).utf16()), STGM_READ))) {
            wchar_t buffer[MAX_PATH * 2] = {};
            if (SUCCEEDED(link->GetPath(buffer, int(std::size(buffer)), nullptr, 0)))
                target = QString::fromWCharArray(buffer);
        }
        file->Release();
    }
    link->Release();
    return target;
}

} // namespace

QList<DiscoveredApp> discoverApps()
{
    QList<DiscoveredApp> out;
    QHash<QString, int> seen; // normalized path -> index
    auto add = [&](const QString& name, const QString& exe, const QString& source) {
        if (exe.isEmpty() || !exe.endsWith(QLatin1StringView(".exe"), Qt::CaseInsensitive) || looksLikeHelperExecutable(exe))
            return;
        const QFileInfo fi(exe);
        if (!fi.exists() || !fi.isFile())
            return;
        const QString key = normalizedPath(exe);
        if (seen.contains(key))
            return;
        seen.insert(key, int(out.size()));
        out << DiscoveredApp{ name.isEmpty() ? fi.completeBaseName() : name, QDir::toNativeSeparators(fi.absoluteFilePath()), source };
    };

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (const KNOWNFOLDERID* id : { &FOLDERID_CommonPrograms, &FOLDERID_Programs }) {
        const QString root = knownFolder(*id);
        if (root.isEmpty())
            continue;
        QDirIterator it(root, { QStringLiteral("*.lnk") }, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString lnk = it.next();
            add(QFileInfo(lnk).completeBaseName(), shortcutTarget(lnk), QStringLiteral("Start menu"));
        }
    }
    if (com == S_OK || com == S_FALSE)
        CoUninitialize();

    for (const char* root : { "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                              "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                              "HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" }) {
        QSettings reg(QString::fromLatin1(root), QSettings::NativeFormat);
        for (const QString& key : reg.childGroups()) {
            reg.beginGroup(key);
            const QString name = reg.value(QStringLiteral("DisplayName")).toString();
            const QString icon = executableFromRegistryValue(reg.value(QStringLiteral("DisplayIcon")).toString());
            if (!name.isEmpty())
                add(name, icon, QStringLiteral("Installed programs"));
            reg.endGroup();
        }
    }

    mon::win::ProcessList list;
    for (const mon::win::ProcessInfo& p : list.snapshot()) {
        if (!p.path.isEmpty() && !p.path.startsWith(QDir::toNativeSeparators(qEnvironmentVariable("SystemRoot")), Qt::CaseInsensitive))
            add(QFileInfo(p.path).completeBaseName(), p.path, QStringLiteral("Running"));
    }
    return out;
}

} // namespace gf::profiles
