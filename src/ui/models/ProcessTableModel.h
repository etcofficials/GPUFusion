#pragma once

#include "monitoring/ProcessModel.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QIcon>
#include <QSortFilterProxyModel>

namespace gf::ui {

// One row per (process, adapter) the process has a footprint on; processes with no footprint get
// one row without an adapter. Rows are updated in place by key so sorting, selection and scroll
// position survive each refresh.
class ProcessTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Application, Pid, Gpu, Usage, Engine, Dedicated, Shared, Activity, Preference, ColumnCount };
    static constexpr int SortRole = Qt::UserRole;
    static constexpr int KeyRole = Qt::UserRole + 1;      // QString "pid:luid"
    static constexpr int LuidRole = Qt::UserRole + 2;     // quint64 (0 = no adapter)
    static constexpr int ActivityRole = Qt::UserRole + 3; // int(mon::Activity)

    struct AdapterInfo { QString name; bool software = false; };

    explicit ProcessTableModel(QObject* parent = nullptr);

    void setAdapters(const QHash<quint64, AdapterInfo>& adapters) { m_adapters = adapters; }
    void setPreferences(const QHash<QString, mon::GpuPreference>& byPath);
    void update(const mon::ProcessSnapshot& snapshot);

    // Process for a key ("pid:luid"); nullptr if it is no longer present.
    const mon::ProcessSample* process(const QString& key) const;
    QString adapterName(quint64 luid) const;
    bool isSoftwareAdapter(quint64 luid) const { return m_adapters.value(luid).software; }
    mon::GpuPreference preferenceFor(const QString& path) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    struct Row {
        QString key;
        quint32 pid = 0;
        quint64 luid = 0;
        int processIndex = -1; // into m_processes
        std::optional<mon::ProcessAdapterUsage> usage;
    };
    QIcon iconFor(const QString& path) const;

    QList<Row> m_rows;
    QHash<QString, int> m_rowByKey;
    QList<mon::ProcessSample> m_processes;
    QHash<quint64, AdapterInfo> m_adapters;
    QHash<QString, mon::GpuPreference> m_preferences;
    mutable QHash<QString, QIcon> m_icons;
};

class ProcessFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;
    void setSearch(const QString& text);
    void setAdapter(quint64 luid);      // 0 = all adapters
    void setShowAll(bool showAll);      // false: only rows with engine activity or GPU memory

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;

private:
    QString m_search;
    quint64 m_adapter = 0;
    bool m_showAll = false;
};

} // namespace gf::ui
