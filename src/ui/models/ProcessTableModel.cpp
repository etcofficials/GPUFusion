#include "ui/models/ProcessTableModel.h"

#include "platform/windows/GpuPreferenceReader.h"
#include "ui/Theme.h"

#include <QFileIconProvider>
#include <QFileInfo>
#include <QSet>

namespace gf::ui {

using namespace gf::mon;

namespace {

QString keyOf(quint32 pid, quint64 luid) { return QStringLiteral("%1:%2").arg(pid).arg(luid); }

double sortValue(const MetricValue& v) { return v.isOk() ? v.value : -1.0; }

bool sameValue(const MetricValue& a, const MetricValue& b)
{
    return a.state == b.state && (a.state != MetricValue::State::Ok || a.value == b.value) && a.note == b.note;
}

// True if a row would display the same thing for both usages.
bool sameShownValues(const ProcessAdapterUsage& a, const ProcessAdapterUsage& b)
{
    return sameValue(a.utilization, b.utilization) && sameValue(a.dedicated, b.dedicated) && sameValue(a.shared, b.shared) &&
           a.busiestEngine == b.busiestEngine && activityOf(a) == activityOf(b) && a.source == b.source;
}

} // namespace

ProcessTableModel::ProcessTableModel(QObject* parent) : QAbstractTableModel(parent) {}

void ProcessTableModel::setPreferences(const QHash<QString, GpuPreference>& byPath)
{
    if (byPath == m_preferences)
        return;
    m_preferences = byPath;
    if (!m_rows.isEmpty())
        Q_EMIT dataChanged(index(0, Preference), index(int(m_rows.size()) - 1, Preference));
}

GpuPreference ProcessTableModel::preferenceFor(const QString& path) const
{
    if (path.isEmpty())
        return GpuPreference::NotSet;
    return m_preferences.value(win::preferenceKey(path), GpuPreference::NotSet);
}

void ProcessTableModel::update(const ProcessSnapshot& snapshot)
{
    // Views may call data() inside every begin/end notification below, so m_rows and m_processes
    // must agree at each of those points: the old process list stays in place while rows are
    // removed, and is swapped together with the row update in step 2 (no signals in between).
    const QList<ProcessSample>& incoming = snapshot.processes;

    // Desired rows, by key (processIndex refers to `incoming`).
    QList<Row> next;
    for (int i = 0; i < incoming.size(); ++i) {
        const ProcessSample& p = incoming[i];
        if (p.adapters.isEmpty()) {
            next << Row{ keyOf(p.pid, 0), p.pid, 0, i, std::nullopt };
            continue;
        }
        for (const ProcessAdapterUsage& u : p.adapters)
            next << Row{ keyOf(p.pid, u.luid), p.pid, u.luid, i, u };
    }
    QSet<QString> nextKeys;
    for (const Row& r : next)
        nextKeys.insert(r.key);

    // 1. Remove rows that disappeared (process exited or released the adapter), from the end.
    for (int i = int(m_rows.size()) - 1; i >= 0; --i) {
        if (!nextKeys.contains(m_rows[i].key)) {
            beginRemoveRows({}, i, i);
            m_rows.removeAt(i);
            endRemoveRows();
        }
    }
    m_rowByKey.clear();
    for (int i = 0; i < m_rows.size(); ++i)
        m_rowByKey.insert(m_rows[i].key, i);

    // 2. Switch to the new process list and update existing rows in place, collect new ones.
    //    Only rows whose shown values changed are signalled: most processes are idle, and a change
    //    signal for every row makes the sort proxy re-sort and the view repaint everything
    //    (measured: ~56 ms of UI time per refresh with ~170 processes).
    const QList<ProcessSample> previous = std::exchange(m_processes, incoming);
    QList<Row> added;
    QList<int> changed;
    for (const Row& r : next) {
        const auto it = m_rowByKey.constFind(r.key);
        if (it == m_rowByKey.constEnd()) {
            added << r;
            continue;
        }
        const Row& old = m_rows[*it];
        const bool same = old.usage.has_value() == r.usage.has_value() &&
                          (!r.usage || sameShownValues(*old.usage, *r.usage)) &&
                          previous[old.processIndex].path == incoming[r.processIndex].path;
        m_rows[*it] = r;
        if (!same)
            changed << *it;
    }
    std::sort(changed.begin(), changed.end());
    for (int i = 0; i < changed.size();) { // signal contiguous runs
        int j = i;
        while (j + 1 < changed.size() && changed[j + 1] == changed[j] + 1)
            ++j;
        Q_EMIT dataChanged(index(changed[i], 0), index(changed[j], ColumnCount - 1));
        i = j + 1;
    }

    // 3. Append new rows.
    if (!added.isEmpty()) {
        beginInsertRows({}, int(m_rows.size()), int(m_rows.size() + added.size()) - 1);
        for (const Row& r : added) {
            m_rowByKey.insert(r.key, int(m_rows.size()));
            m_rows << r;
        }
        endInsertRows();
    }
}

const ProcessSample* ProcessTableModel::process(const QString& key) const
{
    const auto it = m_rowByKey.constFind(key);
    if (it == m_rowByKey.constEnd())
        return nullptr;
    const int pi = m_rows[*it].processIndex;
    return pi >= 0 && pi < m_processes.size() ? &m_processes[pi] : nullptr;
}

QString ProcessTableModel::adapterName(quint64 luid) const
{
    if (luid == 0)
        return QStringLiteral("—");
    const auto it = m_adapters.constFind(luid);
    if (it == m_adapters.constEnd())
        return QStringLiteral("Adapter %1").arg(hw::formatLuid(luid));
    return it->software ? QStringLiteral("%1 (software, CPU)").arg(it->name) : it->name;
}

QIcon ProcessTableModel::iconFor(const QString& path) const
{
    if (path.isEmpty())
        return {};
    auto it = m_icons.constFind(path);
    if (it == m_icons.constEnd()) {
        static QFileIconProvider provider;
        it = m_icons.insert(path, provider.icon(QFileInfo(path)));
    }
    return *it;
}

int ProcessTableModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }
int ProcessTableModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant ProcessTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal)
        return {};
    if (role == Qt::DisplayRole) {
        switch (section) {
        case Application: return tr("Application");
        case Pid:         return tr("PID");
        case Gpu:         return tr("GPU");
        case Usage:       return tr("GPU usage");
        case Engine:      return tr("Busiest engine");
        case Dedicated:   return tr("Dedicated");
        case Shared:      return tr("Shared");
        case Activity:    return tr("Activity");
        case Preference:  return tr("Preference");
        default:          return {};
        }
    }
    if (role == Qt::ToolTipRole) {
        switch (section) {
        case Usage:      return tr("This process's busiest engine on this GPU, as a share of that engine's time (per-process, not the adapter total)");
        case Dedicated:  return tr("GPU-local memory this process holds on this adapter");
        case Shared:     return tr("System memory this process has mapped for this adapter");
        case Activity:   return tr("Active = used an engine in the latest sample. Memory only = holds GPU memory but did no GPU work, which is not counted as using the GPU.");
        case Preference: return tr("The per-app setting in Windows Settings > Display > Graphics. A preference is not proof of which GPU the app uses.");
        default:         return {};
        }
    }
    return {};
}

QVariant ProcessTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows[index.row()];
    const ProcessSample& p = m_processes[r.processIndex];
    const ProcessAdapterUsage* u = r.usage ? &*r.usage : nullptr;
    const mon::Activity activity = u ? activityOf(*u) : mon::Activity::None;

    if (role == KeyRole)
        return r.key;
    if (role == LuidRole)
        return QVariant::fromValue(r.luid);
    if (role == ActivityRole)
        return int(activity);

    switch (index.column()) {
    case Application:
        if (role == Qt::DisplayRole) return p.name;
        if (role == SortRole) return p.name.toLower();
        if (role == Qt::ToolTipRole) return p.path.isEmpty() ? p.pathError : p.path;
        if (role == Qt::DecorationRole) return iconFor(p.path);
        break;
    case Pid:
        if (role == Qt::DisplayRole || role == SortRole) return p.pid;
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    case Gpu:
        if (role == Qt::DisplayRole || role == SortRole) return adapterName(r.luid);
        break;
    case Usage:
    case Dedicated:
    case Shared: {
        if (!u) {
            if (role == Qt::DisplayRole) return QStringLiteral("—");
            if (role == SortRole) return -2.0;
            break;
        }
        const MetricValue& v = index.column() == Usage ? u->utilization : index.column() == Dedicated ? u->dedicated : u->shared;
        if (role == Qt::DisplayRole) return formatMetric(v, index.column() == Usage ? Unit::Percent : Unit::Bytes);
        if (role == SortRole) return sortValue(v);
        if (role == Qt::ToolTipRole) return v.isOk() ? QStringLiteral("Source: %1").arg(u->source) : v.note;
        if (role == Qt::ForegroundRole && !v.isOk()) return colors().textMuted;
        if (role == Qt::TextAlignmentRole) return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    }
    case Engine:
        if (role == Qt::DisplayRole || role == SortRole) return u ? u->busiestEngine : QString();
        break;
    case Activity:
        if (role == Qt::DisplayRole) return u ? activityText(activity) : QStringLiteral("—");
        if (role == SortRole) return activity == mon::Activity::Active ? 2 : activity == mon::Activity::MemoryOnly ? 1 : 0;
        if (role == Qt::ForegroundRole && activity != mon::Activity::Active) return colors().textMuted;
        break;
    case Preference:
        if (role == Qt::DisplayRole || role == SortRole) {
            if (p.path.isEmpty())
                return QStringLiteral("Unknown");
            const GpuPreference pref = preferenceFor(p.path);
            return pref == GpuPreference::NotSet ? QString() : gpuPreferenceText(pref);
        }
        if (role == Qt::ToolTipRole && p.path.isEmpty())
            return tr("The executable path is not readable, so the preference cannot be looked up");
        break;
    default:
        break;
    }
    return {};
}

// ---- filter -----------------------------------------------------------------------------------

void ProcessFilterModel::setSearch(const QString& text)
{
    m_search = text.trimmed();
    invalidateRowsFilter();
}

void ProcessFilterModel::setAdapter(quint64 luid)
{
    m_adapter = luid;
    invalidateRowsFilter();
}

void ProcessFilterModel::setShowAll(bool showAll)
{
    m_showAll = showAll;
    invalidateRowsFilter();
}

bool ProcessFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    const QModelIndex first = sourceModel()->index(sourceRow, 0, sourceParent);
    const quint64 luid = first.data(ProcessTableModel::LuidRole).value<quint64>();
    if (m_adapter != 0 && luid != m_adapter)
        return false;
    if (!m_showAll && first.data(ProcessTableModel::ActivityRole).toInt() == int(mon::Activity::None))
        return false;
    if (m_search.isEmpty())
        return true;
    const QString pid = sourceModel()->index(sourceRow, ProcessTableModel::Pid, sourceParent).data().toString();
    return first.data(Qt::DisplayRole).toString().contains(m_search, Qt::CaseInsensitive) ||
           first.data(Qt::ToolTipRole).toString().contains(m_search, Qt::CaseInsensitive) || pid == m_search;
}

} // namespace gf::ui
