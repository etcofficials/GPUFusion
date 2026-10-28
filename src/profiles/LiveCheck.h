#pragma once

// Passive check of a running program against its GPU profile(s): which GPU it is using right now
// (from the live per-process monitoring) compared with the GPU its profile asks for — without
// launching anything. Pure; used by the Applications page.

#include "monitoring/ProcessModel.h"
#include "profiles/AppProfile.h"

#include <QHash>
#include <QString>

namespace gf::profiles {

struct LiveCheck {
    enum class State {
        NoProfile,      // the program has no GPU profile
        Automatic,      // its profile(s) do not ask for a GPU
        NoGpuWork,      // no engine activity right now, so nothing to compare
        Matches,        // working on the GPU a profile asks for
        BothActive,     // a "Both GPUs" profile, and the program's own work is on two or more GPUs right now
        OneOfBoth,      // a "Both GPUs" profile, but the program works on one GPU only (it does not use the SDK,
                        // or not at the moment): both were offered, the program did not split its work
        Elsewhere       // working on another GPU (or the software renderer)
    };
    State state = State::NoProfile;
    QString text;       // short, for a table column
    QString detail;     // one or two sentences for the details panel
};

// `profiles`: the program's main profile and variants (may be empty). `usage`: this process's current
// per-adapter usage. `adapterNames`: luid -> name of every adapter the monitoring knows (software
// renderers included).
LiveCheck liveCheck(const QList<AppProfile>& profiles, const QList<mon::ProcessAdapterUsage>& usage,
                    const QList<pref::SelectableGpu>& gpus, const QHash<quint64, QString>& adapterNames);

} // namespace gf::profiles
