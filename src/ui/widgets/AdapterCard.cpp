#include "ui/widgets/AdapterCard.h"

#include "ui/Theme.h"

#include <QGridLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace gf::ui {

using namespace gf::hw;

namespace {

QString hex(quint32 v, int width) { return QStringLiteral("0x%1").arg(QString::number(v, 16).rightJustified(width, u'0').toUpper()); }
QString yesNo(bool v) { return v ? QObject::tr("Yes") : QObject::tr("No"); }

QString className(AdapterClass c)
{
    switch (c) {
    case AdapterClass::Discrete:   return QObject::tr("Discrete");
    case AdapterClass::Integrated: return QObject::tr("Integrated");
    case AdapterClass::Software:   return QObject::tr("Software");
    case AdapterClass::Unknown:    return QObject::tr("Type unknown");
    }
    return {};
}

QString probeText(const ApiProbe& p, const QString& okText)
{
    switch (p.state) {
    case ApiProbe::State::NotAttempted: return QObject::tr("Not tested");
    case ApiProbe::State::Succeeded:    return okText;
    case ApiProbe::State::Failed:       return QObject::tr("Not available — %1").arg(p.error);
    }
    return {};
}

QWidget* findingsWidget(const QList<Finding>& findings)
{
    auto* box = new QWidget;
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(8);
    const ThemeColors& c = colors();
    for (const Finding& f : findings) {
        const QColor color = f.severity == Finding::Severity::Error ? c.bad : f.severity == Finding::Severity::Warning ? c.warn : c.accent;
        const QString kind = f.severity == Finding::Severity::Error ? QObject::tr("Error")
                           : f.severity == Finding::Severity::Warning ? QObject::tr("Warning") : QObject::tr("Note");
        auto* label = new QLabel(QStringLiteral("<span style='color:%1'>&#9679;</span> <b>%2</b><br><span style='color:%3'>%4</span>")
                                     .arg(color.name(), f.title.toHtmlEscaped(), c.textMuted.name(), f.detail.toHtmlEscaped()));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setAccessibleName(QStringLiteral("%1: %2. %3").arg(kind, f.title, f.detail));
        layout->addWidget(label);
    }
    return box;
}

} // namespace

QString AdapterCard::statusText(const Adapter& a, Tone* tone, QString* explanation)
{
    auto set = [&](Tone t, QString text, QString why) {
        *tone = t;
        *explanation = std::move(why);
        return text;
    };
    if (a.isSoftware())
        return set(Tone::Neutral, tr("Software renderer"), tr("Runs on the CPU; not a physical GPU."));
    if (a.pnp && a.pnp->hasProblem)
        return set(Tone::Bad, tr("Problem"), describeProblemCode(a.pnp->problemCode));
    if (!a.dxgi) {
        if (a.pnp && !a.pnp->present)
            return set(Tone::Warning, tr("Not detected"), tr("Installed earlier but not currently on the bus."));
        return set(Tone::Warning, tr("Not available to DirectX"), tr("Listed in Device Manager but not enumerated by DXGI."));
    }
    if (a.dxgi->d3d11.device.state == ApiProbe::State::Failed)
        return set(Tone::Bad, tr("Device creation failed"), a.dxgi->d3d11.device.error);
    if (a.dxgi->d3d11.device.ok())
        return set(Tone::Good, tr("Usable"), tr("A Direct3D 11 device was created on this adapter. This shows it can run work, not that any application is using it."));
    return set(Tone::Neutral, tr("Detected"), tr("Enumerated by DXGI; no device was created."));
}

AdapterCard::AdapterCard(const Adapter& a, QWidget* parent) : Card(a.name(), parent)
{
    Tone tone = Tone::Neutral;
    QString why;
    const QString status = statusText(a, &tone, &why);
    addBadge(status, tone, why);
    if (!a.isSoftware()) // the status badge already says "Software renderer"
        addBadge(className(a.classification.value), a.classification.value == AdapterClass::Discrete ? Tone::Accent : Tone::Neutral,
                 a.classification.reason);

    const PciIds ids = a.pciIds();
    const std::optional<PciLocation> location = a.dxgi && a.dxgi->location ? a.dxgi->location
                                              : a.pnp ? a.pnp->location : std::nullopt;

    // Identity
    QList<QPair<QString, QString>> identity;
    if (ids.valid) {
        identity << qMakePair(tr("Vendor"), QStringLiteral("%1 (%2)").arg(vendorName(ids.vendor), hex(ids.vendor, 4)));
        identity << qMakePair(tr("Device ID"), hex(ids.device, 4));
        identity << qMakePair(tr("Subsystem / revision"), QStringLiteral("%1 / %2").arg(hex(ids.subsystem, 8), hex(ids.revision, 2)));
    } else if (a.dxgi) {
        identity << qMakePair(tr("Vendor"), QStringLiteral("%1 (%2)").arg(vendorName(a.dxgi->vendorId), hex(a.dxgi->vendorId, 4)));
        identity << qMakePair(tr("Device ID"), hex(a.dxgi->deviceId, 4));
    }
    if (location)
        identity << qMakePair(tr("PCI location"), location->toString());
    if (a.dxgi)
        identity << qMakePair(tr("DXGI adapter"), tr("#%1, LUID %2").arg(a.dxgi->index).arg(formatLuid(a.dxgi->luid)));
    m_sectionWidgets << section(tr("Identity"), identity);

    // Driver and status
    if (!a.isSoftware()) {
        QList<QPair<QString, QString>> driver;
        if (a.pnp) {
            driver << qMakePair(tr("Device Manager"), a.pnp->present ? (a.pnp->hasProblem ? tr("Problem code %1").arg(a.pnp->problemCode) : tr("Working (no problem reported)"))
                                                                     : tr("Not present"));
            if (!a.pnp->driverVersion.isEmpty())
                driver << qMakePair(tr("Version"), a.pnp->driverVersion);
            if (a.pnp->driverDate.isValid())
                driver << qMakePair(tr("Date"), QLocale().toString(a.pnp->driverDate, QLocale::ShortFormat));
            if (!a.pnp->driverProvider.isEmpty())
                driver << qMakePair(tr("Provider"), a.pnp->driverProvider);
            if (!a.pnp->infName.isEmpty())
                driver << qMakePair(tr("INF"), a.pnp->infName);
        }
        if (a.dxgi) {
            if (!a.dxgi->umdVersion.isEmpty() && (!a.pnp || a.pnp->driverVersion != a.dxgi->umdVersion))
                driver << qMakePair(tr("User-mode driver"), a.dxgi->umdVersion);
            driver << qMakePair(tr("Driver model"), a.dxgi->wddmVersion > 0 ? tr("WDDM %1").arg(formatWddm(a.dxgi->wddmVersion)) : tr("Unknown"));
        }
        m_sectionWidgets << section(tr("Driver"), driver);
    }

    if (a.dxgi) {
        const DxgiAdapter& d = *a.dxgi;
        m_sectionWidgets << section(tr("Memory reported by DXGI"), {
            qMakePair(tr("Dedicated video"), formatBytes(d.dedicatedVideoMemory)),
            qMakePair(tr("Dedicated system"), formatBytes(d.dedicatedSystemMemory)),
            qMakePair(tr("Shared system (limit)"), formatBytes(d.sharedSystemMemory)),
        });

        if (!a.isSoftware()) {
            QList<QPair<QString, QString>> d3d;
            d3d << qMakePair(tr("Direct3D 11"), probeText(d.d3d11.device, tr("Feature level %1").arg(d.d3d11.device.featureLevel)));
            if (d.d3d11.device.ok()) {
                d3d << qMakePair(tr("Compute shaders"), yesNo(d.d3d11.computeShaders));
                d3d << qMakePair(tr("Double precision"), yesNo(d.d3d11.doublePrecision));
            }
            d3d << qMakePair(tr("Direct3D 12"), probeText(d.d3d12.device, tr("Feature level %1").arg(d.d3d12.maxFeatureLevel)));
            if (d.d3d12.uma)
                d3d << qMakePair(tr("Memory architecture"), *d.d3d12.uma ? tr("Unified (shares system RAM)") : tr("Separate video memory"));
            if (d.capsReused)
                d3d << qMakePair(tr("Probe"), tr("From the previous detection (driver unchanged). Refresh re-tests."));
            m_sectionWidgets << section(tr("Direct3D"), d3d);

            QList<QPair<QString, QString>> video;
            if (!d.d3d11.videoDevice) {
                video << qMakePair(tr("Hardware decoding"), d.d3d11.device.ok() ? tr("Not available (no D3D11 video device)") : tr("Unknown"));
            } else {
                const QStringList codecs = decoderCodecs(d.d3d11.decoderProfiles);
                QStringList named;
                int vendorSpecific = 0;
                for (const QString& g : d.d3d11.decoderProfiles) {
                    const QString n = decoderProfileName(g);
                    if (n.isEmpty())
                        ++vendorSpecific;
                    else
                        named << n;
                }
                video << qMakePair(tr("Codecs"), codecs.isEmpty() ? tr("None reported") : codecs.join(QStringLiteral(", ")));
                video << qMakePair(tr("Decoder profiles"), tr("%1 (%2 unidentified vendor-specific)").arg(d.d3d11.decoderProfiles.size()).arg(vendorSpecific));
                if (!named.isEmpty())
                    video << qMakePair(tr("Profiles"), named.join(QStringLiteral(", ")));
            }
            m_sectionWidgets << section(tr("Video decoding (DXVA)"), video);

            QList<QPair<QString, QString>> outputs;
            for (const DisplayOutput& o : d.outputs) {
                outputs << qMakePair(o.deviceName, QStringLiteral("%1%2 × %3%4").arg(o.monitor.isEmpty() ? QString() : o.monitor + QStringLiteral(", "))
                                                       .arg(o.width).arg(o.height).arg(o.attachedToDesktop ? QString() : tr(" (not on desktop)")));
            }
            if (outputs.isEmpty())
                outputs << qMakePair(tr("Displays"), tr("None connected"));
            m_sectionWidgets << section(tr("Outputs"), outputs);
        }
    }

    m_sections = new QGridLayout;
    m_sections->setHorizontalSpacing(28);
    m_sections->setVerticalSpacing(14);
    body()->addLayout(m_sections);
    reflow();

    // Full-width footer: long unbreakable identifiers would otherwise force one column wide.
    QString footer = tr("Classified as %1: %2. Device Manager match: %3.")
                         .arg(className(a.classification.value).toLower(), a.classification.reason, a.matchMethod);
    if (a.pnp)
        footer += QStringLiteral("\n") + tr("Device instance: %1").arg(a.pnp->instanceId);
    auto* classification = new QLabel(footer);
    classification->setObjectName(QStringLiteral("muted"));
    classification->setWordWrap(true);
    classification->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body()->addWidget(classification);

    if (!a.findings.isEmpty())
        body()->addWidget(findingsWidget(a.findings));
}

QWidget* AdapterCard::section(const QString& title, const QList<QPair<QString, QString>>& rows)
{
    auto* box = new QWidget;
    // Width comes only from the grid's equal column stretch, not from content length.
    box->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    auto* heading = new QLabel(title);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
    auto* grid = new InfoGrid;
    for (const auto& [label, value] : rows)
        grid->addRow(label, value);
    layout->addWidget(grid);
    layout->addStretch();
    return box;
}

void AdapterCard::reflow()
{
    const int columns = width() >= 820 ? 2 : 1;
    if (columns == m_columns)
        return;
    m_columns = columns;
    for (QWidget* w : m_sectionWidgets)
        m_sections->removeWidget(w);
    for (int i = 0; i < m_sectionWidgets.size(); ++i)
        m_sections->addWidget(m_sectionWidgets[i], i / columns, i % columns);
    for (int c = 0; c < 2; ++c)
        m_sections->setColumnStretch(c, c < columns ? 1 : 0);
}

void AdapterCard::resizeEvent(QResizeEvent* event)
{
    Card::resizeEvent(event);
    reflow();
}

} // namespace gf::ui
