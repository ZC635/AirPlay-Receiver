# Shortcut Status Column Visibility Design

## Goal

Preserve the pre-existing two-column hotkey binding layout while no shortcut apply error needs attention. Reveal the `Status` column only when it contains an actionable shortcut failure.

## Behavior

- The shortcut table hides the `Status` column when the settings dialog first opens.
- After Apply, the column is shown when at least one shortcut field has a failure that the dialog would present inline.
- The column remains visible while any such shortcut failure remains.
- Editing a failed shortcut clears that row's result. When no shortcut failures remain, the column is hidden immediately.
- Non-shortcut failures do not reveal the shortcut `Status` column.
- Successful, deferred, and unchanged shortcut results do not reveal the column.

## Implementation

Keep the existing three-column table and per-row status labels. `SettingsDialog::refreshFieldErrors()` will derive whether any shortcut failure is eligible for inline display using the same filtering rules used to populate the labels, then call `setColumnHidden(2, ...)` once after refreshing the results.

This keeps error formatting and partial-apply behavior unchanged and avoids rebuilding table cells when visibility changes.

## Testing

Extend `SettingsDialogTest` with focused UI assertions covering:

1. The `Status` column is hidden on initial display.
2. A shortcut apply failure reveals it.
3. Editing the last failed shortcut hides it again.
4. A non-shortcut failure leaves it hidden.
