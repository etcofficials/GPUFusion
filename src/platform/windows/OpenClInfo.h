#pragma once

#include "diagnostics/DiagnosticReport.h"

namespace gf::diag::win {

// Read-only: is the OpenCL loader installed, and which OpenCL drivers (ICDs) are registered?
// Nothing is loaded or initialised.
OpenClInfo readOpenClInfo();

} // namespace gf::diag::win
