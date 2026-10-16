#pragma once

// Test helper: tests that change the real per-user GPU preference may only touch this build's
// GPUFusionProbe.exe. Before changing it they snapshot its original value to a file; restore()
// puts it back and is also run by a separate CTest cleanup fixture, so even a crash cannot leave
// the value changed.

#include "preferences/PreferenceModel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace probeguard {

inline QString probePath()
{
    return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionProbe.exe")));
}

inline QString snapshotPath()
{
    return QDir(QDir::tempPath()).filePath(QStringLiteral("gpufusion-preferences-live-snapshot.json"));
}

// Restores from a snapshot if one exists. Returns false only if a restore was needed and failed.
inline bool restore(gf::pref::IPreferenceStore& store, QString* error)
{
    QFile f(snapshotPath());
    if (!f.open(QIODevice::ReadOnly))
        return true;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    const QString name = o.value(QStringLiteral("name")).toString();
    if (!name.endsWith(QStringLiteral("GPUFusionProbe.exe"), Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("snapshot names something other than the probe; refusing");
        return false;
    }
    const QJsonValue original = o.value(QStringLiteral("original"));
    const bool ok = original.isString() ? store.write(name, original.toString(), error) : store.remove(name, error);
    if (ok)
        QFile::remove(snapshotPath());
    return ok;
}

// Restores any previous snapshot, then records the current value. Returns the original value.
inline std::optional<QString> take(gf::pref::IPreferenceStore& store)
{
    restore(store, nullptr);
    const auto name = gf::pref::findValueName(store, probePath());
    const std::optional<QString> original = name ? store.read(*name) : std::nullopt;
    QFile f(snapshotPath());
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(QJsonObject{ { "name", name.value_or(probePath()) },
                                           { "original", original ? QJsonValue(*original) : QJsonValue(QJsonValue::Null) } }).toJson());
    return original;
}

} // namespace probeguard
