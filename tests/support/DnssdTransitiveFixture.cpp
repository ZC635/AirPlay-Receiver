extern "C" __declspec(dllimport) int dnssd_fixture();
extern "C" __declspec(dllexport) int dnssd_middle_fixture() { return dnssd_fixture(); }
