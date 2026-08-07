# Shortcut Status Column Visibility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Hide the hotkey table's `Status` column unless at least one shortcut apply failure is currently eligible for inline display.

**Architecture:** Retain the existing three-column `QTableWidget` and status-label widgets. `SettingsDialog::refreshFieldErrors()` remains the single projection point for field results and will also derive the column visibility from the same failure filtering rules, while construction establishes the initially hidden state.

**Tech Stack:** C++20, Qt 6 Widgets, Qt Test, CMake/CTest

---

### Task 1: Make shortcut status visibility follow actionable failures

**Files:**
- Modify: `tests/app/SettingsDialogTest.cpp`
- Modify: `src/app/SettingsDialog.cpp`

- [ ] **Step 1: Write the failing UI tests**

Update `shortcutErrorsAppearInStatusColumn()` to verify the normal state is hidden and a shortcut failure reveals the column:

```cpp
void shortcutErrorsAppearInStatusColumn() {
    SettingsDialog dialog(AppSettings::defaults());
    auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
    QVERIFY(table != nullptr);
    QCOMPARE(table->columnCount(), 3);
    QCOMPARE(table->horizontalHeaderItem(2)->text(), QString("Status"));
    QVERIFY(table->isColumnHidden(2));

    SettingsApplyOutcome outcome;
    outcome.committedSettings = AppSettings::defaults();
    outcome.fieldResults = {{SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                             QKeySequence("Ctrl+Shift+T"),
                             SettingsFieldStatus::ApplyFailedRolledBack,
                             "already registered"}};
    dialog.presentApplyOutcome(outcome);

    QVERIFY(!table->isColumnHidden(2));
    auto *status = qobject_cast<QLabel *>(table->cellWidget(3, 2));
    QVERIFY(status != nullptr);
    QVERIFY(status->wordWrap());
    QVERIFY(status->text().contains("Toggle toolbar"));
    QVERIFY(status->text().contains("Ctrl+Shift+T"));
}
```

Add a focused test proving a non-shortcut failure does not reveal the column and editing the last failed shortcut hides it again:

```cpp
void shortcutStatusColumnHidesWhenNoShortcutFailureRemains() {
    SettingsDialog dialog(AppSettings::defaults());
    auto *table = dialog.findChild<QTableWidget *>("shortcutTable");
    auto *shortcut = dialog.findChild<QKeySequenceEdit *>("shortcutEdit_toggleToolbar");
    QVERIFY(table != nullptr);
    QVERIFY(shortcut != nullptr);

    SettingsApplyOutcome receiverFailure;
    receiverFailure.committedSettings = AppSettings::defaults();
    receiverFailure.fieldResults = {
        {SettingsFieldId::receiverName(), QString("Invalid"),
         SettingsFieldStatus::ValidationFailed, "not allowed"},
        {SettingsFieldId::shortcut(ShortcutAction::VolumeUp), QKeySequence("Ctrl+Up"),
         SettingsFieldStatus::Applied, {}},
        {SettingsFieldId::shortcut(ShortcutAction::VolumeDown), QKeySequence("Ctrl+Down"),
         SettingsFieldStatus::Deferred, {}},
    };
    dialog.presentApplyOutcome(receiverFailure);
    QVERIFY(table->isColumnHidden(2));

    SettingsApplyOutcome shortcutFailure;
    shortcutFailure.committedSettings = AppSettings::defaults();
    shortcutFailure.fieldResults = {{SettingsFieldId::shortcut(ShortcutAction::ToggleToolbar),
                                     QKeySequence("Ctrl+Shift+T"),
                                     SettingsFieldStatus::ApplyFailedRolledBack,
                                     "already registered"}};
    dialog.presentApplyOutcome(shortcutFailure);
    QVERIFY(!table->isColumnHidden(2));

    shortcut->setKeySequence(QKeySequence("Ctrl+Shift+Y"));
    QVERIFY(table->isColumnHidden(2));
}
```

- [ ] **Step 2: Build and run the focused test to verify RED**

Run:

```powershell
cmake --build build-uxplay --target SettingsDialogTest
ctest --test-dir build-uxplay -R "^SettingsDialogTest$" --output-on-failure
```

Expected: `SettingsDialogTest` fails because column 2 is visible immediately after construction.

- [ ] **Step 3: Implement the minimal column visibility projection**

After configuring the table headers in the `SettingsDialog` constructor, establish the normal hidden state:

```cpp
table_->setHorizontalHeaderLabels({"Action", "Shortcut", "Status"});
table_->setColumnHidden(2, true);
```

In `SettingsDialog::refreshFieldErrors()`, track only shortcut failures that pass the existing inline-display filter, and update the column once after iterating:

```cpp
void SettingsDialog::refreshFieldErrors() {
    receiverNameError_->hide();
    videoResolutionError_->hide();
    videoFrameRateError_->hide();
    for (QLabel *label : shortcutErrorLabels_) {
        label->clear();
        label->hide();
    }
    bool showShortcutStatus = false;
    for (const SettingsFieldResult &result : fieldResults_) {
        if (!isFailureStatus(result.status)
            || (globalResult_.has_value() && result.status != SettingsFieldStatus::RecoveryFailed)) {
            continue;
        }
        QLabel *label = nullptr;
        switch (result.field.kind) {
        case SettingsFieldKind::ReceiverName: label = receiverNameError_; break;
        case SettingsFieldKind::VideoResolution: label = videoResolutionError_; break;
        case SettingsFieldKind::VideoFrameRate: label = videoFrameRateError_; break;
        case SettingsFieldKind::Shortcut:
            if (result.field.shortcutAction.has_value()) {
                label = shortcutErrorLabels_.value(static_cast<int>(*result.field.shortcutAction), nullptr);
                showShortcutStatus = showShortcutStatus || label != nullptr;
            }
            break;
        default: break;
        }
        if (label != nullptr) {
            label->setText(fieldFailureMessage(result));
            label->show();
        }
    }
    table_->setColumnHidden(2, !showShortcutStatus);
}
```

- [ ] **Step 4: Run focused and related tests to verify GREEN**

Run:

```powershell
cmake --build build-uxplay --target SettingsDialogTest MainWindowSmokeTest
ctest --test-dir build-uxplay -R "^(SettingsDialogTest|MainWindowSmokeTest)$" --output-on-failure
```

Expected: both tests pass with no failures.

- [ ] **Step 5: Verify formatting and commit the behavior change**

Run:

```powershell
git diff --check
git status --short
git add src/app/SettingsDialog.cpp tests/app/SettingsDialogTest.cpp
git commit -m "fix: hide empty shortcut status column"
```

Expected: `git diff --check` produces no output; the commit contains only the dialog implementation and its UI tests.
