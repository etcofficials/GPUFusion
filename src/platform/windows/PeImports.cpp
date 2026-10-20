#include "platform/windows/PeImports.h"

#include <QFile>

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace gf::pref::win {

namespace {

template <typename T>
bool readAt(const QByteArray& data, quint64 offset, T* out)
{
    if (offset > quint64(data.size()) || quint64(data.size()) - offset < sizeof(T))
        return false;
    std::memcpy(out, data.constData() + offset, sizeof(T));
    return true;
}

} // namespace

std::optional<QStringList> readImports(const QString& exePath, QString* error)
{
    auto fail = [&](const QString& why) -> std::optional<QStringList> {
        if (error)
            *error = why;
        return std::nullopt;
    };
    QFile f(exePath);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot read %1: %2").arg(exePath, f.errorString()));
    if (f.size() > 512ll * 1024 * 1024)
        return fail(QStringLiteral("The file is too large to inspect."));
    const QByteArray data = f.readAll();

    IMAGE_DOS_HEADER dos{};
    if (!readAt(data, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return fail(QStringLiteral("Not a Windows executable (no MZ header)."));
    DWORD signature = 0;
    IMAGE_FILE_HEADER file{};
    const quint64 nt = quint64(dos.e_lfanew);
    if (!readAt(data, nt, &signature) || signature != IMAGE_NT_SIGNATURE || !readAt(data, nt + 4, &file))
        return fail(QStringLiteral("Not a Windows executable (no PE header)."));
    const quint64 optional = nt + 4 + sizeof(IMAGE_FILE_HEADER);
    WORD magic = 0;
    if (!readAt(data, optional, &magic))
        return fail(QStringLiteral("Truncated PE header."));
    IMAGE_DATA_DIRECTORY importDir{};
    if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_OPTIONAL_HEADER64 h{};
        if (!readAt(data, optional, &h) || h.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
            return fail(QStringLiteral("Truncated PE header."));
        importDir = h.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        IMAGE_OPTIONAL_HEADER32 h{};
        if (!readAt(data, optional, &h) || h.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
            return fail(QStringLiteral("Truncated PE header."));
        importDir = h.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    } else {
        return fail(QStringLiteral("Unknown PE format."));
    }
    QStringList out;
    if (importDir.VirtualAddress == 0)
        return out; // no imports

    // Map RVAs to file offsets through the section table.
    const quint64 sections = optional + file.SizeOfOptionalHeader;
    auto toOffset = [&](DWORD rva) -> std::optional<quint64> {
        for (WORD i = 0; i < file.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER s{};
            if (!readAt(data, sections + quint64(i) * sizeof(IMAGE_SECTION_HEADER), &s))
                return std::nullopt;
            const DWORD size = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + size)
                return quint64(rva - s.VirtualAddress) + s.PointerToRawData;
        }
        return std::nullopt;
    };
    auto start = toOffset(importDir.VirtualAddress);
    if (!start)
        return fail(QStringLiteral("The import table could not be located."));
    for (int i = 0; i < 4096; ++i) { // bounded: a damaged file cannot loop forever
        IMAGE_IMPORT_DESCRIPTOR d{};
        if (!readAt(data, *start + quint64(i) * sizeof(d), &d))
            return fail(QStringLiteral("Truncated import table."));
        if (d.Name == 0 && d.FirstThunk == 0)
            break;
        const auto name = toOffset(d.Name);
        if (!name)
            continue;
        QByteArray dll;
        for (quint64 p = *name; p < quint64(data.size()) && data[qsizetype(p)] != '\0' && dll.size() < 260; ++p)
            dll += data[qsizetype(p)];
        if (!dll.isEmpty())
            out << QString::fromLatin1(dll).toLower();
    }
    out.removeDuplicates();
    return out;
}

} // namespace gf::pref::win
