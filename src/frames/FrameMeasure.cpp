#include "frames/FrameMeasure.h"

#include "core/Logging.h"
#include "monitoring/ProcessModel.h"
#include "platform/windows/KmtProcessProvider.h"
#include "platform/windows/PdhGpuProcessProvider.h"
#include "platform/windows/ProcessList.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QThread>

#include <windows.h>
#include <shellapi.h>

namespace gf::frames {

namespace win = mon::win;

bool isElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) && GetTokenInformation(token, TokenElevation, &e, sizeof e, &size);
    if (token)
        CloseHandle(token);
    return ok && e.TokenIsElevated;
}

namespace {

double processCpuMs()
{
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    auto v = [](const FILETIME& f) { return double((quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}

QString quoteArg(const QString& a) { return QStringLiteral("\"%1\"").arg(a); }

// Starts the helper; with `elevate` through the administrator prompt. Returns the process handle, or
// null with *error (ERROR_CANCELLED when the user declined the prompt).
HANDLE startHelper(const QString& exe, const QStringList& args, bool elevate, DWORD* error)
{
    QString params;
    for (const QString& a : args)
        params += (params.isEmpty() ? QString() : QStringLiteral(" ")) + quoteArg(a);
    const std::wstring file = QDir::toNativeSeparators(exe).toStdWString();
    std::wstring p = params.toStdWString();
    if (elevate) {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = file.c_str();
        sei.lpParameters = p.c_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            *error = GetLastError();
            return nullptr;
        }
        return sei.hProcess;
    }
    std::wstring cmd = L"\"" + file + L"\" " + p;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(file.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        *error = GetLastError();
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

struct Accumulator {
    QString adapter, source;
    int samples = 0;
    double sum = 0, max = 0;
    QHash<QString, int> engines;
};

} // namespace

FrameSession measureFrames(const QList<mon::MonitorTarget>& targets, const MeasureOptions& o, const std::atomic<bool>* cancel,
                           const MeasureProgress& progress)
{
    FrameSession s;
    s.id = newSessionId();
    s.label = o.label;
    s.created = QDateTime::currentDateTime();
    s.pid = o.pid;
    s.processName = o.processName;
    s.processPath = o.processPath;

    win::ProcessList list;
    const QList<win::ProcessInfo> processes = list.snapshot();
    const auto self = std::find_if(processes.begin(), processes.end(), [&](const win::ProcessInfo& p) { return p.pid == o.pid; });
    if (self == processes.end()) {
        s.error = QStringLiteral("No running process %1.").arg(o.pid);
        return s;
    }
    if (s.processName.isEmpty())
        s.processName = self->name;
    if (s.processPath.isEmpty())
        s.processPath = self->path;
    if (!QFileInfo::exists(o.helperPath)) {
        s.error = QStringLiteral("The capture helper is missing: %1").arg(QDir::toNativeSeparators(o.helperPath));
        return s;
    }

    QDir().mkpath(o.workDir);
    const QString out = QDir(o.workDir).filePath(s.id + QStringLiteral("-capture.json"));
    const QString stop = QDir(o.workDir).filePath(s.id + QStringLiteral(".stop"));
    QFile::remove(out);
    QFile::remove(stop);
    const QStringList args{ QStringLiteral("--out"), QDir::toNativeSeparators(out), QStringLiteral("--seconds"), QString::number(o.seconds),
                            QStringLiteral("--pid"), QString::number(o.pid), QStringLiteral("--stop-file"), QDir::toNativeSeparators(stop) };
    const bool elevate = !isElevated();
    if (progress)
        progress(elevate ? QStringLiteral("Waiting for the administrator prompt…") : QStringLiteral("Starting the capture…"), 0);
    DWORD startError = 0;
    HANDLE helper = startHelper(o.helperPath, args, elevate, &startError);
    if (!helper) {
        s.error = startError == ERROR_CANCELLED
            ? QStringLiteral("The administrator prompt was declined, so nothing was measured. Frame-time capture uses Windows' graphics event "
                             "tracing, which needs administrator rights for each capture.")
            : QStringLiteral("The capture helper could not be started (error %1).").arg(startError);
        qCWarning(lcApp).noquote() << "Frame capture:" << s.error;
        return s;
    }
    qCInfo(lcApp).noquote() << QStringLiteral("Frame capture: measuring %1 (process %2) for %3 s%4").arg(s.processName).arg(o.pid).arg(o.seconds)
                                   .arg(elevate ? QStringLiteral(", helper started through the administrator prompt") : QString());

    // While the helper captures: GPU Fusion's estimate of the process's GPU use, once a second.
    QList<quint64> pdhLuids;
    QList<win::KmtProcessProvider::Adapter> legacy;
    QHash<quint64, Accumulator> acc;
    for (const mon::MonitorTarget& t : targets) {
        if (t.software)
            continue;
        acc[t.luid].adapter = t.name;
        if (t.wddmVersion >= 2000) {
            pdhLuids << t.luid;
            acc[t.luid].source = QStringLiteral("Windows GPU performance counters");
        } else {
            legacy << win::KmtProcessProvider::Adapter{ t.luid, t.name };
            acc[t.luid].source = QStringLiteral("D3DKMT process statistics");
        }
    }
    win::PdhGpuProcessProvider pdh;
    win::KmtProcessProvider kmt;
    kmt.setAdapters(legacy);
    QStringList notes;
    const double cpu0 = processCpuMs();
    QElapsedTimer clock;
    clock.start();
    bool stopRequested = false;
    for (int pass = 0;; ++pass) {
        if (WaitForSingleObject(helper, 0) == WAIT_OBJECT_0)
            break;
        if (!stopRequested && cancel && cancel->load()) {
            QFile f(stop);
            f.open(QIODevice::WriteOnly);
            stopRequested = true;
        }
        if (clock.elapsed() > qint64(o.seconds + 60) * 1000) { // the helper should have ended long ago
            notes << QStringLiteral("the capture helper did not finish in time");
            break;
        }
        QList<win::ProcessInfo> member;
        for (const win::ProcessInfo& p : list.snapshot())
            if (p.pid == o.pid)
                member << p;
        QHash<quint32, QList<mon::ProcessAdapterUsage>> usage;
        if (!pdhLuids.isEmpty()) {
            QList<mon::PdhEngineReading> engines;
            QList<mon::PdhMemoryReading> memory;
            if (pdh.collect(&engines, &memory))
                usage = mon::aggregatePdh(engines, memory, pdhLuids);
        }
        int inspectable = 0, notInspectable = 0;
        QList<win::ProcessInfo> skipped;
        kmt.sample(member, usage, &inspectable, &notInspectable, &skipped);
        if (!skipped.isEmpty() && !notes.contains(QStringLiteral("D3DKMT could not inspect the process")))
            notes << QStringLiteral("D3DKMT could not inspect the process");
        // The first pass only primes the rate counters.
        if (pass > 0) {
            for (const mon::ProcessAdapterUsage& u : usage.value(o.pid)) {
                if (!acc.contains(u.luid) || u.utilization.state != mon::MetricValue::State::Ok)
                    continue;
                Accumulator& a = acc[u.luid];
                ++a.samples;
                a.sum += u.utilization.value;
                a.max = std::max(a.max, u.utilization.value);
                if (!u.busiestEngine.isEmpty())
                    ++a.engines[u.busiestEngine];
            }
        }
        if (progress)
            progress(QStringLiteral("Measuring %1…").arg(s.processName), std::min(99, int(100 * clock.elapsed() / std::max(1, o.seconds * 1000))));
        const qint64 next = qint64(pass + 1) * 1000;
        while (clock.elapsed() < next && WaitForSingleObject(helper, 50) == WAIT_TIMEOUT) {
            if (!stopRequested && cancel && cancel->load())
                break;
        }
    }
    WaitForSingleObject(helper, 10000);
    DWORD code = 0;
    GetExitCodeProcess(helper, &code);
    CloseHandle(helper);
    const double elapsed = double(clock.elapsed());
    s.fusionCpuPercent = elapsed > 0 ? 100.0 * (processCpuMs() - cpu0) / elapsed / std::max(1, QThread::idealThreadCount()) : -1;
    QFile::remove(stop);

    QFile f(out);
    if (!f.open(QIODevice::ReadOnly)) {
        s.error = QStringLiteral("The capture produced no result (helper exit code %1).").arg(code);
        return s;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    QFile::remove(out); // its content is kept inside the session
    s.captureJson = doc.object();
    s.capture = Capture::fromJson(s.captureJson);
    if (!s.capture.ok)
        s.error = s.capture.error.isEmpty() ? QStringLiteral("The capture failed (helper exit code %1).").arg(code) : s.capture.error;

    for (auto it = acc.cbegin(); it != acc.cend(); ++it) {
        const Accumulator& a = it.value();
        if (a.samples == 0)
            continue;
        GpuEstimate g;
        g.adapter = a.adapter;
        g.luid = it.key();
        g.samples = a.samples;
        g.meanPercent = a.sum / a.samples;
        g.maxPercent = a.max;
        g.source = a.source;
        int best = 0;
        for (auto e = a.engines.cbegin(); e != a.engines.cend(); ++e)
            if (e.value() > best) {
                best = e.value();
                g.busiestEngine = e.key();
            }
        s.gpu << g;
    }
    if (s.gpu.isEmpty())
        notes << QStringLiteral("no GPU engine readings for the process during the capture");
    s.gpuNotes = notes;
    qCInfo(lcApp).noquote() << "Frame capture:" << (s.measured() ? summaryLines(s).value(0) : s.error);
    return s;
}

} // namespace gf::frames
