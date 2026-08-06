# Partial Settings Apply and Field-Specific Errors

Status: awaiting final written-spec review
Scope: the modal Settings dialog and the runtime services it invokes

## Summary

Change Settings Apply from one all-or-nothing `AppSettings` transaction into a coordinated, field-aware operation. A failure in one setting rolls back only that setting and does not block unrelated valid settings. The dialog stays open after a partial failure, preserves each failed input as an editable draft, and shows a summary at the top plus a specific error beside the affected control or shortcut row.

The design directly fixes the reproduced defect in which an unrelated global-hotkey registration failure prevents a 60 fps video setting from being saved or applied. It also establishes consistent behavior for validation failures, AirPlay restart failures, shared settings-file failures, deferred application, and recovery failures.

This feature changes only Apply behavior in the Settings dialog. Existing immediate saves for toolbar volume, aspect-ratio lock, video-fit mode, and window state remain outside this transaction.

## Confirmed product decisions

- Roll back at individual-field granularity. Each shortcut row is an individual field.
- Every click on Apply processes the full dialog draft through the ordinary Apply path. There is no special retry-only optimization; unchanged fields naturally return `Unchanged`.
- A partial failure keeps the dialog open. All successful fields are committed immediately.
- Failed values remain visible as drafts even though their saved and runtime values roll back.
- Editing a failed field immediately clears its stale error.
- Cancel discards only unapplied drafts. It never reverses fields already committed by an earlier Apply from the same dialog session.
- A settings-file write failure aborts the complete Apply because all fields share one JSON file.
- Receiver name and video quality are applied as one AirPlay configuration batch and require at most one restart.
- Choosing to wait for the current AirPlay session to disconnect counts as a successful, deferred application.
- A later deferred-application failure displays a specific modal error and rolls back the affected saved fields. It does not retain the failed draft for the next opening of Settings.
- If compensation cannot restore the previous runtime state, expose a distinct recovery failure. Never claim that rollback succeeded.
- Summaries appear at the top of the Settings dialog. Errors also appear beside their individual fields.

## Fields and failure boundaries

The coordinator treats these Settings-dialog controls as fields:

- receiver name;
- video resolution;
- video frame rate;
- each `ShortcutAction` row independently;
- recording format;
- recording output directory;
- recording-completion notification preference.

Receiver name, resolution, and frame rate share one AirPlay application operation. If that shared restart fails, every changed field that depended on the operation fails and rolls back. Unchanged receiver fields are not marked as failed.

Two shortcut rows that contain the same candidate sequence form one validation conflict. Both rows fail validation because neither candidate can be selected safely as the winner. Other shortcut rows and non-shortcut settings continue.

The JSON file is a global persistence boundary, not a field. An open, write, or commit failure prevents every field in that Apply from being committed.

## Architecture

### SettingsDialog

`SettingsDialog` owns the editable draft and presentation state. It no longer calls `QDialog::accept()` immediately when the Apply button is pressed.

Its responsibilities are:

- collect the complete candidate `AppSettings` draft;
- request an apply operation;
- display a top summary and field-specific results;
- keep failed candidate values visible;
- adopt successful values as the new committed baseline;
- clear a field's stale error as soon as that field is edited;
- close only when the outcome contains no failed or recovery-failed fields.

If a partial Apply succeeds for some fields, the dialog must retain two concepts:

- the latest committed baseline, used by Cancel and future comparisons;
- the current UI draft, which may still contain failed attempted values.

Apply always submits the full current draft. The coordinator, rather than the dialog, decides which values are unchanged, valid, applicable, or recoverable.

### SettingsApplyCoordinator

Add a coordinator dedicated to planning and executing Settings Apply. It must not display dialogs or directly manipulate widgets.

The coordinator receives:

- the latest committed `AppSettings` baseline;
- the full candidate draft;
- the receiver/session/recording state needed to plan AirPlay changes;
- the hotkey service, settings store, receiver configuration service, and deferral service.

It returns a structured outcome containing:

- the resulting committed `AppSettings` snapshot;
- one result for each submitted field;
- an optional global persistence error;
- whether an AirPlay change is deferred;
- whether the Settings dialog may close.

Use stable field identifiers rather than UI labels. Shortcut fields include their `ShortcutAction`. A field result carries the attempted value, status, user-facing reason, optional native/system error code, and optional recovery error.

Required statuses are:

- `Applied`;
- `Unchanged`;
- `Deferred`;
- `ValidationFailed`;
- `ApplyFailedRolledBack`;
- `RecoveryFailed`.

A global settings-file failure is represented separately as `PersistenceFailed`, because it applies to the complete transaction rather than one field.

### Planning and the active-session decision

Applying receiver settings during an active session requires a user decision, while the coordinator must remain UI-independent. Use a two-phase operation:

1. `plan` validates the full draft and reports whether the valid receiver changes require an active-session decision.
2. The Settings-dialog controller presents explicit choices: `Disconnect and apply now` and `Apply after disconnect`.
3. `execute` receives the plan plus the selected timing.

Do not prompt when every receiver field is unchanged or invalid. The same plan contains the per-field validation results used during execution, preventing the prompt and execution phases from interpreting the draft differently.

### MainWindow

`MainWindow` connects Settings-dialog requests to the coordinator and supplies application services. It is responsible for:

- presenting the active-session timing decision requested by a plan;
- passing the outcome back to `SettingsDialog`;
- adopting the coordinator's committed settings;
- updating shortcut tooltips and other main-window projections;
- displaying a modal error when a deferred AirPlay operation later fails;
- avoiding a second receiver restart when name and video quality changed together.

Detailed transaction and error logic must not remain embedded in `MainWindow::showSettingsDialog()`.

### HotkeyService

Replace the Boolean-only registration result with a structured result that contains:

- whether the requested binding registered;
- the Windows error code and formatted system message when it failed;
- whether the previous binding was restored;
- a separate recovery error when restoration failed.

Apply shortcut rows independently. Do not call `unregisterAll()` as the Settings Apply strategy. Registering a candidate for one action must leave other actions untouched. A failed candidate restores that action's previous binding while successful actions remain active.

Startup may still register every configured action, but it must report which actions failed rather than collapsing them into one Boolean.

### AppSettingsStore

Replace the Boolean-only save result with a detailed result containing:

- the absolute target path;
- failure stage: open, write, or commit;
- `QFileDevice` error and `errorString()`;
- success state.

Continue using `QSaveFile` so each JSON replacement remains atomic. Do not split settings across multiple files.

### AirPlay receiver configuration

Introduce one batch operation for receiver name and video quality. The request identifies which of name, resolution, and frame rate changed, and carries both the requested and rollback values.

The batch result must distinguish:

- applied successfully;
- accepted for deferred application;
- apply failed and old configuration was restored;
- apply failed and restoration also failed.

When name and video quality change together, store the new values and perform at most one receiver/discovery restart. A shared restart failure marks all changed receiver fields as failed while leaving unrelated settings committed.

Extend deferred receiver state so one pending batch retains the requested values and rollback snapshot. If the later apply fails, restore the affected persisted fields and show one modal error containing the individual fields and backend reason.

## Apply transaction

Each Apply follows these phases.

### 1. Validate every field

Validate all fields without stopping at the first error. Invalid fields retain their committed values in the prospective committed snapshot. Valid unrelated fields continue.

Validation includes the existing rules:

- receiver name is not empty;
- shortcut sequences are not empty;
- shortcut sequences are unique;
- each shortcut uses one supported native key combination.

Recording-path Choose/Open action errors remain independent UI action errors. They are not converted into Apply failures unless the candidate setting itself fails validation or persistence.

### 2. Apply shortcut rows independently

Process every valid shortcut field through the ordinary registration API, including unchanged rows. The service may return `Unchanged` without a system call when its actual registration already matches, but the coordinator does not maintain a special retry subset.

For a failed candidate:

- if the old binding is restored, keep the old binding in the committed snapshot and return `ApplyFailedRolledBack`;
- if the old binding cannot be restored, keep the last persisted value but return `RecoveryFailed` and state that the action currently has no confirmed global shortcut.

Track every binding changed during this attempt so it can be compensated if global persistence fails.

### 3. Build the prospective committed snapshot

Merge candidate values for successful fields with baseline values for failed fields. This produces the only snapshot eligible for JSON persistence.

### 4. Persist JSON atomically

Save the prospective snapshot before starting an AirPlay restart.

If saving fails:

- restore every hotkey changed during this Apply;
- do not apply or defer receiver changes;
- leave the in-memory committed baseline unchanged;
- return a global `PersistenceFailed` result;
- keep the dialog open with all candidate inputs intact.

If any hotkey restoration also fails, include its field-level `RecoveryFailed` result alongside the global persistence error.

### 5. Apply or defer the receiver batch

After persistence succeeds:

- immediate timing applies all valid changed receiver fields using at most one restart;
- deferred timing stores one pending batch and marks its fields `Deferred`;
- a successful immediate operation marks its fields `Applied`;
- an immediate failure restores the old runtime receiver configuration and rewrites JSON with only the affected receiver fields rolled back;
- successful shortcut and recording fields remain committed during that compensating JSON write.

If the compensating JSON write or backend restoration fails, return `RecoveryFailed` with the known saved state, known runtime state, and explicit uncertainty. The UI must not present a normal rolled-back message.

No ordering can provide a true atomic transaction across JSON, Windows global hotkeys, and an AirPlay restart. This order deliberately prevents an unwritable settings file from causing an AirPlay disconnect and uses explicit compensation for later failures.

## Dialog behavior and messages

Place the summary above all setting groups. Hide it when there is no error or pending draft state.

Examples:

- partial success: `Some settings were applied. 2 settings were not applied; correct the highlighted fields.`
- persistence failure: `Could not save C:\path\airplay-settings.json: Access is denied. No changes from this Apply were committed.`
- recovery failure: `The new shortcut failed and the previous shortcut could not be restored. Toggle toolbar currently has no global shortcut.`

Place receiver-name, resolution, and frame-rate errors directly below their controls. Add a Status column to the shortcut table for row-specific errors. Every message names the field, attempted value, immediate cause, and recovery result. When Windows supplies an unrecognized error, include its numeric code.

After a partial outcome:

- successful values update the dialog's committed baseline;
- failed candidate values remain in their controls and are highlighted;
- editing a highlighted field clears that field's stale result immediately;
- the top summary recomputes from the remaining errors and unapplied draft;
- Apply remains available and processes the full draft again;
- Cancel closes and discards only values that were never committed.

If all fields are `Applied`, `Unchanged`, or `Deferred`, close the Settings dialog without a success modal. The active-session prompt uses explicit actions rather than Yes/No, so the user already knows when a receiver change will be deferred.

If a deferred batch later fails after Settings has closed:

- restore the affected JSON fields when possible;
- display one modal error containing the attempted values, backend reason, and rollback result;
- do not preserve the failed draft for the next Settings session.

## Error text sources

Specific errors must originate at the failing boundary rather than be guessed by the UI:

- input validation supplies field and rule;
- `WindowsHotkeyService` captures `GetLastError()` immediately after `RegisterHotKey` fails and formats it with the Windows message API;
- `AppSettingsStore` supplies path, save stage, `QFileDevice::FileError`, and `errorString()`;
- the AirPlay batch API supplies its restart/apply and restoration errors;
- the coordinator adds transaction context, such as whether the old value was restored or another field remained committed.

The UI may translate or format these results but must not replace them with generic messages such as `Could not register one or more shortcuts` or `Could not apply video quality`.

Keep new UI strings in English to match the current application.

## Test strategy

### Regression test for the reported defect

Add a deterministic MainWindow/Settings integration test:

1. Configure `FakeHotkeyService` to reject an existing default shortcut.
2. Open Settings and change only frame rate to 60 fps.
3. Apply.
4. Assert that the failed shortcut keeps its committed value and displays the specific error.
5. Assert that JSON contains 60 fps.
6. Assert that the receiver receives 60 fps.
7. Assert that the dialog remains open because one field failed.

The existing no-conflict 60 fps case remains the control.

### Coordinator tests

Cover at least:

- one and multiple validation failures with unrelated success;
- one shortcut conflict among multiple shortcut changes;
- candidate registration failure with successful old-binding restoration;
- candidate and restoration registration failures;
- open, write, and commit persistence failures;
- persistence failure followed by hotkey compensation failure;
- combined name and quality change causing one restart;
- shared restart failure rolling back only changed receiver fields;
- receiver restoration failure and compensating JSON failure;
- immediate and deferred receiver application;
- a second Apply processing the full draft;
- partial success followed by Cancel.

### SettingsDialog tests

Cover:

- summary placement above all groups;
- field and shortcut-row error placement;
- failed draft preservation;
- stale error clearing on edit;
- committed-baseline updates after partial success;
- partial failure keeping the dialog open;
- success/deferred-only outcomes closing it;
- explicit active-session decision labels.

### Boundary result tests

Cover:

- hotkey native error code/message capture and restoration state;
- settings path and open/write/commit error reporting;
- AirPlay batch success, deferred, rollback-success, and recovery-failure results;
- one restart for a combined receiver batch.

Stable regression tests use fakes for hotkey contention and receiver failures. Real Windows `RegisterHotKey` contention depends on machine state and is not required for the deterministic suite.

## Acceptance criteria

- A hotkey conflict cannot prevent a valid frame-rate or other unrelated setting from being saved and applied.
- One failed shortcut cannot roll back another successful shortcut.
- Validation errors permit unrelated valid fields to commit.
- A shared JSON failure commits no field from that Apply and starts no AirPlay restart.
- Name and video quality changes share at most one AirPlay restart.
- Every failure identifies the affected field or global boundary, attempted value, direct reason, and rollback outcome.
- The dialog retains failed inputs, clears stale errors on edit, and closes only when no failed fields remain.
- Recovery failures are distinguishable from successful rollback.
- Existing unrelated settings, recording, receiver lifecycle, and hotkey behavior remain covered by the full test suite.

## Out of scope

- Splitting settings into multiple files.
- Redesigning toolbar-originated immediate settings saves.
- Automatically restarting the whole application after recovery failure.
- Retaining a deferred failure draft across Settings sessions.
- Adding a special retry-only Apply mode.
- Requiring a real external process to occupy a Windows hotkey in automated tests.
