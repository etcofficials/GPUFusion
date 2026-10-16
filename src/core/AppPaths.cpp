#include "core/AppPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTextStream>

namespace gf {

QString DataLocation::logsDir() const { return QDir(root).filePath(QStringLiteral("logs")); }
QString DataLocation::benchmarksDir() const { return QDir(root).filePath(QStringLiteral("benchmarks")); }
QString DataLocation::reportsDir() const { return QDir(root).filePath(QStringLiteral("reports")); }
QString DataLocation::configDir() const { return QDir(root).filePath(QStringLiteral("config")); }
QString DataLocation::tempDir() const { return QDir(root).filePath(QStringLiteral("temp")); }
QString DataLocation::settingsFile() const { return QDir(configDir()).filePath(QStringLiteral("settings.json")); }

QString describe(DataLocation::Source source)
{
    switch (source) {
    case DataLocation::Source::CommandLine: return QStringLiteral("command line (--data-dir)");
    case DataLocation::Source::PointerFile: return QStringLiteral("%1 next to the executable").arg(QLatin1StringView(kDataDirPointerFile));
    case DataLocation::Source::Portable:    return QStringLiteral("portable folder next to the executable");
    case DataLocation::Source::Fallback:    return QStringLiteral("per-user fallback folder");
    }
    return QStringLiteral("unknown");
}

namespace {

QString readPointerFile(const QString& exeDir)
{
    QFile file(QDir(exeDir).filePath(QLatin1StringView(kDataDirPointerFile)));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty() && !line.startsWith(u'#'))
            return line;
    }
    return {};
}

QString absoluteFrom(const QString& base, const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).isAbsolute() ? path : QDir(base).filePath(path));
}

// Returns an error message if root cannot be created or written.
QString checkWritable(const QString& root)
{
    if (!QDir().mkpath(root))
        return QStringLiteral("cannot create folder %1").arg(QDir::toNativeSeparators(root));
    QTemporaryFile probe(QDir(root).filePath(QStringLiteral(".write-test-XXXXXX")));
    if (!probe.open())
        return QStringLiteral("folder %1 is not writable (%2)").arg(QDir::toNativeSeparators(root), probe.errorString());
    return {};
}

} // namespace

DataLocation resolveDataLocation(const QString& exeDir, const QString& commandLineOverride,
                                 const QString& fallbackRoot)
{
    DataLocation loc;
    if (!commandLineOverride.isEmpty()) {
        loc.root = absoluteFrom(exeDir, commandLineOverride);
        loc.source = DataLocation::Source::CommandLine;
    } else if (const QString pointer = readPointerFile(exeDir); !pointer.isEmpty()) {
        loc.root = absoluteFrom(exeDir, pointer);
        loc.source = DataLocation::Source::PointerFile;
    } else {
        loc.root = QDir(exeDir).filePath(QStringLiteral("data"));
        loc.source = DataLocation::Source::Portable;
    }

    if (const QString error = checkWritable(loc.root); !error.isEmpty()) {
        loc.problem = QStringLiteral("Preferred data folder rejected: %1").arg(error);
        loc.root = QDir::cleanPath(fallbackRoot);
        loc.source = DataLocation::Source::Fallback;
    }
    return loc;
}

QString ensureDataLayout(const DataLocation& location)
{
    for (const QString& dir : { location.root, location.logsDir(), location.benchmarksDir(),
                                location.reportsDir(), location.configDir(), location.tempDir() }) {
        if (!QDir().mkpath(dir))
            return QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(dir));
    }
    return {};
}

} // namespace gf
