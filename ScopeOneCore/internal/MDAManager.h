#pragma once

#include <QHash>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>
#include <atomic>
#include <functional>
#include <vector>
#include "internal/AcquisitionHardware.h"
#include "scopeone/ExperimentDocument.h"
#include "scopeone/CameraProvider.h"
#include "scopeone/HardwareCapabilities.h"

namespace scopeone::core::internal
{
    struct MDAOutput
    {
        AcquisitionEvent event;
        QMap<QString, ImageFrame> frames;
        quint64 startedTimestampNs{0};
        quint64 completedTimestampNs{0};
        bool succeeded{false};
        QString errorMessage;
    };

    class MDAManager : public QObject
    {
        Q_OBJECT

    public:
        explicit MDAManager(QObject* parent = nullptr);
        ~MDAManager() override;

        bool isRunning() const { return m_running.load(); }

        void setCameraProvider(CameraProvider* cameraProvider);
        void setStageProvider(StageProvider* stageProvider);
        void setConfigurationProvider(ConfigurationProvider* configurationProvider);
        void setAcquisitionHardware(AcquisitionHardware* hardware);
        bool start(const QList<AcquisitionEvent>& events, bool block = false);
        void requestCancel();
        void cancelAndWait();

    signals:
        void eventFinished(const scopeone::core::internal::MDAOutput& output);
        void sequenceFinished();
        void sequenceCanceled();
        void sequenceError(const QString& message);

    private:
        // What the hardware was last set to during one sequence, so that unchanged
        // settings are not applied again
        struct SequenceState
        {
            // The channel preset that is applied
            QString channelGroup;
            QString channelPreset;
            bool channelChanged{false};
            // Exposure, stage positions and focus offset as last sent to the hardware
            bool exposureKnown{false};
            double exposureMs{0.0};
            QStringList exposureCameras;
            bool xyKnown{false};
            double x{0.0};
            double y{0.0};
            bool zKnown{false};
            double z{0.0};
            double zOffset{0.0};
            // State before the sequence, which is restored afterwards
            bool hasInitialXY{false};
            double initialX{0.0};
            double initialY{0.0};
            bool hasInitialZ{false};
            double initialZ{0.0};
            // Focus position that channel offsets are measured from. It follows the autofocus,
            // while initialZ stays where the stage is put back to.
            double zReference{0.0};
            // Distance the autofocus moved the focus at each position
            QHash<int, double> focusShift;
            QString initialChannelGroup;
            QString initialChannelPreset;
            // The shutter is held open between events, which needs the automatic shutter off
            bool holdShutter{false};
            bool shutterOpen{false};
            // Whether the first hardware sequence of the run was reported
            bool reportedSequence{false};
        };

        bool runAutofocus(const AcquisitionEvent& event, SequenceState& state, QString* errorMessage);
        // Number of events from the given one on that make one hardware sequence of Z slices,
        // or 1 when the event is taken on its own
        int hardwareSequenceLength(const QList<AcquisitionEvent>& events, int first) const;
        bool execHardwareSequence(const QList<AcquisitionEvent>& events,
                                  int first,
                                  int count,
                                  SequenceState& state,
                                  QList<MDAOutput>& outputs,
                                  QString* errorMessage);
        // Same for the channels of one Z slice, when the properties that differ between the
        // presets of the channels can step through their values by themselves. The values
        // of those properties on each trigger are returned in sequences.
        int channelSequenceLength(const QList<AcquisitionEvent>& events,
                                  int first,
                                  std::vector<PropertySequence>& sequences) const;
        bool execChannelSequence(const QList<AcquisitionEvent>& events,
                                 int first,
                                 int count,
                                 const std::vector<PropertySequence>& sequences,
                                 SequenceState& state,
                                 QList<MDAOutput>& outputs,
                                 QString* errorMessage);
        // Takes the images of the events in one camera sequence while armed hardware
        // advances, and turns them into one output per event
        bool captureSequencedBlock(const QList<AcquisitionEvent>& events,
                                   int first,
                                   int count,
                                   SequenceState& state,
                                   QList<MDAOutput>& outputs,
                                   const std::function<bool(QString*)>& arm,
                                   const std::function<void()>& disarm,
                                   QString* errorMessage);
        bool openShutter(const AcquisitionEvent& event, SequenceState& state, QString* errorMessage);
        void closeShutter(const AcquisitionEvent& event, SequenceState& state);

        void captureInitialState(const QList<AcquisitionEvent>& events, SequenceState& state);
        void restoreInitialState(const SequenceState& state);
        bool setupEvent(const AcquisitionEvent& event, SequenceState& state, QString* errorMessage);
        bool applyChannel(const AcquisitionEvent& event, SequenceState& state, QString* errorMessage);
        bool applyZOffset(const AcquisitionEvent& event, SequenceState& state, QString* errorMessage);
        bool execEvent(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage);
        bool captureCameras(const AcquisitionEvent& event,
                            MDAOutput& output,
                            QString* errorMessage);
        bool setExposure(const QStringList& cameraIds,
                         double exposureMs,
                         QString* errorMessage);
        bool moveXY(double x, double y, QString* errorMessage);
        bool moveZ(double z, QString* errorMessage);
        void runSequence(QList<AcquisitionEvent> events);

        CameraProvider* m_cameraProvider{nullptr};
        StageProvider* m_stageProvider{nullptr};
        ConfigurationProvider* m_configurationProvider{nullptr};
        AcquisitionHardware* m_acquisitionHardware{nullptr};
        QThreadPool m_threadPool;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
    };
}

Q_DECLARE_METATYPE(scopeone::core::internal::MDAOutput)
