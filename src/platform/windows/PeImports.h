#pragma once

#include <QString>
#include <QStringList>

#include <optional>

namespace gf::pref::win {

// DLL names an executable imports directly (PE import directory, 32- and 64-bit), lower-case,
// e.g. "d3d11.dll". Only static imports: libraries a program loads at run time (LoadLibrary), or
// that a launcher's child processes use, are not visible here. Reads the file only.
std::optional<QStringList> readImports(const QString& exePath, QString* error);

} // namespace gf::pref::win
