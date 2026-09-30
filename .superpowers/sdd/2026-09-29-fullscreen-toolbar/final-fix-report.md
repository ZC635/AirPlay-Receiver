# Final review fix: fullscreen restoration and topmost state

## Change

Fullscreen exit now uses the exact pre-fullscreen normal rectangle only while it intersects an available screen. If that screen is gone, it keeps Qt's valid relocated rectangle. If Qt's rectangle is also offscreen, it clamps the original size and position to an available screen. Maximized restoration remains on its existing path. A Windows QPA test checks the actual `WS_EX_TOPMOST` style before, during, and after fullscreen.

## TDD evidence

RED: `cmake --build build --target MainWindowSmokeTest -j 4` first failed because the new `FullscreenRestoreGeometry.h` test seam was absent. With the prior unconditional-original behavior represented by the helper, the focused run of `fullscreenRestoreGeometryKeepsOriginalOnAvailableScreen`, `fullscreenRestoreGeometryKeepsQtRelocationWhenMonitorDisappears`, and `fullscreenRestoreGeometryClampsWhenBothRectsAreOffscreen` reported 3 passed and 2 failed (including Qt relocation and clamping).

GREEN: After the fix, the same three cases plus `fullscreenRestoresNormalGeometryAndToolbarChoice` and `fullscreenRestoresMaximizedWindow` reported 7 passed, 0 failed, including QtTest setup/cleanup. `nativeFullscreenPreservesTopmostWindowStyle` on Windows QPA reported 3 passed, 0 failed, including setup/cleanup.

`cmake --build build -j 4` passed. Registered CTest run for `MainWindowNativeFullscreenKeysTest`, `MainWindowNativeFullscreenAspectTest`, `MainWindowNativeFullscreenTopmostTest`, `LanguageManagerTest`, `ToolbarWidgetTest`, and `ToolbarVisibilityControllerTest` passed 6/6. `git diff --check` found no whitespace errors.

## Review and limit

The helper tests simulate available-screen geometry because a physical monitor could not be disconnected during this run. The restored normal rectangle remains exact on an available screen; native topmost was verified using the existing Windows helper. The ignored `build-ui/` directory remains untracked and was not staged.
