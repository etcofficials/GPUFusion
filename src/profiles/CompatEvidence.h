#pragma once

// What GPU Fusion has verified for a program, from its GPU profiles, for the compatibility classifier.

#include "compat/Compatibility.h"
#include "profiles/AppProfile.h"

namespace gf::profiles {

// A confirmed verification in Integrated mode means the program rendered on the integrated GPU when
// GPU Fusion started it that way; in Discrete mode, on the discrete GPU.
compat::ProgramEvidence evidenceFor(const QString& exePath, const QList<AppProfile>& profiles);

} // namespace gf::profiles
