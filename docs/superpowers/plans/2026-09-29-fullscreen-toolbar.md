# Fullscreen Toolbar Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add reversible fullscreen and consistent, configurable toolbar hover behavior.

**Architecture:** MainWindow owns fullscreen snapshots and receiver transitions. A focused ToolbarVisibilityController owns toolbar visibility and pointer regions; existing settings transactions persist the preference. ToolbarWidget owns button presentation.

**Tech Stack:** Windows, C++17, Qt6 Widgets/QtTest, CMake/Ninja, MSYS2 UCRT64, UxPlay/GStreamer.

**Spec:** `docs/superpowers/specs/2026-09-29-fullscreen-toolbar-design.md`

## Global Constraints

- Fullscreen transition never independently changes toolbar visible/hidden state.
- Fullscreen button immediately left of Settings; label/tooltip `Fullscreen` / `Exit Fullscreen`, Chinese `全屏` / `退出全屏`; no F11 tooltip suffix.
- F11 toggles and Esc exits only in foreground app; no global registration or double-click behavior.
- Preserve video fit; suspend aspect enforcement in fullscreen; restore normal geometry/maximized state; never persist fullscreen.
- Checkbox Chinese text `工具栏隐藏时，鼠标移到顶部显示`, default true.
- Work only in `C:\Users\Z\.codex\worktrees\fullscreen-toolbar\AirPlay`; primary checkout is read-only reference.

## Review Focus

- Native video consumes mouse messages: polling fallback still reveals/hides, covered in Task 3.
- Dialog/popup focus and Esc: dismiss controls without accidental fullscreen exit, covered in Task 2/3.
- Negative monitor origins and mixed DPI: global logical rectangles hit correctly, covered in Task 3.
- Fullscreen close after entering maximized: restart retains maximized normal geometry, covered in Task 2.
- Duplicate receiver notifications and explicit hide at top: manual intent survives until a real transition/re-entry, covered in Task 3.

## Build baseline and commands

No applicable AGENTS.md found in this worktree or ancestor directories. Branch `codex/fullscreen-toolbar` starts at the stated base. Installed tools are `C:\msys64\ucrt64\bin`; prepend that path in each shell. Native tests already have Windows QPA entries; most tests use offscreen through CTest.

Baseline attempted: `cmake -S . -B build-ui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DAIRPLAY_WITH_UXPLAY=OFF` configured successfully. Building MainWindowSmokeTest and related UI targets failed before tests: existing `src/backend/VideoFrameBridge.h:6` includes `gst/gst.h`, but OFF omits dependency include paths. No baseline tests passed or ran. `third_party/uxplay` in this worktree is empty. Do not mistake this for a feature regression.

For implementation, materialize the pinned UxPlay dependency from local Git objects if available; otherwise copy the primary checkout dependency contents into this worktree without changing the primary. Verify the dependency revision against the gitlink. Configure `build/` (already ignored) with `-DAIRPLAY_WITH_UXPLAY=ON -DNO_MARCH_NATIVE=ON -DCMAKE_BUILD_TYPE=Debug -G Ninja`. Build needed test targets first; use full build and CTest at final validation. `build-ui/` created during baseline is untracked; never add its artifacts. Docs are ignored and need `git add -f`.

Common targeted invocation after building targets:
`ctest --test-dir build --output-on-failure -R "^(AppSettingsTest|AppSettingsStoreTest|SettingsDialogTest|SettingsDialogHighDpiTest|SettingsApplyTypesTest|SettingsApplyCoordinatorTest|ToolbarWidgetTest|MainWindowSmokeTest|WindowStateStoreTest|LanguageManagerTest|ToolbarVisibilityControllerTest)$"`

---

### Task 1: Persist and apply toolbar hover preference

**Files:** Modify `src/app/AppSettings.{h,cpp}`, `AppSettingsStore.cpp`, `SettingsApplyTypes.{h,cpp}`, `SettingsDialog.{h,cpp}`, `translations/airplay_zh_CN.ts`; tests `tests/app/AppSettingsTest.cpp`, `AppSettingsStoreTest.cpp`, `SettingsApplyTypesTest.cpp`, `SettingsApplyCoordinatorTest.cpp`, `SettingsDialogTest.cpp`, `LanguageManagerTest.cpp`.

**Interfaces:** Produce `bool AppSettings::toolbarHoverReveal() const`, `void setToolbarHoverReveal(bool)`, `SettingsFieldKind::ToolbarHoverReveal`, `SettingsFieldId::toolbarHoverReveal()`. JSON root key `toolbarHoverReveal`; checkbox object name `toolbarHoverRevealCheckBox`.

- [ ] Add failing cases: defaults true; false round-trip; missing/string/null JSON defaults true; settings field value/copy/diff includes the bool; dialog checkbox draft, cancel, successful apply, failed-save rollback; exact Chinese text after language switch. Follow existing completion-notification field pattern for transactional UI refresh.
- [ ] Build and run the six affected test targets; confirm new cases fail for missing interface/behavior.
- [ ] Implement the getter/setter, boolean-only loading and serialization, complete field plumbing and General checkbox. English source text is `Show hidden toolbar when the pointer reaches the top`. Check existing exhaustive field switches and dialog outcome/dirty/baseline logic; the new field is local and requires no receiver restart.
- [ ] Run affected tests plus SettingsDialogHighDpiTest; require all pass and no translation omissions.
- [ ] Commit `feat: add toolbar hover reveal preference`.

### Task 2: Fullscreen button and reversible lifecycle

**Files:** Modify `src/app/MainWindow.{h,cpp}`, `ToolbarWidget.{h,cpp}`, `translations/airplay_zh_CN.ts`; tests `tests/app/MainWindowSmokeTest.cpp`, `ToolbarWidgetTest.cpp`, `LanguageManagerTest.cpp`; register native cases in `tests/CMakeLists.txt`.

**Interfaces:** Produce public `void MainWindow::setFullscreenEnabled(bool)` (state read via QWidget::isFullScreen), ToolbarWidget `void setFullscreenChecked(bool)` and signal `void fullscreenToggled(bool)`. Object name `fullscreenButton`. Retain an optional `WindowStateSnapshot` from immediately before entry; existing WindowStateStore format remains sufficient.

- [ ] Add failing tests: button order/checkability/text/tooltip with no F11; enter/exit normal and maximized restore; fit preference unchanged; toolbar hidden/shown unchanged on both transitions; disconnected entry survives Discoverable and exits only after active session ends; close while fullscreen restores normal/maximized next launch. Parameterize active end states including Error.
- [ ] Run new tests to see missing behavior fail.
- [ ] Implement snapshot-on-entry/showFullScreen/current-screen and restore-on-exit; use signal blockers for button synchronization. Save snapshot instead of live fullscreen geometry; defensively clear fullscreen on startup restore. Gate nativeEvent aspect handling and enforceAspectRatio while fullscreen, including incoming video-size updates. Restore geometry without immediately enforcing a new ratio.
- [ ] Add local F11/Esc handling scoped to active main window (Qt WindowShortcut or filtered key delivery), ignore autorepeat, and preserve popup/modal handling. Never add ShortcutAction/global hotkeys. Test native foreground/background, child control focus, dialog Esc dismissal, and aspect WM_SIZING suspension; add Windows QPA CTest registrations beside existing native cases.
- [ ] Run affected tests and native cases; verify button checked appearance uses the same existing style as Pin, language switches update current fullscreen text, and always-on-top survives transitions.
- [ ] Commit `feat: add reversible fullscreen mode`.

### Task 3: Unified toolbar policy and native-safe hover

**Files:** Create `src/app/ToolbarVisibilityController.{h,cpp}`, `tests/app/ToolbarVisibilityControllerTest.cpp`; modify `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/app/MainWindow.{h,cpp}`, and `tests/app/MainWindowSmokeTest.cpp`. Modify ToolbarWidget only if needed to expose owned interactive regions.

**Interfaces:** Controller constructor `ToolbarVisibilityController(QWidget *content, QWidget *toolbar, QObject *parent = nullptr)`; methods `void receiverStateChanged(ReceiverState)`, `void setHoverRevealEnabled(bool)`, `void toggleManually()`, `bool isVisible() const`, `void evaluatePointer(const QPoint &globalPosition, bool active)`; signal `void visibilityChanged(bool)`. Controller installs/removes its event filter and owns its timer. Deterministic evaluatePointer is the test seam used by both events and timer.

- [ ] Add failing controller tests for initially shown, Connected hidden, other changed states shown, duplicate notification preserves manual choice, manual shown survives leaving, hover reveal/leave, preference false, deactivation, explicit hide requiring leave/re-entry. Assert fullscreen-independent controller input has no fullscreen state.
- [ ] Add geometry cases with a top band at a negative global origin: left/middle/right points reveal, y just below band hides unless over toolbar/control; expanded volume and owned popup rectangles retain only while inside; foreign windows and modal dialogs do not retain; hiding/closing popup reevaluates. Test logical coordinates independent of devicePixelRatio.
- [ ] Run the new target and confirm failure, then implement baseline visibility plus temporary reveal and re-entry suppression. Derive band from content global rect and toolbar height/sizeHint; query current widget rectangles each evaluation. Only visible toolbar-owned popups extend the region. Events evaluate immediately; 25 ms active-window cursor fallback handles native surfaces. Stop work when inactive or disabled and remove filters safely on destruction.
- [ ] Replace direct toolbar visibility writes in MainWindow with controller forwarding and one render connection that raises native overlays. Preserve status-label rules. Forward committed preference after settings apply and actual receiver state notifications; fullscreen setters never reset controller state. Reevaluate geometry changes without dropping temporary visibility merely because a mode transition is in progress.
- [ ] Add integration tests: temporary/manual toolbar across fullscreen entry/exit, connected state auto-hide, disconnect exit/full toolbar, immediate disabling through settings, native surface cursor fallback, popup focus return. Run controller + MainWindow tests including native overlay stacking and high-DPI cases.
- [ ] Build all targets (also `RendererSampleTapsTest` when UxPlay ON), run full CTest once. Manually check current-monitor fullscreen, maximized restore, F11/Esc foreground scope, Pin-like colors, full-width top reveal and immediate leave hide with volume/menu at available DPI/monitor setups. Record untested hardware conditions explicitly.
- [ ] Commit `feat: unify toolbar hover visibility in windowed and fullscreen modes`.

## Handoff

The user already authorized plan then implementation, so execution proceeds without another approval request. Self-review checked every spec requirement against the three tasks and their interfaces; no separate refactor or release work is required.
