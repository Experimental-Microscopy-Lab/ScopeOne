#pragma once

#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>
#include <atomic>
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
        bool start(const QList<AcquisitionEvent>& events, bool block = false);
        void requestCancel();
        void cancelAndWait();

    signals:
        void eventFinished(const scopeone::core::internal::MDAOutput& output);
        void sequenceFinished();
        void sequenceCanceled();
        void sequenceError(const QString& message);

    private:
        bool setupEvent(const AcquisitionEvent& event, QString* errorMessage);
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
        QThreadPool m_threadPool;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
    };
}

Q_DECLARE_METATYPE(scopeone::core::internal::MDAOutput)
