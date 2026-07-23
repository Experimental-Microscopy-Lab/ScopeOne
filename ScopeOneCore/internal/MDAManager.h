#pragma once

#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QtGlobal>
#include <atomic>
#include <memory>

#include "scopeone/ExperimentDocument.h"

class CMMCore;

namespace scopeone::core::internal
{
    class CameraManager;

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
        explicit MDAManager(std::shared_ptr<CMMCore> core, QObject* parent = nullptr);
        ~MDAManager() override;

        bool isRunning() const { return m_running.load(); }

        void setCameraManager(CameraManager* cameraManager);
        bool start(const QList<AcquisitionEvent>& events, bool block = false);
        void requestCancel();

    signals:
        void eventFinished(const scopeone::core::internal::MDAOutput& output);
        void sequenceFinished();
        void sequenceCanceled();
        void sequenceError(const QString& message);

    private:
        bool setupEvent(const AcquisitionEvent& event, QString* errorMessage);
        bool execEvent(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage);
        bool execEventSingleCamera(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage);
        bool execEventMultiCamera(const AcquisitionEvent& event, MDAOutput& output, QString* errorMessage);
        bool setExposure(double exposureMs, QString* errorMessage);
        bool moveXY(double x, double y, QString* errorMessage);
        bool moveZ(double z, QString* errorMessage);
        void runSequence(QList<AcquisitionEvent> events);

        std::shared_ptr<CMMCore> m_mmcore;
        CameraManager* m_cameraManager{nullptr};
        QThreadPool m_threadPool;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_cancelRequested{false};
    };
}

Q_DECLARE_METATYPE(scopeone::core::internal::MDAOutput)
