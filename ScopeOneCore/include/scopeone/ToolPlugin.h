#pragma once

#include "scopeone/DaqDevice.h"
#include "scopeone/ImageFrame.h"
#include "scopeone/RecordingSession.h"
#include "scopeone/SignalSource.h"
#include "scopeone/scopeone_sdk_export.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtPlugin>
#include <memory>

class QWidget;

namespace scopeone::core
{
    class ImageSceneModel;
    class ScopeOneCore;
}

namespace scopeone::ui
{
    enum class ToolWindowMode
    {
        Modal,
        ModelessSingleton
    };

    struct ToolDescriptor
    {
        QString id;
        QString name;
        QString category;
        ToolWindowMode windowMode{ToolWindowMode::ModelessSingleton};
        bool requiresCamera{false};
    };

    // The part of the Core a tool works with: layers and their images, signal sources and DAQ
    // devices. Tools see only this, so the rest of the Core can change without breaking them.
    class SCOPEONE_SDK_EXPORT ScopeOneToolCore final : public QObject
    {
        Q_OBJECT

    public:
        explicit ScopeOneToolCore(scopeone::core::ScopeOneCore& core, QObject* parent = nullptr);

        static QString rawLayerKey(const QString& cameraId);
        static QString toolLayerKey(const QString& sourceId);
        static QString staticLayerKey(const QString& sourceId);
        static QString sourceIdFromLayerKey(const QString& layerKey);

        QStringList cameraIds() const;
        scopeone::core::ImageFrame graphFrame(const QString& layerKey) const;
        scopeone::core::ImageSceneModel* imageSceneModel() const;
        scopeone::core::ImageFrame publishToolStreamFrame(const QString& sourceId,
                                                          const scopeone::core::ImageFrame& frame,
                                                          const QString& displayName = QString());

        QList<scopeone::core::SignalSourceDescriptor> signalSources() const;
        bool startSignalTrace(const scopeone::core::SignalAcquisitionConfig& config,
                              QString* errorMessage = nullptr);
        void stopSignalTrace(const QString& sourceId);
        scopeone::core::SignalSourceState signalSourceState(const QString& sourceId) const;
        QString signalSourceStateMessage(const QString& sourceId) const;

        QList<scopeone::core::DaqDeviceDescriptor> daqDevices() const;
        bool startDaqSession(const scopeone::core::DaqSessionConfig& config,
                             QString* errorMessage = nullptr);
        void stopDaqSession(const QString& deviceId);
        QString daqStateMessage(const QString& deviceId) const;

    signals:
        void previewRawFrameReady(const scopeone::core::ImageFrame& frame);
        void staticFramePublished(const QString& sourceId,
                                  const QString& displayName,
                                  const scopeone::core::ImageFrame& frame);
        void signalTimeSeriesReady(const scopeone::core::TimeSeriesChunk& chunk);
        void signalSourceStateChanged(const QString& sourceId,
                                      scopeone::core::SignalSourceState state,
                                      const QString& message);
        void signalSourceError(const QString& sourceId, const QString& errorMessage);
        void scanImageSessionReady(
            const std::shared_ptr<scopeone::core::RecordingSessionData>& session);
        void daqStateChanged(const QString& deviceId,
                             scopeone::core::DaqState state,
                             const QString& message);
        void daqError(const QString& deviceId, const QString& errorMessage);
        void daqInputDataReady(const scopeone::core::DaqInputChunk& chunk);

    private:
        scopeone::core::ScopeOneCore& m_core;
    };

    class ScopeOneToolContext
    {
    public:
        virtual ~ScopeOneToolContext() = default;

        virtual ScopeOneToolCore& core() const = 0;
        virtual QString currentLayerKey() const = 0;
        virtual void showLayers(const QStringList& layerKeys, bool sideBySide = false) = 0;
        virtual void showToolStatus(const QString& message, int timeoutMs = 5000) = 0;
        virtual void presentSession(
            const std::shared_ptr<scopeone::core::RecordingSessionData>& session,
            const QString& title) = 0;
    };

    class ScopeOneToolPlugin
    {
    public:
        virtual ~ScopeOneToolPlugin() = default;

        virtual QList<ToolDescriptor> tools() const = 0;
        virtual QWidget* createTool(const QString& toolId,
                                    ScopeOneToolContext& context,
                                    QWidget* parent) = 0;
    };
}

#define ScopeOneToolPlugin_iid "org.scopeone.ToolPlugin/3.0"
Q_DECLARE_INTERFACE(scopeone::ui::ScopeOneToolPlugin, ScopeOneToolPlugin_iid)
