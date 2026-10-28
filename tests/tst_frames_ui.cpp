// The frame-time dialog without measuring (pressing Measure would raise the administrator prompt): its
// controls are named for screen readers, and earlier measurements of the same program are offered for
// comparison, newest first.
#include "frames/FrameSession.h"
#include "ui/widgets/FrameTimeDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>

using namespace gf;

namespace {
frames::FrameSession saved(const QString& id, const QString& process, const QString& label)
{
    frames::FrameSession s;
    s.id = id;
    s.label = label;
    s.created = QDateTime::fromString(id.mid(7, 15), QStringLiteral("yyyyMMdd-HHmmss"));
    s.pid = 7;
    s.processName = process;
    QJsonArray t;
    for (int i = 0; i < 200; ++i)
        t << i * 16.7;
    s.captureJson = QJsonObject{ { QStringLiteral("ok"), true },
                                 { QStringLiteral("processes"), QJsonArray{ QJsonObject{ { QStringLiteral("pid"), 7 }, { QStringLiteral("name"), process },
                                                                                        { QStringLiteral("streams"), QJsonArray{ QJsonObject{ { QStringLiteral("runtime"), QStringLiteral("DXGI") }, { QStringLiteral("t"), t } } } } } } } };
    s.capture = frames::Capture::fromJson(s.captureJson);
    return s;
}
} // namespace

class TestFramesUi : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void dialogOffersEarlierMeasurements()
    {
        QTemporaryDir dir;
        QString error;
        QVERIFY(frames::saveSession(dir.path(), saved(QStringLiteral("frames-20261028-120000-000"), QStringLiteral("game.exe"), QStringLiteral("before")), &error));
        QVERIFY(frames::saveSession(dir.path(), saved(QStringLiteral("frames-20261028-121000-000"), QStringLiteral("game.exe"), QStringLiteral("after")), &error));
        QVERIFY(frames::saveSession(dir.path(), saved(QStringLiteral("frames-20261028-122000-000"), QStringLiteral("other.exe"), QString()), &error));

        ui::FrameTimeDialog d(7, QStringLiteral("game.exe"), QStringLiteral("C:\\Games\\game.exe"), {}, dir.path(), dir.path(), dir.path());
        d.show();
        QVERIFY(QTest::qWaitForWindowExposed(&d));
        auto* compare = d.findChild<QComboBox*>();
        QVERIFY(compare);
        QCOMPARE(compare->count(), 3); // "nothing" + the two game.exe measurements, not other.exe
        QCOMPARE(compare->currentIndex(), 1);
        QVERIFY(compare->currentText().contains(QStringLiteral("after"))); // the newest is preselected
        for (QWidget* w : d.findChildren<QWidget*>()) {
            // Buttons are named by their text; parts of a spin box or combo box by their owner.
            if (!w->isVisible() || !w->focusPolicy() || qobject_cast<QPushButton*>(w) || qobject_cast<QAbstractSpinBox*>(w->parentWidget())
                || qobject_cast<QComboBox*>(w->parentWidget()))
                continue;
            QVERIFY2(!w->accessibleName().isEmpty(), w->metaObject()->className());
        }
        auto buttons = d.findChildren<QPushButton*>();
        auto find = [&](const QString& text) {
            return *std::find_if(buttons.begin(), buttons.end(), [&](QPushButton* b) { return b->text() == text; });
        };
        QVERIFY(find(QStringLiteral("Measure"))->isEnabled());
        QVERIFY(!find(QStringLiteral("Stop early"))->isEnabled());
        QVERIFY(!find(QStringLiteral("Open report"))->isEnabled());
    }
};

QTEST_MAIN(TestFramesUi)
#include "tst_frames_ui.moc"
