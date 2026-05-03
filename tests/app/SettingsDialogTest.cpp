#include <QtTest/QtTest>

#include <QTableWidget>

#include "app/SettingsDialog.h"

class SettingsDialogTest : public QObject {
    Q_OBJECT

private slots:
    void listsAllShortcutActions() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
        QVERIFY(table);
        QCOMPARE(table->rowCount(), 4);
    }
};

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
