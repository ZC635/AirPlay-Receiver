#pragma once

#include "app/SettingsApplyTypes.h"
#include "backend/ReceiverConfigurationChange.h"
#include "backend/RecordingTypes.h"
#include "backend/ReceiverState.h"

#include <QPointer>
#include <optional>
#include <memory>
#include <functional>
#include <QObject>

class AirPlayReceiver;
class HotkeyService;
class SettingsChangeDeferrer;
class SettingsPersistence;
class DiagnosticLogSink;
struct SettingsApplyPlan;

class SettingsApplyCoordinator : public QObject {
    Q_OBJECT
public:
    using RecordingPresentationCompletion = std::function<void()>;
    using RecordingPresentationDispatcher = std::function<void(RecordingPresentationCompletion)>;
    using TimingChooser = std::function<std::optional<ReceiverApplyTiming>()>;
    SettingsApplyCoordinator(AppSettings &current, HotkeyService *hotkeys,
        SettingsPersistence *persistence, AirPlayReceiver *receiver,
        DiagnosticLogSink *diagnosticSink = nullptr, QObject *parent = nullptr,
        RecordingPresentationDispatcher presentationDispatcher = {});
    ~SettingsApplyCoordinator() override;
    SettingsSubmitResult apply(const AppSettings &fullDraft, const TimingChooser &chooseTiming);
    ReceiverStartPreparationResult prepareReceiverStart();
    void endReceiverLifecycle();
signals:
    void deferredApplyFinished(SettingsDeferredResult result);
private:
    SettingsApplyPlan plan(const AppSettings &baseline,
                           const AppSettings &candidate,
                           bool receiverSessionActive,
                           RecordingState recordingState) const;

    SettingsApplyOutcome executePlan(const SettingsApplyPlan &plan, ReceiverApplyTiming timing);
    ReceiverConfigurationBatchRequest mergeSavedReceiverTarget(
        const AppSettings &savedTarget, const ReceiverConfigurationBatchRequest &delta) const;
    SettingsApplyOutcome completeDeferredReceiverApplyImpl(
        const ReceiverConfigurationBatchRequest &batch, const AppSettings &currentlyCommitted);
    QPointer<HotkeyService> hotkeys_;
    SettingsPersistence *persistence_ = nullptr;
    QPointer<AirPlayReceiver> receiver_;
    std::unique_ptr<SettingsChangeDeferrer> deferrer_;
    AppSettings &current_;
    RecordingPresentationDispatcher presentationDispatcher_;
    SettingsSubmitResult *activeSubmit_ = nullptr;
    bool busy_ = false;
    bool applying_ = false;
    bool ready_ = false;
    bool preparedStart_ = false;
    bool awaitingRecordingResult_ = false;
    quint64 recordingVersion_ = 0;
    quint64 presentationVersion_ = 0;
    bool timingAuthorized_ = false;
    bool timingSelectionPending_ = false;
    ReceiverApplyTiming pendingTiming_ = ReceiverApplyTiming::Immediate;
    quint64 epoch_ = 0;
    quint64 version_ = 0;
    quint64 operationEpoch_ = 0;
    std::optional<std::pair<quint64, quint64>> posted_;
    std::optional<std::pair<quint64, quint64>> readyWhileBusy_;
    bool sessionActive() const;
    bool recordingBlocked() const;
    void receiverStateChanged(ReceiverState state);
    void recordingStateChanged(RecordingState state);
    void recordingTerminated();
    void consumeReady(quint64 epoch, quint64 version);
    void finishOperation();
    ReceiverConfigurationBatchResult invokeBackend(const ReceiverConfigurationBatchRequest &batch);

    DiagnosticLogSink *diagnosticSink_ = nullptr;
    std::optional<ReceiverConfigurationBatchRequest> pendingReceiverBatch_;
};
