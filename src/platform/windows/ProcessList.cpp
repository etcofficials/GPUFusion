#include "platform/windows/ProcessList.h"

#include <QSet>

#include <windows.h>
#include <tlhelp32.h>

namespace gf::mon::win {

QList<ProcessInfo> ProcessList::snapshot()
{
    QList<ProcessInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;
    QSet<quint32> seen;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == 0) // System Idle Process
            continue;
        ProcessInfo p;
        p.pid = pe.th32ProcessID;
        p.name = QString::fromWCharArray(pe.szExeFile);
        seen.insert(p.pid);

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
        const DWORD openError = h ? 0 : GetLastError();
        if (h) {
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(h, &created, &exited, &kernel, &user))
                p.creationTime = (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        }
        const auto it = m_cache.constFind(p.pid);
        if (it != m_cache.constEnd() && it->creationTime == p.creationTime) {
            p.path = it->path;
            p.pathError = it->pathError;
        } else if (h) {
            wchar_t buffer[MAX_PATH * 4];
            DWORD size = DWORD(std::size(buffer));
            if (QueryFullProcessImageNameW(h, 0, buffer, &size))
                p.path = QString::fromWCharArray(buffer, int(size));
            else
                p.pathError = QStringLiteral("Path not readable (error %1)").arg(GetLastError());
            m_cache.insert(p.pid, { p.creationTime, p.path, p.pathError });
        } else {
            p.pathError = openError == ERROR_ACCESS_DENIED
                ? QStringLiteral("Protected or another user's process: Windows does not let a standard user open it")
                : QStringLiteral("Process could not be opened (error %1)").arg(openError);
            m_cache.insert(p.pid, { p.creationTime, p.path, p.pathError });
        }
        if (h)
            CloseHandle(h);
        out << p;
    }
    CloseHandle(snap);
    // Forget processes that have exited.
    for (auto it = m_cache.begin(); it != m_cache.end();)
        it = seen.contains(it.key()) ? std::next(it) : m_cache.erase(it);
    return out;
}

} // namespace gf::mon::win
