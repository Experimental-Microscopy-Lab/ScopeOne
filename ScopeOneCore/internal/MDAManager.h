#pragma once

#include <QHash>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>
#include <atomic>
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
        };

        bool runAutofocus(const AcquisitionEvent& event, SequenceState& state, QString* errorMessage);
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
