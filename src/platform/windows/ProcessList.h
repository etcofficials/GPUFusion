#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace gf::mon::win {

struct ProcessInfo {
    quint32 pid = 0;
    quint64 creationTime = 0; // FILETIME; 0 if unreadable. (pid, creationTime) identifies a process.
    QString name;             // executable name from the process snapshot
    QString path;             // full image path; empty if the process could not be opened
    QString pathError;
};

// Running processes (Toolhelp snapshot). Paths and creation times are read with
// PROCESS_QUERY_LIMITED_INFORMATION and cached per (pid, creation time), so each process is opened
// once, not every sample. Processes that exit simply stop appearing.
class ProcessList {
public:
    QList<ProcessInfo> snapshot();

private:
    struct Cached { quint64 creationTime; QString path, pathError; };
    QHash<quint32, Cached> m_cache;
};

} // namespace gf::mon::win
