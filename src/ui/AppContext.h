#pragma once

#include "core/AppPaths.h"
#include "core/Settings.h"

#include <QObject>

namespace gf::hw { class HardwareService; }
namespace gf::mon { class MonitorService; }
namespace gf::pref { class PreferenceService; }
namespace gf::bench { class BenchmarkService; }
namespace gf::profiles { class ProfileStore; }

namespace gf::ui {

// Application-wide state shared by the pages. Owned by main(); outlives the main window.
class AppContext : public QObject {
    Q_OBJECT
public:
    AppContext(DataLocation data, SettingsStore* store, SettingsStore::LoadResult loaded,
               hw::HardwareService* hardware, mon::MonitorService* monitor, pref::PreferenceService* preferences,
               bench::BenchmarkService* benchmarks = nullptr, bench::BenchmarkService* lab = nullptr,
               profiles::ProfileStore* profiles = nullptr);

    const DataLocation& data() const { return m_data; }
    hw::HardwareService& hardware() { return *m_hardware; }
    mon::MonitorService& monitor() { return *m_monitor; }
    pref::PreferenceService& preferences() { return *m_preferences; }
    bench::BenchmarkService* benchmarks() { return m_benchmarks; } // null where benchmarking is not offered (tests)
    bench::BenchmarkService* lab() { return m_lab; }
    profiles::ProfileStore* profiles() { return m_profiles; }         // null in tests that do not need it               // Workload Lab sessions; null in tests that do not need it
    SettingsStore& store() { return *m_store; }
    const Settings& settings() const { return m_settings; }
    const SettingsStore::LoadResult& loadResult() const { return m_loaded; }

    // Validates, saves and broadcasts new settings. Returns false with *error on failure;
    // the in-memory settings are only replaced when the save succeeds.
    bool updateSettings(const Settings& settings, QString* error);
    // Updates non-user-facing state (window geometry) without broadcasting.
    void rememberWindowGeometry(const QByteArray& geometry);

Q_SIGNALS:
    void settingsChanged(const gf::Settings& settings);

private:
    DataLocation m_data;
    SettingsStore* m_store;
    SettingsStore::LoadResult m_loaded;
    Settings m_settings;
    hw::HardwareService* m_hardware;
    mon::MonitorService* m_monitor;
    pref::PreferenceService* m_preferences;
    bench::BenchmarkService* m_benchmarks;
    bench::BenchmarkService* m_lab;
    profiles::ProfileStore* m_profiles;
};

} // namespace gf::ui
