#include "diagnostics/DiagnosticLogSink.h"

namespace {

class NullDiagnosticLogSink final : public DiagnosticLogSink {
public:
    void record(DiagnosticEvent) override {}
};

} // namespace

DiagnosticLogSink &nullDiagnosticLogSink() {
    static NullDiagnosticLogSink sink;
    return sink;
}
