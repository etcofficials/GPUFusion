#pragma once

// Frame times of running programs, measured from outside by GPUFusionFrameCapture (Windows graphics
// event tracing; nothing is loaded into the measured program). This file holds the capture's data, as
// the helper writes it, and the statistics computed from it. Pure: unit-testable without tracing.
//
// What is measured: the time at which a program called Present (DXGI: Direct3D 10/11/12; Direct3D 9)
// or presented through the kernel (OpenGL, Vulkan and other APIs). Frame time = time between
// consecutive presents of one swap chain. Not measured: when frames reached the display, input
// latency, GPU time per frame.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

namespace gf::frames {

// One sequence of presents: a DXGI or D3D9 swap chain, or a process's kernel-level presents.
struct FrameStream {
    QString runtime;            // "DXGI", "D3D9" or "Kernel"
    QString swapChain;          // "0x…" for DXGI/D3D9; "0x0" for kernel presents
    std::optional<int> syncInterval; // DXGI only
    quint32 presentFlags = 0;
    int occluded = 0;           // DXGI presents of a hidden window (counted as frames, not shown)
    int discarded = 0;          // calls that presented nothing (DXGI_PRESENT_TEST, "still drawing")
    QList<double> t;            // present times, ms since the capture started
    QList<double> inPresentMs;  // time inside the present call (DXGI/D3D9); empty for kernel presents
};

struct CapturedProcess {
    quint32 pid = 0;
    QString name, path;
    QList<FrameStream> streams;
    int frames() const;
};

struct Capture {
    bool ok = false;
    QString error;
    qint64 errorCode = 0;
    bool accessDenied() const { return errorCode == 5; }
    bool elevated = false;
    QDateTime started;
    double requestedSeconds = 0, wallMs = 0;
    QString endReason;
    QList<quint32> targets;
    QStringList providerProblems;   // providers that could not be enabled
    quint64 events = 0, eventsLost = 0, buffersLost = 0, framesNotStored = 0;
    double helperCpuMs = 0, helperCpuPercent = 0, helperPeakWorkingSetMB = 0;
    QList<CapturedProcess> processes;

    static Capture fromJson(const QJsonObject& o);
    const CapturedProcess* process(quint32 pid) const;
};

// Which graphics API a stream's presents came through, in words.
QString runtimeText(const QString& runtime);

// The stream that represents the program's frames: a DXGI/D3D9 swap chain if there is one (kernel
// presents of such a process come from its driver, not extra frames), else the kernel presents; the
// one with the most presents.
const FrameStream* mainStream(const CapturedProcess& p);

struct FrameStats {
    int presents = 0;           // presents in the stream
    int frames = 0;             // frame times (presents - 1)
    double spanMs = 0;          // first to last present
    double fps = 0;             // frames / span
    double meanMs = 0, medianMs = 0, p95Ms = 0, p99Ms = 0, maxMs = 0, sdMs = 0;
    double low1Fps = 0;         // 1000 / mean of the slowest 1 % of frame times ("1 % low")
    int longFrames = 0;         // frame times > 2 × median and > median + 4 ms (stutters)
    QList<double> perSecondMeanMs; // mean frame time in each whole second: the samples for comparisons
};
FrameStats computeStats(const QList<double>& presentTimes);

} // namespace gf::frames
