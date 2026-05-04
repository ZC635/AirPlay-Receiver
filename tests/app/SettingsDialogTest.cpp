#include <QtTest/QtTest>

#include <QKeySequenceEdit>
#include <QLabel>
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

    void exposesAcceptedSettings() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Shift+H"));
        dialog.accept();

        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
    }

    void rejectsDuplicateShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Alt+T"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("Duplicate shortcut"));
    }

    void rejectsUnsupportedShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+Alt+Left"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("Unsupported shortcut"));
    }

    void rejectsMultiStepShortcutsOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        QVERIFY(edit != nullptr);

        edit->setKeySequence(QKeySequence("Ctrl+K, Ctrl+C"));
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("single key combination"));
    }
};

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
