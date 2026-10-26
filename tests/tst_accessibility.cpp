// Accessibility of the real main window: on every page, every visible interactive control (buttons,
// check boxes, text fields, lists, tables, combo and spin boxes, the guide viewer) has an accessible
// name — what a screen reader announces — and can be reached with the keyboard (Tab).
#include "benchmarks/BenchmarkService.h"
#include "benchmarks/LabRunner.h"
#include "core/AppPaths.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "preferences/PreferenceService.h"
#include "profiles/ProfileStore.h"
#include "ui/AppContext.h"
#include "ui/MainWindow.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QAccessible>
#include <QComboBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

using namespace gf;

namespace {

bool interactive(const QWidget* w)
{
    return qobject_cast<const QAbstractButton*>(w) || qobject_cast<const QLineEdit*>(w) || qobject_cast<const QComboBox*>(w) ||
           qobject_cast<const QAbstractSpinBox*>(w) || qobject_cast<const QAbstractItemView*>(w) || qobject_cast<const QTextEdit*>(w);
}

QString describe(const QWidget* w)
{
    QString d = QString::fromLatin1(w->metaObject()->className());
    if (const auto* b = qobject_cast<const QAbstractButton*>(w); b && !b->text().isEmpty())
        d += QStringLiteral(" \"%1\"").arg(b->text());
    if (!w->toolTip().isEmpty())
        d += QStringLiteral(" (tooltip: %1)").arg(w->toolTip().left(50));
    if (const auto* e = qobject_cast<const QLineEdit*>(w); e && !e->placeholderText().isEmpty())
        d += QStringLiteral(" (placeholder: %1)").arg(e->placeholderText().left(50));
    return d;
}

} // namespace

class TestAccessibility : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void everyControlHasANameAndKeyboardAccess()
    {
        QTemporaryDir dir;
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        hw::HardwareService hardware;
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), QString());
        bench::BenchmarkService benchmarks(data.benchmarksDir());
        bench::BenchmarkService lab(bench::labDir(data.benchmarksDir()));
        profiles::ProfileStore store(QDir(data.configDir()).filePath(QStringLiteral("profiles")));
        // One profile, so the GPU Manager's editor (mode buttons, arguments, actions) is populated.
        profiles::AppProfile p = profiles::newProfile(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionProbe.exe")));
        QString error;
        QVERIFY(store.save(p, &error));
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs, &benchmarks, &lab, &store);
        ui::MainWindow window(context);
        window.resize(1200, 860);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QStringList problems;
        int checked = 0;
        auto* stack = window.findChild<QStackedWidget*>();
        QVERIFY(stack);
        for (const QString& id : ui::MainWindow::pageIds()) {
            QVERIFY(window.showPage(id));
            QTest::qWait(150);
            QWidget* page = stack->currentWidget();
            for (QWidget* w : page->findChildren<QWidget*>()) {
                if (!interactive(w) || !w->isVisibleTo(&window))
                    continue;
                // The internal line edit of a combo/spin box and a table's header are covered by their owner
                // (screen readers reach headers through the table; headers do not take Tab focus).
                if (qobject_cast<QAbstractSpinBox*>(w->parentWidget()) || qobject_cast<QComboBox*>(w->parentWidget()) ||
                    qobject_cast<QHeaderView*>(w))
                    continue;
                ++checked;
                QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(w);
                const QString name = iface ? iface->text(QAccessible::Name).trimmed() : QString();
                if (name.isEmpty())
                    problems << QStringLiteral("%1: no accessible name: %2").arg(id, describe(w));
                if (!(w->focusPolicy() & Qt::TabFocus))
                    problems << QStringLiteral("%1: not reachable with Tab: %2").arg(id, describe(w));
            }
        }
        qInfo().noquote() << checked << "controls checked on" << ui::MainWindow::pageIds().size() << "pages";

        // Keyboard only: the benchmarks to run are chosen with the arrow keys and Space.
        QVERIFY(window.showPage(QStringLiteral("benchmarks")));
        QTest::qWait(150);
        QTreeWidget* tests = nullptr;
        for (QTreeWidget* t : window.findChildren<QTreeWidget*>())
            if (t->accessibleName() == QStringLiteral("Benchmarks to run"))
                tests = t;
        QVERIFY(tests);
        QTreeWidgetItem* first = nullptr;
        for (QTreeWidgetItemIterator it(tests); *it && !first; ++it)
            if ((*it)->flags() & Qt::ItemIsUserCheckable)
                first = *it;
        QVERIFY2(first, "no checkable benchmark in the list");
        {
            tests->setFocus();
            tests->setCurrentItem(first);
            const Qt::CheckState before = first->checkState(0);
            QTest::keyClick(tests, Qt::Key_Space);
            QVERIFY2(first->checkState(0) != before, "Space did not toggle the benchmark");
            QTest::keyClick(tests, Qt::Key_Space);
            QCOMPARE(first->checkState(0), before);
        }
        for (const QString& problem : problems)
            qInfo().noquote() << "  " << problem;
        QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("%1 accessibility problem(s), listed above").arg(problems.size())));
    }
};

QTEST_MAIN(TestAccessibility)
#include "tst_accessibility.moc"
