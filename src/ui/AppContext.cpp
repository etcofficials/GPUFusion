#include "ui/AppContext.h"

#include "core/Logging.h"

namespace gf::ui {

AppContext::AppContext(DataLocation data, SettingsStore* store, SettingsStore::LoadResult loaded,
                       hw::HardwareService* hardware, mon::MonitorService* monitor, pref::PreferenceService* preferences,
                       bench::BenchmarkService* benchmarks, bench::BenchmarkService* lab, profiles::ProfileStore* profiles)
    : m_data(std::move(data)), m_store(store), m_loaded(std::move(loaded)), m_settings(m_loaded.settings),
      m_hardware(hardware), m_monitor(monitor), m_preferences(preferences), m_benchmarks(benchmarks), m_lab(lab), m_profiles(profiles)
{
}

bool AppContext::updateSettings(const Settings& settings, QString* error)
{
    if (!m_store->save(settings, error)) {
        qCWarning(lcSettings).noquote() << "Saving settings failed:" << (error ? *error : QString());
        return false;
    }
    m_settings = settings;
    qCInfo(lcSettings).noquote() << "Settings saved to" << m_store->filePath();
    Q_EMIT settingsChanged(m_settings);
    return true;
}

void AppContext::rememberWindowGeometry(const QByteArray& geometry)
{
    if (m_store->isReadOnly() || geometry == m_settings.windowGeometry)
        return;
    Settings next = m_settings;
    next.windowGeometry = geometry;
    QString error;
    if (m_store->save(next, &error))
        m_settings = next;
    else
        qCWarning(lcSettings).noquote() << "Could not save window geometry:" << error;
}

} // namespace gf::ui
