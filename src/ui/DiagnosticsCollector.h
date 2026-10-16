#pragma once

#include "diagnostics/DiagnosticReport.h"

namespace gf::ui {

class AppContext;

// Gathers the current state from the services into report inputs (no probing).
diag::ReportInputs collectReportInputs(AppContext& context);

// Writes <basePath>.md and <basePath>.json, both passed through the privacy redactor.
// Returns an empty string on success, otherwise the error.
QString exportReport(const diag::Report& report, const QString& basePath, QStringList* written);

} // namespace gf::ui
