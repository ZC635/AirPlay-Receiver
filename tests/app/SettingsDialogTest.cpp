#include <QtTest/QtTest>

#include <QGroupBox>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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

    void showsGeneralAndHotkeyBindingSections() {
        SettingsDialog dialog(AppSettings::defaults());

        QVERIFY(dialog.findChild<QGroupBox *>("generalSettingsGroup") != nullptr);
        QVERIFY(dialog.findChild<QGroupBox *>("hotkeyBindingGroup") != nullptr);
        QVERIFY(dialog.findChild<QLineEdit *>("receiverNameEdit") != nullptr);
        QVERIFY(dialog.findChild<QPushButton *>("resetHotkeysButton") != nullptr);
    }

    void exposesAcceptedReceiverName() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(edit != nullptr);

        edit->setText("Desk Receiver");
        dialog.accept();

        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QCOMPARE(dialog.settings().receiverName(), QString("Desk Receiver"));
    }

    void rejectsEmptyReceiverNameOnAccept() {
        SettingsDialog dialog(AppSettings::defaults());
        auto *edit = dialog.findChild<QLineEdit *>("receiverNameEdit");
        QVERIFY(edit != nullptr);

        edit->setText("   ");
        dialog.accept();

        QVERIFY(dialog.result() != static_cast<int>(QDialog::Accepted));
        auto *error = dialog.findChild<QLabel *>("settingsErrorLabel");
        QVERIFY(error != nullptr);
        QVERIFY(error->text().contains("Receiver name"));
    }

    void resetHotkeysRestoresDefaultEditsWithoutSaving() {
        AppSettings settings = AppSettings::defaults();
        settings.setShortcut(ShortcutAction::ToggleToolbar, QKeySequence("Ctrl+Shift+H"));
        SettingsDialog dialog(settings);

        auto *edit = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
        auto *button = dialog.findChild<QPushButton *>("resetHotkeysButton");
        QVERIFY(edit != nullptr);
        QVERIFY(button != nullptr);

        QCOMPARE(edit->keySequence(), QKeySequence("Ctrl+Shift+H"));
        button->click();

        QCOMPARE(edit->keySequence(), AppSettings::defaults().shortcutFor(ShortcutAction::ToggleToolbar));
        QCOMPARE(dialog.settings().shortcutFor(ShortcutAction::ToggleToolbar), QKeySequence("Ctrl+Shift+H"));
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
