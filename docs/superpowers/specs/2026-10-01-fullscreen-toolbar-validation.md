# Fullscreen and toolbar validation

Implemented on `codex/fullscreen-toolbar`, based on `ed1da8d`, for issue #6.

## Delivered

- Fullscreen/Exit Fullscreen button before Settings, checked styling matching Pin; local F11 toggle and Esc exit.
- Restore normal/maximized geometry, preserve fit/topmost, suspend aspect sizing, never persist fullscreen; exit after active receiver session ends.
- Shared manual/receiver/hover toolbar policy in both window modes.
- Persisted default-on hover setting with Chinese translation and settings transaction coverage.
- Full-width content-top trigger at toolbar height; immediate leave hiding, owned-control retention, native cursor fallback.
- Screen-aware restore when original monitor is unavailable.

## Final evidence (2026-10-01)

`cmake --build build -j 4` succeeded.

`ctest --test-dir build --output-on-failure -j 4`: 63 entries, 62 passed, 0 failed, 1 skipped, 41.40 seconds. All new registered tests passed, including native toolbar hover, fullscreen keys/aspect/topmost and controller high-DPI cases.

The five stale MainWindowSmokeTest expectations following base commit ed1da8d were updated to the enabled aspect ratio lock and video fit defaults. The startup save condition now compares against loaded settings, and a regression checks that constructing the window with both enabled does not create a settings file. The smoke executable recorded 132 passed, 0 failed and 8 Windows-only skips. GStreamerPluginReadinessProcessTest is the skipped CTest entry.

Native hover repeat verification: 20/20 passed after moving the probe inside the top band, restoring the OS cursor on exit, and serializing the native test. Earlier intermittent failures coincided with a 2-pixel OS cursor change; its source was unconfirmed, so production policy was preserved.

Task reviews, scoped fix reviews and whole-branch review completed. Final test-only stabilization review found no new Important/Critical breakage. Native topmost tests read WS_EX_TOPMOST before/during/after fullscreen. Missing-monitor tests simulate available screen rectangles.

## Limits and decision

Physical multi-monitor removal, mixed DPI, live AirPlay playback and manual color inspection were not verified. The controller keeps temporary visibility unchanged through fullscreen geometry changes until actual cursor movement; this follows the approved requirement. Cost: a temporarily shown toolbar can remain visible until the first movement after switching modes.

The worktree and build remain available for testing. The initial ignored-build experiment `build-ui/` is generated untracked output.
