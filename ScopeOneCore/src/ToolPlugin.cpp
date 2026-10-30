#include "scopeone/ToolPlugin.h"
#include "scopeone/ScopeOneCore.h"

namespace scopeone::ui
{
    using scopeone::core::ScopeOneCore;

    ScopeOneToolCore::ScopeOneToolCore(ScopeOneCore& core, QObject* parent)
        : QObject(parent), m_core(core)
    {
        connect(&m_core, &ScopeOneCore::previewRawFrameReady,
                this, &ScopeOneToolCore::previewRawFrameReady);
        connect(&m_core, &ScopeOneCore::staticFramePublished,
                this, &ScopeOneToolCore::staticFramePublished);
        connect(&m_core, &ScopeOneCore::signalTimeSeriesReady,
                this, &ScopeOneToolCore::signalTimeSeriesReady);
        connect(&m_core, &ScopeOneCore::signalSourceStateChanged,
                this, &ScopeOneToolCore::signalSourceStateChanged);
        connect(&m_core, &ScopeOneCore::signalSourceError,
                this, &ScopeOneToolCore::signalSourceError);
        connect(&m_core, &ScopeOneCore::scanImageSessionReady,
                this, &ScopeOneToolCore::scanImageSessionReady);
        connect(&m_core, &ScopeOneCore::daqStateChanged,
                this, &ScopeOneToolCore::daqStateChanged);
        connect(&m_core, &ScopeOneCore::daqError,
                this, &ScopeOneToolCore::daqError);
        connect(&m_core, &ScopeOneCore::daqInputDataReady,
                this, &ScopeOneToolCore::daqInputDataReady);
    }

    QString ScopeOneToolCore::rawLayerKey(const QString& cameraId)
    {
        return ScopeOneCore::rawLayerKey(cameraId);
    }

    QString ScopeOneToolCore::toolLayerKey(const QString& sourceId)
    {
        return ScopeOneCore::toolLayerKey(sourceId);
    }

    QString ScopeOneToolCore::staticLayerKey(const QString& sourceId)
    {
        return ScopeOneCore::staticLayerKey(sourceId);
    }

    QString ScopeOneToolCore::sourceIdFromLayerKey(const QString& layerKey)
    {
        return ScopeOneCore::sourceIdFromLayerKey(layerKey);
    }

    QStringList ScopeOneToolCore::cameraIds() const
    {
        return m_core.cameraIds();
    }

    scopeone::core::ImageFrame ScopeOneToolCore::graphFrame(const QString& layerKey) const
    {
        return m_core.graphFrame(layerKey);
    }

    scopeone::core::ImageSceneModel* ScopeOneToolCore::imageSceneModel() const
    {
        return m_core.imageSceneModel();
    }

    scopeone::core::ImageFrame ScopeOneToolCore::publishToolStreamFrame(const QString& sourceId,
                                                                        const scopeone::core::ImageFrame& frame,
                                                                        const QString& displayName)
    {
        return m_core.publishToolStreamFrame(sourceId, frame, displayName);
    }

    QList<scopeone::core::SignalSourceDescriptor> ScopeOneToolCore::signalSources() const
    {
        return m_core.signalSources();
    }

    bool ScopeOneToolCore::startSignalTrace(const scopeone::core::SignalAcquisitionConfig& config,
                                            QString* errorMessage)
    {
        return m_core.startSignalTrace(config, errorMessage);
    }

    void ScopeOneToolCore::stopSignalTrace(const QString& sourceId)
    {
        m_core.stopSignalTrace(sourceId);
    }

    scopeone::core::SignalSourceState ScopeOneToolCore::signalSourceState(const QString& sourceId) const
    {
        return m_core.signalSourceState(sourceId);
    }

    QString ScopeOneToolCore::signalSourceStateMessage(const QString& sourceId) const
    {
        return m_core.signalSourceStateMessage(sourceId);
    }

    QList<scopeone::core::DaqDeviceDescriptor> ScopeOneToolCore::daqDevices() const
    {
        return m_core.daqDevices();
    }

    bool ScopeOneToolCore::startDaqSession(const scopeone::core::DaqSessionConfig& config,
                                           QString* errorMessage)
    {
        return m_core.startDaqSession(config, errorMessage);
    }

    void ScopeOneToolCore::stopDaqSession(const QString& deviceId)
    {
        m_core.stopDaqSession(deviceId);
    }

    QString ScopeOneToolCore::daqStateMessage(const QString& deviceId) const
    {
        return m_core.daqStateMessage(deviceId);
    }
}
