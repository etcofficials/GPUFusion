#pragma once

// Runs one frame-time measurement of a running program: starts GPUFusionFrameCapture (through the
// Windows administrator prompt when GPU Fusion itself is not elevated — the user approves or declines
// each time; no setting or group membership is changed), and while it runs samples the program's GPU
// engine use with GPU Fusion's normal monitoring (an estimate, kept apart from the measured frame
// times). Synchronous: call from a worker thread.

#include "frames/FrameSession.h"
#include "monitoring/MetricModel.h"

#include <atomic>
#include <functional>

namespace gf::frames {

struct MeasureOptions {
    quint32 pid = 0;
    QString processName, processPath; // for the record; read from the process if empty
    int seconds = 20;
    QString label;
    QString helperPath;   // GPUFusionFrameCapture.exe
    QString workDir;      // where the helper writes its result (GPU Fusion's data folder)
};

using MeasureProgress = std::function<void(const QString& status, int percent)>;

// True if this process runs with administrator rights (the capture then needs no prompt).
bool isElevated();

FrameSession measureFrames(const QList<mon::MonitorTarget>& targets, const MeasureOptions& options, const std::atomic<bool>* cancel = nullptr,
                           const MeasureProgress& progress = {});

} // namespace gf::frames
