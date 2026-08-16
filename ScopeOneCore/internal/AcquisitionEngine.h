#pragma once

#include <QObject>
#include <QString>

#include "scopeone/CameraProvider.h"

namespace scopeone::core::internal
{
    class DeviceRegistry;

    class AcquisitionEngine : public QObject
    {
        Q_OBJECT

    public:
        enum class State
        {
            Idle,
            Prepared,
            Running
        };

        AcquisitionEngine(DeviceRegistry* deviceRegistry, QObject* parent = nullptr);

        void prepare();
        void reset();
        bool start(const QString& cameraIdOrAll);
        bool stop(const QString& cameraIdOrAll);
        State state() const { return m_state; }

    private:
        DeviceRegistry* m_deviceRegistry{nullptr};
        State m_state{State::Idle};
    };
}
