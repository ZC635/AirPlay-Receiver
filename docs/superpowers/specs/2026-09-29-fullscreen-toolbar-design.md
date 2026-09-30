# Fullscreen and toolbar behavior

## Scope

Implement GitHub ZC635/AirPlay-Receiver issue #6 (add fullscreen mode; no issue body) and the approved toolbar refinements. Base: ed1da8d0932bf97652a569e3727fa604f6766d54. Windows, C++17, Qt6 Widgets; preserve the existing receiver and rendering architecture.

## Fullscreen

The main window fills its current monitor. A checkable button immediately left of Settings uses the same black/white checked styling as Pin. Its label and tooltip are `Fullscreen` / `Exit Fullscreen`, translated as `全屏` / `退出全屏`. Do not put F11 in its tooltip. F11 toggles and Esc exits while this app is foreground; neither is a global registered hotkey. Do not add double-click behavior. Modal dialogs retain their normal key handling (especially Esc dismissal), and popup dismissal must not accidentally exit fullscreen.

Capture normal geometry and maximized state once on entry; restore them on exit. Preserve always-on-top. Keep video fit preference and presentation unchanged. Suspend both native aspect sizing and programmatic aspect enforcement while fullscreen; preserve the preference and resume future sizing on exit without immediately distorting restored geometry.

Fullscreen is available disconnected. Startup/discoverable notifications alone do not exit it. Once a Connecting or Connected session becomes inactive (including error), exit fullscreen. Restart/close saves the pre-fullscreen normal/maximized snapshot, never fullscreen. Defensive restoration clears any legacy fullscreen flag.

A fullscreen transition never independently changes toolbar visible/hidden state, including temporary visibility. Receiver state changes retain their own rules.

## Toolbar visibility

Use one policy in normal and fullscreen modes. On an actual receiver state change, Connected hides the toolbar and other states show it, as today. Duplicate notifications do not discard a manual choice. A manually shown toolbar remains shown until manually hidden or a receiver state change. The existing toolbar shortcut stays available.

Add a persisted General checkbox with exact Chinese text `工具栏隐藏时，鼠标移到顶部显示`; English text is `Show hidden toolbar when the pointer reaches the top`. Default true; missing or invalid stored values use true. Apply through the existing transactional settings flow, including dirty tracking, cancel, failed-save rollback and language changes.

When baseline visibility is hidden and hover is enabled, reveal temporarily when the pointer enters the full-width top CONTENT band. Band height is the current toolbar height (size hint when hidden), in Qt logical coordinates. Temporary visibility lasts only while the pointer is in the union of that band, visible toolbar/volume control, and visible toolbar-owned menus/popups. Leaving the union hides immediately, with no inactivity timeout or animation. An open control does not grant unlimited visibility after the pointer leaves its region. Modal settings dialogs do not count as hover retention regions. Deactivation clears temporary reveal and prevents background hover activation; manual visibility remains intact. Turning the preference off clears temporary reveal immediately.

Manual toggle acts on current visible state: a temporary reveal can be explicitly hidden, and hidden can be made persistently visible. After manual hide inside the top band, suppress re-reveal until the pointer leaves the region and re-enters. Pointer geometry uses mapToGlobal and QCursor::pos, never raw physical-pixel comparisons.

## Design

Keep fullscreen lifecycle in MainWindow, toolbar button and translated copy in ToolbarWidget, preference in AppSettings/AppSettingsStore/SettingsApplyTypes/SettingsDialog. Introduce a small ToolbarVisibilityController QObject to own baseline/manual/temporary visibility and hover hit testing, instead of further mixing it into the large MainWindow. MainWindow supplies the central content widget and ToolbarWidget, forwards receiver state changes and committed preference changes, and responds to visibilityChanged by preserving native overlay raising. Status-label behavior remains tied to receiver state.

Use an application event filter for mouse, enter/leave, activation and popup events, scoped to this window and its controls. A short cursor polling fallback (about 25 ms while the main window is active and hover can matter) handles native video surfaces that consume Qt mouse events. Events evaluate immediately; no deliberate hide delay. Use one pointer evaluation path so polling and event delivery cannot disagree. Reevaluate when controls resize or language changes. Fullscreen layout changes do not synthesize a toolbar policy change.

## Acceptance and limits

Automated tests cover preference transactions, button semantics/translations, fullscreen restore/persistence/session transitions, toolbar policy and deterministic hit testing. Native Windows tests cover foreground keyboard handling, aspect sizing suspension and overlay stacking. Manually inspect multiple monitors and non-100% DPI where hardware is available, including volume/menu use and a native video surface. If unavailable, report that limit explicitly. Do not claim real AirPlay, multi-monitor or native visual verification from offscreen tests.
