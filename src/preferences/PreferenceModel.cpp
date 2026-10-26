#include "preferences/PreferenceModel.h"

#include "core/Logging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>

namespace gf::pref {

// ---- value model ---------------------------------------------------------------------------------

GpuPreference parseGpuPreference(const QString& data)
{
    static const QRegularExpression re(QStringLiteral(R"((?:^|;)\s*GpuPreference\s*=\s*(\d+)\s*(?:;|$))"),
                                       QRegularExpression::CaseInsensitiveOption);
    // Other keys only (e.g. Windows' own "SwapEffectUpgradeEnable=1;") means no GPU preference is set;
    // a GpuPreference key that cannot be read is unrecognised.
    static const QRegularExpression key(QStringLiteral(R"((?:^|;)\s*GpuPreference\s*=)"), QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(data);
    if (!m.hasMatch())
        return key.match(data).hasMatch() ? GpuPreference::Unrecognised : GpuPreference::NotSet;
    switch (m.captured(1).toInt()) {
    case 0:  return GpuPreference::LetWindowsDecide;
    case 1:  return GpuPreference::PowerSaving;
    case 2:  return GpuPreference::HighPerformance;
    default: return GpuPreference::Unrecognised;
    }
}

QString gpuPreferenceText(GpuPreference p)
{
    switch (p) {
    case GpuPreference::NotSet:           return QStringLiteral("Not set");
    case GpuPreference::LetWindowsDecide: return QStringLiteral("Let Windows decide");
    case GpuPreference::PowerSaving:      return QStringLiteral("Power saving");
    case GpuPreference::HighPerformance:  return QStringLiteral("High performance");
    case GpuPreference::Unrecognised:     return QStringLiteral("Unrecognised value");
    }
    return {};
}

int gpuPreferenceCode(GpuPreference p)
{
    switch (p) {
    case GpuPreference::LetWindowsDecide: return 0;
    case GpuPreference::PowerSaving:      return 1;
    case GpuPreference::HighPerformance:  return 2;
    default:                              return -1;
    }
}

QString withGpuPreference(const QString& existing, GpuPreference p)
{
    const int code = gpuPreferenceCode(p);
    QStringList pairs;
    bool replaced = false;
    for (const QString& raw : existing.split(u';', Qt::SkipEmptyParts)) {
        const QString item = raw.trimmed();
        if (item.isEmpty())
            continue;
        if (item.section(u'=', 0, 0).trimmed().compare(QLatin1StringView("GpuPreference"), Qt::CaseInsensitive) == 0) {
            if (code >= 0 && !replaced)
                pairs << QStringLiteral("GpuPreference=%1").arg(code);
            replaced = true;
            continue;
        }
        pairs << item;
    }
    if (!replaced && code >= 0)
        pairs << QStringLiteral("GpuPreference=%1").arg(code);
    return pairs.isEmpty() ? QString() : pairs.join(u';') + u';';
}

bool isConfigurableExecutable(const QString& path, QString* why)
{
    const QFileInfo fi(path);
    auto fail = [&](const QString& reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (path.trimmed().isEmpty())
        return fail(QStringLiteral("No executable chosen."));
    if (!fi.isAbsolute())
        return fail(QStringLiteral("The path must be absolute."));
    if (fi.suffix().compare(QLatin1StringView("exe"), Qt::CaseInsensitive) != 0)
        return fail(QStringLiteral("Graphics preferences apply to .exe files."));
    if (!fi.isFile())
        return fail(QStringLiteral("The file does not exist."));
    return true;
}

std::optional<QString> findValueName(const IPreferenceStore& store, const QString& exePath)
{
    const QString wanted = QDir::toNativeSeparators(QDir::cleanPath(exePath));
    for (const QString& name : store.names()) {
        if (QDir::toNativeSeparators(QDir::cleanPath(name)).compare(wanted, Qt::CaseInsensitive) == 0)
            return name;
    }
    return std::nullopt;
}

// ---- journal --------------------------------------------------------------------------------------

namespace {

QJsonValue optionalString(const std::optional<QString>& s) { return s ? QJsonValue(*s) : QJsonValue(QJsonValue::Null); }
std::optional<QString> readOptional(const QJsonValue& v) { return v.isString() ? std::optional<QString>(v.toString()) : std::nullopt; }

} // namespace

QJsonObject ChangeRecord::toJson() const
{
    return QJsonObject{ { "id", id }, { "time", time.toString(Qt::ISODateWithMs) }, { "valueName", valueName },
                        { "before", optionalString(before) }, { "after", optionalString(after) },
                        { "origin", origin }, { "rolledBack", rolledBack } };
}

std::optional<ChangeRecord> ChangeRecord::fromJson(const QJsonObject& o)
{
    if (!o.value(QLatin1StringView("id")).isString() || !o.value(QLatin1StringView("valueName")).isString())
        return std::nullopt;
    ChangeRecord r;
    r.id = o.value(QLatin1StringView("id")).toString();
    r.time = QDateTime::fromString(o.value(QLatin1StringView("time")).toString(), Qt::ISODateWithMs);
    r.valueName = o.value(QLatin1StringView("valueName")).toString();
    r.before = readOptional(o.value(QLatin1StringView("before")));
    r.after = readOptional(o.value(QLatin1StringView("after")));
    r.origin = o.value(QLatin1StringView("origin")).toString();
    r.rolledBack = o.value(QLatin1StringView("rolledBack")).toBool();
    return r;
}

ChangeJournal::ChangeJournal(QString filePath, int maxRecords) : m_filePath(std::move(filePath)), m_maxRecords(maxRecords) {}

bool ChangeJournal::load(QString* error)
{
    m_records.clear();
    m_pending.reset();
    QFile f(m_filePath);
    if (!f.exists())
        return true;
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("The preference change journal is not valid JSON.");
        return false;
    }
    for (const QJsonValue& v : doc.object().value(QLatin1StringView("records")).toArray()) {
        if (auto r = ChangeRecord::fromJson(v.toObject()))
            m_records << *r;
    }
    const QJsonObject p = doc.object().value(QLatin1StringView("pendingVerification")).toObject();
    if (p.value(QLatin1StringView("valueName")).isString())
        m_pending = PendingVerification{ p.value(QLatin1StringView("valueName")).toString(), readOptional(p.value(QLatin1StringView("original"))) };
    return true;
}

bool ChangeJournal::save(QString* error) const
{
    QJsonArray records;
    for (const ChangeRecord& r : m_records)
        records << r.toJson();
    QJsonObject root{ { "schemaVersion", 1 }, { "records", records } };
    if (m_pending)
        root.insert(QStringLiteral("pendingVerification"),
                    QJsonObject{ { "valueName", m_pending->valueName }, { "original", optionalString(m_pending->original) } });
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile f(m_filePath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

void ChangeJournal::append(const ChangeRecord& r)
{
    m_records << r;
    while (m_records.size() > m_maxRecords)
        m_records.removeFirst();
}

ChangeRecord* ChangeJournal::find(const QString& id)
{
    for (ChangeRecord& r : m_records) {
        if (r.id == id)
            return &r;
    }
    return nullptr;
}

// ---- manager --------------------------------------------------------------------------------------

PreferenceManager::PreferenceManager(IPreferenceStore& store, ChangeJournal& journal) : m_store(store), m_journal(journal) {}

std::optional<QString> PreferenceManager::rawValue(const QString& exePath) const
{
    const auto name = findValueName(m_store, exePath);
    return name ? m_store.read(*name) : std::nullopt;
}

GpuPreference PreferenceManager::current(const QString& exePath) const
{
    const auto raw = rawValue(exePath);
    return raw ? parseGpuPreference(*raw) : GpuPreference::NotSet;
}

ChangeResult PreferenceManager::writeAndJournal(QString valueName, std::optional<QString> before, std::optional<QString> after, QString origin)
{
    ChangeResult result;
    ChangeRecord record;
    record.time = QDateTime::currentDateTime();
    record.id = QStringLiteral("%1-%2").arg(record.time.toString(QStringLiteral("yyyyMMddHHmmsszzz"))).arg(m_journal.records().size());
    record.valueName = valueName;
    record.before = before;
    record.after = after;
    record.origin = origin;

    // Journal first: if this fails, the registry is not touched.
    m_journal.append(record);
    QString error;
    if (!m_journal.save(&error)) {
        result.error = QStringLiteral("Could not save the change journal, so nothing was changed: %1").arg(error);
        return result;
    }
    const bool ok = after ? m_store.write(valueName, *after, &error) : m_store.remove(valueName, &error);
    if (!ok) {
        // Drop the record of a change that did not happen.
        m_journal.find(record.id)->rolledBack = true;
        m_journal.find(record.id)->origin += QStringLiteral(" (failed)");
        m_journal.save(nullptr);
        result.error = QStringLiteral("Windows refused the change: %1").arg(error);
        return result;
    }
    qCInfo(lcPrefs).noquote() << QStringLiteral("Preference for %1: %2 -> %3 (%4)")
                                     .arg(valueName, before.value_or(QStringLiteral("<none>")), after.value_or(QStringLiteral("<none>")), origin);
    result.ok = true;
    result.record = record;
    return result;
}

ChangeResult PreferenceManager::apply(const QString& exePath, GpuPreference preference, const QString& origin)
{
    ChangeResult result;
    QString why;
    if (!isConfigurableExecutable(exePath, &why)) {
        result.error = why;
        return result;
    }
    if (preference == GpuPreference::Unrecognised) {
        result.error = QStringLiteral("Not a preference that can be set.");
        return result;
    }
    const QString valueName = findValueName(m_store, exePath).value_or(QDir::toNativeSeparators(QDir::cleanPath(exePath)));
    const std::optional<QString> before = m_store.read(valueName);
    const QString next = withGpuPreference(before.value_or(QString()), preference);
    const std::optional<QString> after = next.isEmpty() ? std::nullopt : std::optional<QString>(next);
    if (before == after) {
        result.ok = true; // already in that state; nothing written, nothing journalled
        return result;
    }
    return writeAndJournal(valueName, before, after, origin);
}

ChangeResult PreferenceManager::rollback(const QString& recordId, bool force)
{
    ChangeResult result;
    ChangeRecord* record = m_journal.find(recordId);
    if (!record) {
        result.error = QStringLiteral("That change is not in the journal.");
        return result;
    }
    if (record->rolledBack) {
        result.error = QStringLiteral("That change has already been undone.");
        return result;
    }
    const std::optional<QString> now = m_store.read(record->valueName);
    if (now != record->after && !force) {
        result.error = QStringLiteral("The setting was changed outside GPU Fusion since (it is now \"%1\"). Undoing would overwrite that newer choice.")
                           .arg(now.value_or(QStringLiteral("not set")));
        return result;
    }
    const QString id = record->id;
    const QString valueName = record->valueName;
    const std::optional<QString> restore = record->before;
    record = nullptr; // writeAndJournal appends to the journal; do not hold pointers into it
    result = writeAndJournal(valueName, now, restore, QStringLiteral("undo of %1").arg(id));
    if (result.ok) {
        m_journal.find(id)->rolledBack = true; // re-find: append may have reallocated
        m_journal.save(nullptr);
    }
    return result;
}

// ---- verification ---------------------------------------------------------------------------------

namespace {

std::optional<ProbeAdapter> adapterFrom(const QJsonValue& v)
{
    if (!v.isObject())
        return std::nullopt;
    const QJsonObject o = v.toObject();
    return ProbeAdapter{ o.value(QLatin1StringView("name")).toString(), quint32(o.value(QLatin1StringView("vendorId")).toInteger()),
                         o.value(QLatin1StringView("luid")).toString() };
}

QJsonValue adapterJson(const std::optional<ProbeAdapter>& a)
{
    if (!a)
        return QJsonValue::Null;
    return QJsonObject{ { "name", a->name }, { "vendorId", qint64(a->vendorId) }, { "luid", a->luid } };
}

} // namespace

std::optional<ProbeReport> parseProbeReport(const QByteArray& json, QString* error)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.trimmed(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("Probe output is not valid JSON (%1).").arg(pe.errorString());
        return std::nullopt;
    }
    const QJsonObject o = doc.object();
    ProbeReport r;
    r.exe = o.value(QLatin1StringView("exe")).toString();
    for (const QJsonValue& v : o.value(QLatin1StringView("enumOrder")).toArray()) {
        if (auto a = adapterFrom(v))
            r.enumOrder << *a;
    }
    r.d3d11Default = adapterFrom(o.value(QLatin1StringView("d3d11Default")));
    const QJsonObject by = o.value(QLatin1StringView("byPreference")).toObject();
    r.unspecified = adapterFrom(by.value(QLatin1StringView("unspecified")));
    r.minimumPower = adapterFrom(by.value(QLatin1StringView("minimumPower")));
    r.highPerformance = adapterFrom(by.value(QLatin1StringView("highPerformance")));
    if (o.contains(QLatin1StringView("error")))
        r.error = o.value(QLatin1StringView("error")).toString();
    else if (o.contains(QLatin1StringView("d3d11Error")))
        r.error = QStringLiteral("Direct3D 11 device creation failed (%1)").arg(o.value(QLatin1StringView("d3d11Error")).toString());
    return r;
}

bool VerificationResult::preferenceHasNoEffect() const
{
    QString luid;
    int ok = 0;
    for (const VerificationStep& s : steps) {
        if (!s.ok || !s.report.d3d11Default)
            continue;
        if (ok++ == 0)
            luid = s.report.d3d11Default->luid;
        else if (s.report.d3d11Default->luid != luid)
            return false;
    }
    return ok >= 2;
}

QJsonObject VerificationResult::toJson() const
{
    QJsonArray arr;
    for (const VerificationStep& s : steps) {
        QJsonArray order;
        for (const ProbeAdapter& a : s.report.enumOrder)
            order << adapterJson(a);
        arr << QJsonObject{ { "preference", gpuPreferenceCode(s.preference) }, { "preferenceText", gpuPreferenceText(s.preference) },
                            { "ok", s.ok }, { "error", s.error }, { "d3d11Default", adapterJson(s.report.d3d11Default) },
                            { "enumOrder", order }, { "minimumPower", adapterJson(s.report.minimumPower) },
                            { "highPerformance", adapterJson(s.report.highPerformance) } };
    }
    return QJsonObject{ { "schemaVersion", 1 }, { "time", time.toString(Qt::ISODateWithMs) }, { "probePath", probePath },
                        { "steps", arr }, { "restored", restored }, { "restoreError", restoreError } };
}

std::optional<VerificationResult> VerificationResult::fromJson(const QJsonObject& o)
{
    if (o.value(QLatin1StringView("schemaVersion")).toInt() != 1)
        return std::nullopt;
    VerificationResult r;
    r.time = QDateTime::fromString(o.value(QLatin1StringView("time")).toString(), Qt::ISODateWithMs);
    r.probePath = o.value(QLatin1StringView("probePath")).toString();
    r.restored = o.value(QLatin1StringView("restored")).toBool();
    r.restoreError = o.value(QLatin1StringView("restoreError")).toString();
    for (const QJsonValue& v : o.value(QLatin1StringView("steps")).toArray()) {
        const QJsonObject s = v.toObject();
        VerificationStep step;
        const int code = s.value(QLatin1StringView("preference")).toInt(-1);
        step.preference = code == 0 ? GpuPreference::LetWindowsDecide : code == 1 ? GpuPreference::PowerSaving
                        : code == 2 ? GpuPreference::HighPerformance : GpuPreference::NotSet;
        step.ok = s.value(QLatin1StringView("ok")).toBool();
        step.error = s.value(QLatin1StringView("error")).toString();
        step.report.d3d11Default = adapterFrom(s.value(QLatin1StringView("d3d11Default")));
        step.report.minimumPower = adapterFrom(s.value(QLatin1StringView("minimumPower")));
        step.report.highPerformance = adapterFrom(s.value(QLatin1StringView("highPerformance")));
        for (const QJsonValue& a : s.value(QLatin1StringView("enumOrder")).toArray()) {
            if (auto ad = adapterFrom(a))
                step.report.enumOrder << *ad;
        }
        r.steps << step;
    }
    return r;
}

namespace {

bool setRaw(IPreferenceStore& store, const QString& name, const std::optional<QString>& data, QString* error)
{
    return data ? store.write(name, *data, error) : store.remove(name, error);
}

} // namespace

VerificationResult verifyPreferences(IPreferenceStore& store, ChangeJournal& journal, const QString& probePath, const ProbeRunner& run)
{
    VerificationResult result;
    result.time = QDateTime::currentDateTime();
    result.probePath = QDir::toNativeSeparators(probePath);
    const QString name = findValueName(store, probePath).value_or(result.probePath);
    const std::optional<QString> original = store.read(name);

    // Marker first: if GPU Fusion stops mid-run, recoverPending() puts the original back.
    journal.setPending(PendingVerification{ name, original });
    QString error;
    if (!journal.save(&error)) {
        journal.setPending(std::nullopt);
        result.restoreError = QStringLiteral("Could not save the journal, so verification did not start: %1").arg(error);
        result.restored = true; // nothing was changed
        return result;
    }

    for (GpuPreference p : { GpuPreference::NotSet, GpuPreference::LetWindowsDecide, GpuPreference::PowerSaving, GpuPreference::HighPerformance }) {
        VerificationStep step;
        step.preference = p;
        const QString data = withGpuPreference(original.value_or(QString()), p);
        QString err;
        if (!setRaw(store, name, data.isEmpty() ? std::nullopt : std::optional<QString>(data), &err)) {
            step.error = QStringLiteral("Could not set the preference: %1").arg(err);
        } else if (const auto out = run(probePath, &err); !out) {
            step.error = QStringLiteral("The probe did not run: %1").arg(err);
        } else if (const auto report = parseProbeReport(*out, &err); !report) {
            step.error = err;
        } else {
            step.report = *report;
            step.ok = report->error.isEmpty() && report->d3d11Default.has_value();
            step.error = report->error;
        }
        qCInfo(lcPrefs).noquote() << QStringLiteral("Verification: %1 -> %2").arg(gpuPreferenceText(p),
            step.ok ? step.report.d3d11Default->name : QStringLiteral("failed: %1").arg(step.error));
        result.steps << step;
    }

    // Restore exactly what was there, and check by reading it back.
    QString err;
    if (!setRaw(store, name, original, &err)) {
        result.restoreError = err;
    } else if (store.read(name) != original) {
        result.restoreError = QStringLiteral("The value read back does not match the original.");
    } else {
        result.restored = true;
        journal.setPending(std::nullopt);
        journal.save(nullptr);
    }
    if (!result.restored)
        qCWarning(lcPrefs).noquote() << "Verification could not restore the probe's preference:" << result.restoreError
                                     << "- it will be retried at next start";
    return result;
}

bool recoverPending(IPreferenceStore& store, ChangeJournal& journal, QString* error)
{
    const auto pending = journal.pending();
    if (!pending)
        return false;
    QString err;
    if (!setRaw(store, pending->valueName, pending->original, &err)) {
        if (error) *error = err;
        return false;
    }
    journal.setPending(std::nullopt);
    journal.save(nullptr);
    qCInfo(lcPrefs).noquote() << "Restored the probe preference left by an interrupted verification:" << pending->valueName;
    return true;
}

} // namespace gf::pref
