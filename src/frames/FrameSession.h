#pragma once

// One frame-time measurement of one program, as GPU Fusion keeps it: what the capture measured (frame
// times), what GPU Fusion's monitoring estimated at the same time (GPU engine utilization of that
// process, sampled once a second), the measurement's own cost, and a label such as "before" or
// "after". Saved as JSON under <data>\frames; a readable HTML report goes to <data>\reports.
// Two measurements of the same program can be compared ("before/after").

#include "benchmarks/BenchmarkModel.h"
#include "frames/FrameCapture.h"

#include <QJsonObject>

#include <optional>

namespace gf::frames {

// GPU Fusion's estimate of one adapter's use by the measured process during the capture.
struct GpuEstimate {
    QString adapter;
    quint64 luid = 0;
    int samples = 0;           // samples with a reading
    double meanPercent = 0;    // busiest engine, mean of the samples
    double maxPercent = 0;
    QString busiestEngine;     // most often the busiest
    QString source;            // "Windows GPU performance counters" or "D3DKMT process statistics"
};

struct FrameSession {
    QString id;                // "frames-20261028-143000-123"
    QString label;             // the user's words, e.g. "before"
    QDateTime created;
    quint32 pid = 0;
    QString processName, processPath;
    Capture capture;
    QJsonObject captureJson;   // the helper's output, kept as measured
    QList<GpuEstimate> gpu;
    QStringList gpuNotes;      // why an estimate is missing, etc.
    double fusionCpuPercent = -1; // GPU Fusion's own CPU use during the capture (share of all cores); -1 = not measured
    QString error;             // set when nothing was measured

    bool measured() const { return error.isEmpty() && capture.ok; }
    const CapturedProcess* target() const { return capture.process(pid); }

    QJsonObject toJson() const;
    static std::optional<FrameSession> fromJson(const QJsonObject& o);
};

QString newSessionId();
bool saveSession(const QString& dir, const FrameSession& s, QString* error);
std::optional<FrameSession> loadSession(const QString& file, QString* error = nullptr);
// Every saved session, newest first.
QList<FrameSession> loadSessions(const QString& dir);

// Before/after: the main streams' frame times. The samples compared are the mean frame time of each
// whole second (consecutive frame times are not independent), with the significance rule used
// everywhere in GPU Fusion (difference > twice the combined standard error and ≥ 1 %).
struct FrameComparison {
    bool comparable = false;
    QString reason;            // why not comparable
    FrameStats before, after;
    bench::Difference difference; // after vs before: Faster = shorter frame times
    QString text;              // the verdict in words
};
FrameComparison compareSessions(const FrameSession& before, const FrameSession& after);

// Plain-language lines: what was measured, the main stream's statistics, the GPU estimate, the cost.
QStringList summaryLines(const FrameSession& s);
// A self-contained HTML report (optionally with a comparison against `before`).
QString reportHtml(const FrameSession& s, const FrameSession* before = nullptr);

} // namespace gf::frames
