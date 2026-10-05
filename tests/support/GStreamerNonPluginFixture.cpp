// A loadable PE DLL with no GStreamer registration symbol or side effects.
extern "C" __declspec(dllexport) int airplay_non_plugin_fixture() { return 42; }
