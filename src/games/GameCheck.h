#pragma once

// Game check: what happens on the PC while a game (any program that renders) runs. Works with any
// program; nothing is game-specific, nothing is started, closed or changed.
//
// For N seconds, once a second, it samples (estimates, from Windows' counters):
//   - every process's GPU engine use on every GPU (Windows GPU performance counters; D3DKMT for
//     WDDM 1.x adapters such as the HD 2000), busiest engine per process and adapter;
//   - the load of each CPU core, and the game process's own CPU time;
// optionally with the game's frame times measured by GPUFusionFrameCapture (see frames/).
//
// The analysis lists the programs competing for the game's GPU, what each could do about it (from the
// compatibility registry: can it be moved to the other GPU, and how), and indications of a bottleneck.
// Indications are labelled as such: utilization alone does not prove a bottleneck.

#include "compat/Compatibility.h"
#include "frames/FrameSession.h"
#include "monitoring/MetricModel.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <atomic>
#include <functional>
#include <optional>

namespace gf::games {

// One process's use of one adapter during the check (busiest engine per sample; percent of that engine).
struct ProcessGpu {
    quint32 pid = 0;
    QString name, path;
    quint64 luid = 0;
    QString adapter;
    int samples = 0;            // samples in which the process had a reading on this adapter
    double meanPercent = 0;     // mean over all samples of the check (missing = 0)
    double maxPercent = 0;
    QString busiestEngine;      // most often busiest ("3D", "VideoDecode", "Engine 0", ...)
};

struct CpuStats {
    int cores = 0;
    double meanTotalPercent = 0;     // all cores
    double meanBusiestCorePercent = 0;
    double maxBusiestCorePercent = 0;
    double gameCpuPercentOfOneCore = -1; // the game's own CPU time per second, as % of one core (-1 = unreadable)
};

// One sample, as the runner collects it (also the unit-test input).
struct Sample {
    QList<ProcessGpu> gpu;          // per process and adapter: meanPercent = this sample's value
    QList<double> corePercent;      // per core
    double gameCpuPercentOfOneCore = -1;
};

enum class Indication { GpuBound, Contention, CpuBound, NoneClear };
QString indicationText(Indication i);

struct Competitor {
    ProcessGpu use;
    compat::Answer move;            // MoveOffGameGpu for its executable
    QString mechanism;              // how it could be moved, if it can
    bool movable() const { return move.status == compat::Status::Supported || move.status == compat::Status::Experimental; }
};

struct GameCheck {
    QString id;                     // "game-20261028-..."
    QDateTime created;
    QString label;
    quint32 pid = 0;
    QString name, path;
    int seconds = 0, samples = 0;
    QString gameAdapter;            // the GPU the game worked on most
    quint64 gameLuid = 0;
    double gameGpuMean = 0, gameGpuMax = 0;
    double othersOnGameGpuMean = 0; // sum of the other processes' mean busiest-engine use on the game's GPU
    QList<ProcessGpu> processes;    // every process with GPU use, all adapters, busiest first
    QList<Competitor> competitors;  // other processes on the game's GPU, busiest first
    CpuStats cpu;
    QList<std::pair<Indication, QString>> indications;
    std::optional<frames::FrameSession> frames; // if frame times were measured
    QStringList notes;              // what could not be read, etc.
    double checkCpuPercent = -1;    // GPU Fusion's own CPU use during the check
    QString error;

    QJsonObject toJson() const;
    static std::optional<GameCheck> fromJson(const QJsonObject& o);
};

// Pure: builds the check's results from samples (the game is `pid`). `assessExe` returns the
// compatibility assessment of an executable (cached by the caller).
GameCheck analyze(quint32 pid, const QString& name, const QString& path, const QList<Sample>& samples,
                  const std::function<compat::Assessment(const QString& exePath)>& assessExe);

struct CheckOptions {
    quint32 pid = 0;
    QString name, path;
    int seconds = 15;
    QString label;
    bool measureFrames = false;     // also GPUFusionFrameCapture (administrator prompt)
    QString helperPath, workDir;    // for the frame capture
};
using CheckProgress = std::function<void(const QString& status, int percent)>;

// Runs a check (synchronous: call from a worker thread).
GameCheck runCheck(const QList<mon::MonitorTarget>& targets, const CheckOptions& options,
                   const std::function<compat::Assessment(const QString& exePath)>& assessExe, const std::atomic<bool>* cancel = nullptr,
                   const CheckProgress& progress = {});

// Processes using a GPU right now: one sample over about `ms` milliseconds (estimates; for picking a
// game). Busiest first; one entry per process (its most used adapter).
QList<ProcessGpu> sampleGpuUse(const QList<mon::MonitorTarget>& targets, int ms = 1000);

// Before/after: the game's frame times if both checks measured them (the frames verdict), and the
// competitors' GPU use on the game's GPU.
QStringList compareChecks(const GameCheck& before, const GameCheck& after);

QStringList summaryLines(const GameCheck& c);
QString reportHtml(const GameCheck& c, const GameCheck* before = nullptr);

bool saveCheck(const QString& dir, const GameCheck& c, QString* error);
QList<GameCheck> loadChecks(const QString& dir); // newest first

} // namespace gf::games
