#ifdef DNSSD_TRANSITIVE_FIXTURE
extern "C" __declspec(dllimport) int dnssd_middle_fixture();
int main() { return dnssd_middle_fixture() == 7 ? 0 : 1; }
#else
extern "C" __declspec(dllimport) int dnssd_fixture();
int main() { return dnssd_fixture() == 7 ? 0 : 1; }
#endif
