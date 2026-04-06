#include "scopeone/ScopeOneCore.h"

#include "internal/ImageProcessingFramework.h"
#include "internal/MMCoreManager.h"
#include "internal/MultiProcessCameraManager.h"
#include "internal/RecordingManager.h"
#include "MMCore.h"
#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kHistogramBinCount = 256;
    constexpr double kHistogramAutoStretchOutlierPercent = 1.0;
    constexpr qint64 kHistogramRefreshIntervalMs = 250;

    int histogramBinForValue(int value, int maxValue)
    {
        if (maxValue <= 0)
        {
            return 0;
        }
        const qint64 scaled = static_cast<qint64>(value) * (kHistogramBinCount - 1);
        return qBound(0, static_cast<int>(scaled / maxValue), kHistogramBinCount - 1);
    }

    int histogramBinLowerValue(int binIndex, int maxValue)
    {
        if (maxValue <= 0 || binIndex <= 0)
        {
            return 0;
        }
        const qint64 numerator = static_cast<qint64>(binIndex) * (maxValue + 1);
        return qBound(0, static_cast<int>(numerator / kHistogramBinCount), maxValue);
    }

    int histogramBinUpperValue(int binIndex, int maxValue)
    {
        if (maxValue <= 0)
        {
            return 0;
        }
        const qint64 numerator = static_cast<qint64>(binIndex + 1) * (maxValue + 1);
        return qBound(0, static_cast<int>((numerator - 1) / kHistogramBinCount), maxValue);
    }

    void computeAutoLevels(scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        stats.autoMinLevel = 0;
        stats.autoMaxLevel = stats.maxValue > 0 ? stats.maxValue : 255;
        if (stats.totalPixels <= 0 || stats.histogram.empty())
        {
            return;
        }

        const int outlierPixels = static_cast<int>(
            stats.totalPixels * kHistogramAutoStretchOutlierPercent / 100.0);

        int cumulative = 0;
        for (int i = 0; i < static_cast<int>(stats.histogram.size()); ++i)
        {
            cumulative += stats.histogram[static_cast<size_t>(i)];
            if (cumulative >= outlierPixels)
            {
                stats.autoMinLevel = histogramBinLowerValue(i, stats.maxValue);
                break;
            }
        }

        cumulative = 0;
        for (int i = static_cast<int>(stats.histogram.size()) - 1; i >= 0; --i)
        {
            cumulative += stats.histogram[static_cast<size_t>(i)];
            if (cumulative >= outlierPixels)
            {
                stats.autoMaxLevel = histogramBinUpperValue(i, stats.maxValue);
                break;
            }
        }

        if (stats.autoMinLevel >= stats.autoMaxLevel)
        {
            stats.autoMinLevel = 0;
            stats.autoMaxLevel = stats.maxValue > 0 ? stats.maxValue : 255;
        }
    }

    QStringList toQStringList(const std::vector<std::string>& values)
    {
        QStringList out;
        out.reserve(static_cast<int>(values.size()));
        for (const auto& value : values)
        {
            out.append(QString::fromStdString(value));
        }
        return out;
    }

    QString histogramStreamKey(const QString& cameraId, bool processed)
    {
        return QStringLiteral("%1:%2")
            .arg(processed ? QStringLiteral("proc") : QStringLiteral("raw"),
                 cameraId.trimmed());
    }

    QString recordingMetadataFileName(const QString& baseName)
    {
        const QString trimmedBaseName = baseName.trimmed();
        if (trimmedBaseName.isEmpty())
        {
            return QStringLiteral("recording_metadata.json");
        }
        return trimmedBaseName + QStringLiteral("_metadata.json");
    }

    bool computeHistogramStatsInternal(const scopeone::core::ImageFrame& frame,
                                       scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        if (!frame.isValid())
        {
            return false;
        }

        const int totalPixels = frame.width * frame.height;
        if (totalPixels <= 0)
        {
            return false;
        }

        const bool mono16 = frame.isMono16();
        const bool mono8 = frame.isMono8();
        if (!mono16 && !mono8)
        {
            return false;
        }

        const int bytesPerPixel = mono16 ? 2 : 1;
        const qint64 requiredBytes = static_cast<qint64>(frame.stride) * frame.height;
        if (frame.bytes.size() < requiredBytes || frame.stride < frame.width * bytesPerPixel)
        {
            return false;
        }

        stats = scopeone::core::ScopeOneCore::HistogramStats{};
        stats.totalPixels = totalPixels;
        stats.bitDepth = frame.bitsPerSample;
        if (stats.bitDepth <= 0)
        {
            stats.bitDepth = mono16 ? 16 : 8;
        }
        if (stats.bitDepth > 16)
        {
            stats.bitDepth = 16;
        }
        stats.maxValue = (1 << stats.bitDepth) - 1;
        if (stats.maxValue <= 0)
        {
            stats.maxValue = mono16 ? 65535 : 255;
        }
        stats.histogram.assign(kHistogramBinCount, 0);

        stats.minVal = static_cast<double>(stats.maxValue);
        stats.maxVal = 0.0;

        const uchar* bytes = reinterpret_cast<const uchar*>(frame.bytes.constData());
        double sum = 0.0;
        double sumSq = 0.0;

        if (mono16)
        {
            for (int y = 0; y < frame.height; ++y)
            {
                const quint16* row = reinterpret_cast<const quint16*>(bytes + y * frame.stride);
                for (int x = 0; x < frame.width; ++x)
                {
                    const int value = static_cast<int>(row[x]);
                    sum += value;
                    sumSq += static_cast<double>(value) * value;
                    stats.minVal = (std::min)(stats.minVal, static_cast<double>(value));
                    stats.maxVal = (std::max)(stats.maxVal, static_cast<double>(value));
                    stats.histogram[static_cast<size_t>(histogramBinForValue(value, stats.maxValue))] += 1;
                }
            }
        }
        else
        {
            stats.bitDepth = 8;
            stats.maxValue = 255;
            stats.histogram.assign(kHistogramBinCount, 0);
            for (int y = 0; y < frame.height; ++y)
            {
                const uchar* row = bytes + y * frame.stride;
                for (int x = 0; x < frame.width; ++x)
                {
                    const int value = static_cast<int>(row[x]);
                    sum += value;
                    sumSq += static_cast<double>(value) * value;
                    stats.minVal = (std::min)(stats.minVal, static_cast<double>(value));
                    stats.maxVal = (std::max)(stats.maxVal, static_cast<double>(value));
                    stats.histogram[static_cast<size_t>(histogramBinForValue(value, stats.maxValue))] += 1;
                }
            }
        }

        stats.mean = sum / (std::max)(1, totalPixels);
        const double variance = (sumSq / (std::max)(1, totalPixels)) - (stats.mean * stats.mean);
        stats.stdDev = std::sqrt((std::max)(0.0, variance));
        computeAutoLevels(stats);
        return true;
    }

    bool sampleFrameValue(const scopeone::core::ImageFrame& frame,
                          const QPoint& point,
                          int& value)
    {
        if (!frame.isValid()
            || point.x() < 0 || point.y() < 0
            || point.x() >= frame.width || point.y() >= frame.height)
        {
            return false;
        }

        const char* rowData = frame.bytes.constData() + frame.stride * point.y();
        if (frame.isMono8())
        {
            const uchar* row = reinterpret_cast<const uchar*>(rowData);
            value = static_cast<int>(row[point.x()]);
            return true;
        }
        if (frame.isMono16())
        {
            const quint16* row = reinterpret_cast<const quint16*>(rowData);
            value = static_cast<int>(row[point.x()]);
            return true;
        }
        return false;
    }

    template <typename Sampler>
    bool sampleLine(const QPoint& start, const QPoint& end, QVector<int>& values, Sampler&& sampler)
    {
        const int dx = end.x() - start.x();
        const int dy = end.y() - start.y();
        const int steps = qMax(qAbs(dx), qAbs(dy));
        values.clear();
        values.reserve(steps + 1);
        for (int i = 0; i <= steps; ++i)
        {
            const double t = (steps == 0) ? 0.0 : static_cast<double>(i) / static_cast<double>(steps);
            const QPoint point(qRound(start.x() + dx * t), qRound(start.y() + dy * t));
            int value = 0;
            if (sampler(point, value))
            {
                values.push_back(value);
            }
        }
        return !values.isEmpty();
    }

    scopeone::core::ScopeOneCore::LoadConfigResult toFacadeLoadConfigResult(
        const scopeone::core::internal::MMCoreManager::LoadConfigResult& result)
    {
        scopeone::core::ScopeOneCore::LoadConfigResult facade;
        facade.cameraIds = result.cameraIds;
        facade.successCount = result.successCount;
        facade.failCount = result.failCount;
        facade.skippedCameraCount = result.skippedCameraCount;
        facade.foundCamera = result.foundCamera;
        return facade;
    }

    scopeone::core::internal::RecordingManager::Settings toRecordingManagerSettings(
        const scopeone::core::ScopeOneCore::RecordingSettings& settings)
    {
        scopeone::core::internal::RecordingManager::Settings managerSettings;
        managerSettings.format = settings.format;
        managerSettings.streamToDisk = settings.streamToDisk;
        managerSettings.enableCompression = settings.enableCompression;
        managerSettings.compressionLevel = settings.compressionLevel;
        managerSettings.framesPerBurst = settings.framesPerBurst;
        managerSettings.burstMode = settings.burstMode;
        managerSettings.targetBursts = settings.targetBursts;
        managerSettings.burstIntervalMs = settings.burstIntervalMs;
        managerSettings.mdaIntervalMs = settings.mdaIntervalMs;
        managerSettings.positions = settings.positions;
        managerSettings.zPositions = settings.zPositions;
        managerSettings.order.clear();
        managerSettings.order.reserve(settings.order.size());
        for (scopeone::core::ScopeOneCore::RecordingAxis axis : settings.order)
        {
            switch (axis)
            {
            case scopeone::core::ScopeOneCore::RecordingAxis::Time:
                managerSettings.order.push_back(scopeone::core::internal::MDAAxis::Time);
                break;
            case scopeone::core::ScopeOneCore::RecordingAxis::Z:
                managerSettings.order.push_back(scopeone::core::internal::MDAAxis::Z);
                break;
            case scopeone::core::ScopeOneCore::RecordingAxis::XY:
                managerSettings.order.push_back(scopeone::core::internal::MDAAxis::XY);
                break;
            }
        }
        if (managerSettings.order.empty())
        {
            managerSettings.order = scopeone::core::internal::MDAOrder{
                scopeone::core::internal::MDAAxis::Time,
                scopeone::core::internal::MDAAxis::Z,
                scopeone::core::internal::MDAAxis::XY
            };
        }
        managerSettings.saveDir = settings.saveDir;
        managerSettings.baseName = settings.baseName;
        managerSettings.captureAll = settings.captureAll;
        managerSettings.metadataFileName = settings.metadataFileName;
        managerSettings.sessionMetadataJson = settings.sessionMetadataJson;
        return managerSettings;
    }

    scopeone::core::ScopeOneCore::ProcessingModuleKind processingModuleKind(
        const scopeone::core::internal::ProcessingModule* module)
    {
        if (!module)
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::Unknown;
        }
        if (qobject_cast<const scopeone::core::internal::FFTModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::FFT;
        }
        if (qobject_cast<const scopeone::core::internal::MedianFilterModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::MedianFilter;
        }
        if (qobject_cast<const scopeone::core::internal::BackgroundCalibrationModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::BackgroundCalibration;
        }
        if (qobject_cast<const scopeone::core::internal::SpatiotemporalBinningModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::SpatiotemporalBinning;
        }
        if (qobject_cast<const scopeone::core::internal::GaussianBlurModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::GaussianBlur;
        }
        return scopeone::core::ScopeOneCore::ProcessingModuleKind::Unknown;
    }

    scopeone::core::internal::ProcessingPipeline* processingPipeline(
        scopeone::core::internal::ImageProcessingManager* manager)
    {
        if (!manager)
        {
            return nullptr;
        }
        return manager->pipeline();
    }

    QByteArray buildDevicePropertyMetadataJson(const scopeone::core::ScopeOneCore& core)
    {
        QStringList deviceLabels = core.loadedDevices();
        deviceLabels.removeDuplicates();
        deviceLabels.sort(Qt::CaseInsensitive);

        QJsonObject devicePropertiesObject;
        for (const QString& deviceLabel : deviceLabels)
        {
            const QString trimmedDeviceLabel = deviceLabel.trimmed();
            if (trimmedDeviceLabel.isEmpty())
            {
                continue;
            }

            QJsonObject propertyValuesObject;
            const QList<scopeone::core::ScopeOneCore::DevicePropertyInfo> properties =
                core.deviceProperties(trimmedDeviceLabel, false);
            for (const auto& property : properties)
            {
                const QString propertyName = property.name().trimmed();
                if (propertyName.isEmpty())
                {
                    continue;
                }
                propertyValuesObject.insert(propertyName, property.value());
            }

            devicePropertiesObject.insert(trimmedDeviceLabel, propertyValuesObject);
        }

        QJsonObject rootObject;
        rootObject.insert(QStringLiteral("device_properties"), devicePropertiesObject);
        return QJsonDocument(rootObject).toJson(QJsonDocument::Indented);
    }
}

namespace scopeone::core
{
    using scopeone::core::internal::BackgroundCalibrationModule;
    using scopeone::core::internal::FFTModule;
    using scopeone::core::internal::GaussianBlurModule;
    using scopeone::core::internal::ImageFrame;
    using scopeone::core::internal::ImageProcessingManager;
    using scopeone::core::internal::MMCoreManager;
    using scopeone::core::internal::MedianFilterModule;
    using scopeone::core::internal::MultiProcessCameraManager;
    using scopeone::core::internal::ProcessingModule;
    using scopeone::core::internal::ProcessingPipeline;
    using scopeone::core::internal::RecordingManager;
    using scopeone::core::internal::SpatiotemporalBinningModule;

    struct ScopeOneCore::Managers
    {
        MMCoreManager* mmcoreManager{nullptr};
        MultiProcessCameraManager* mpcm{nullptr};
        RecordingManager* recordingManager{nullptr};
        ImageProcessingManager* imageProcessingManager{nullptr};
    };

    QString ScopeOneCore::getVersion()
    {
        return QStringLiteral(SCOPEONE_CORE_VERSION_STRING);
    }

    ScopeOneCore::ScopeOneCore(QObject* parent)
        : QObject(parent)
          , m_managers(std::make_unique<Managers>())
    {
        qRegisterMetaType<std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>>(
            "std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>");
        qRegisterMetaType<scopeone::core::ScopeOneCore::RecordingWriterStatus>(
            "scopeone::core::ScopeOneCore::RecordingWriterStatus");
        qRegisterMetaType<scopeone::core::ScopeOneCore::HistogramStats>(
            "scopeone::core::ScopeOneCore::HistogramStats");
        qRegisterMetaType<scopeone::core::SharedFrameHeader>("scopeone::core::SharedFrameHeader");
        qRegisterMetaType<scopeone::core::ImageFrame>("scopeone::core::ImageFrame");
        m_managers->mmcoreManager = new MMCoreManager(this);
        m_managers->mpcm = new MultiProcessCameraManager(this);
        m_managers->recordingManager = new RecordingManager(this);
        m_managers->imageProcessingManager = new ImageProcessingManager(this);
        m_managers->recordingManager->setMultiProcessCameraManager(m_managers->mpcm);
        m_managers->recordingManager->setMMCore(m_managers->mmcoreManager->getCore());
        m_managers->recordingManager->setLatestFrameFetcher(
            [this](const QString& cameraId, SharedFrameHeader& header, QByteArray& data)
            {
                return getLatestRawTransport(cameraId, header, data);
            });

        connect(m_managers->mpcm, &MultiProcessCameraManager::newRawFrameReady,
                this, [this](const QString& cameraId,
                             const SharedFrameHeader& header,
                             const QByteArray& rawData)
                {
                    handleIncomingRawFrame(ImageFrame::fromSharedFrame(cameraId, header, rawData), header);
                });
        connect(m_managers->mpcm, &MultiProcessCameraManager::previewStateChanged,
                this, &ScopeOneCore::previewStateChanged);
        connect(m_managers->mpcm, &MultiProcessCameraManager::agentControlServerListening,
                this, &ScopeOneCore::agentControlServerListening);

        connect(m_managers->mpcm, &MultiProcessCameraManager::newRawFrameReady,
                m_managers->recordingManager, &RecordingManager::onNewRawFrameReady,
                Qt::QueuedConnection);
        connect(m_managers->recordingManager, &RecordingManager::mdaRawFrameReady,
                this, &ScopeOneCore::handleIncomingRawFrame, Qt::QueuedConnection);

        connect(m_managers->recordingManager, &RecordingManager::progressChanged,
                this, &ScopeOneCore::recordingProgressChanged);
        connect(m_managers->recordingManager, &RecordingManager::writerStatusChanged,
                this, &ScopeOneCore::recordingWriterStatusChanged);
        connect(m_managers->recordingManager, &RecordingManager::recordingStateChanged,
                this, &ScopeOneCore::recordingStateChanged);
        connect(m_managers->recordingManager, &RecordingManager::recordingStopped,
                this, &ScopeOneCore::recordingStopped);

        connect(m_managers->imageProcessingManager, &ImageProcessingManager::imageProcessed,
                this, [this](const ImageFrame& frame)
                {
                    if (!frame.isValid())
                    {
                        return;
                    }
                    m_latestProcessedFrames.insert(frame.cameraId, frame);
                    emit processedFrameReady(frame.cameraId, frame);
                    scheduleHistogramStats(frame.cameraId, true, frame);
                    updateLineProfile(frame.cameraId, true, frame);
                });
        connect(m_managers->imageProcessingManager, &ImageProcessingManager::processingError,
                this, &ScopeOneCore::processingError);
    }

    ScopeOneCore::~ScopeOneCore()
    {
        unloadConfiguration();
    }

    std::shared_ptr<CMMCore> ScopeOneCore::core() const
    {
        if (!m_managers || !m_managers->mmcoreManager)
        {
            return {};
        }
        return m_managers->mmcoreManager->getCore();
    }

    bool ScopeOneCore::hasCore() const
    {
        return core() != nullptr;
    }

    bool ScopeOneCore::isAgentCamera(const QString& deviceLabel) const
    {
        return m_cameraIds.contains(deviceLabel);
    }

    bool ScopeOneCore::loadConfigurationInternal(const QString& configPath,
                                                 const QStringList& existingCameraIds,
                                                 LoadConfigResult* result,
                                                 QString* errorMessage)
    {
        // Keep manager state aligned with loaded cameras
        if (!m_managers || !m_managers->mmcoreManager || !m_managers->mpcm)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("ScopeOneCore is not initialized");
            }
            return false;
        }
        MMCoreManager::LoadConfigResult mmResult;
        if (!m_managers->mmcoreManager->loadConfigurationAndStartCameras(
            configPath, m_managers->mpcm, existingCameraIds, &mmResult, errorMessage))
        {
            return false;
        }
        const LoadConfigResult facadeResult = toFacadeLoadConfigResult(mmResult);
        if (result)
        {
            *result = facadeResult;
        }
        m_cameraIds = facadeResult.cameraIds;
        return true;
    }

    bool ScopeOneCore::loadConfiguration(const QString& configPath,
                                         LoadConfigResult* result,
                                         QString* errorMessage)
    {
        return loadConfigurationInternal(configPath, m_cameraIds, result, errorMessage);
    }

    void ScopeOneCore::unloadConfiguration()
    {
        if (m_managers && m_managers->mpcm)
        {
            m_managers->mpcm->stopPreview();
            m_managers->mpcm->stopAgents();
        }
        auto handle = core();
        if (handle)
        {
            try
            {
                handle->unloadAllDevices();
            }
            catch (const CMMError&)
            {
            }
        }
        m_cameraIds.clear();
        m_latestRawHeaders.clear();
        m_latestRawFrames.clear();
        m_latestProcessedFrames.clear();
        m_histogramJobStates.clear();
        m_latestHistogramStats.clear();
        clearLineProfile();
    }

    void ScopeOneCore::startPreview(const QString& cameraIdOrAll)
    {
        // Route preview to one camera or all cameras
        if (!m_managers || !m_managers->mpcm)
        {
            return;
        }
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return;
        }
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            m_managers->mpcm->startPreview();
        }
        else
        {
            m_managers->mpcm->startPreviewFor(target);
        }
    }

    void ScopeOneCore::stopPreview(const QString& cameraIdOrAll)
    {
        if (!m_managers || !m_managers->mpcm)
        {
            return;
        }
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return;
        }
        if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            m_managers->mpcm->stopPreview();
        }
        else
        {
            m_managers->mpcm->stopPreviewFor(target);
        }
    }

    bool ScopeOneCore::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        if (!m_managers || !m_managers->mpcm)
        {
            return false;
        }
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->setExposure(target, exposureMs);
    }

    bool ScopeOneCore::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        if (!m_managers || !m_managers->mpcm || cameraId.trimmed().isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->setROI(cameraId, x, y, width, height);
    }

    bool ScopeOneCore::clearROI(const QString& cameraId)
    {
        if (!m_managers || !m_managers->mpcm || cameraId.trimmed().isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->clearROI(cameraId);
    }

    void ScopeOneCore::setLineProfile(const QString& cameraId,
                                      const QPoint& start,
                                      const QPoint& end,
                                      bool processed)
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty())
        {
            clearLineProfile();
            return;
        }

        m_activeLineProfile.cameraId = trimmedCameraId;
        m_activeLineProfile.start = start;
        m_activeLineProfile.end = end;
        m_activeLineProfile.processed = processed;
        m_activeLineProfile.active = true;

        if (processed)
        {
            const ImageFrame frame = m_latestProcessedFrames.value(trimmedCameraId);
            if (frame.isValid())
            {
                updateLineProfile(trimmedCameraId, true, frame);
            }
            return;
        }

        ImageFrame frame;
        if (getLatestRawFrame(trimmedCameraId, frame))
        {
            updateLineProfile(trimmedCameraId, false, frame);
        }
    }

    void ScopeOneCore::clearLineProfile()
    {
        if (!m_activeLineProfile.active)
        {
            return;
        }
        m_activeLineProfile = ActiveLineProfile{};
        emit lineProfileCleared();
    }

    bool ScopeOneCore::getLatestRawTransport(const QString& cameraId,
                                             SharedFrameHeader& header,
                                             QByteArray& data) const
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty())
        {
            return false;
        }

        const auto cachedFrame = m_latestRawFrames.constFind(trimmedCameraId);
        const auto cachedHeader = m_latestRawHeaders.constFind(trimmedCameraId);
        if (cachedFrame != m_latestRawFrames.constEnd()
            && cachedHeader != m_latestRawHeaders.constEnd()
            && cachedFrame.value().isValid())
        {
            header = cachedHeader.value();
            data = cachedFrame.value().bytes;
            return true;
        }

        if (!m_managers || !m_managers->mpcm)
        {
            return false;
        }
        return m_managers->mpcm->getLatestRaw(trimmedCameraId, header, data);
    }

    void ScopeOneCore::handleIncomingRawFrame(const ImageFrame& frame,
                                              const SharedFrameHeader& header)
    {
        const QString cameraId = frame.cameraId.trimmed();
        if (!frame.isValid() || cameraId.isEmpty())
        {
            return;
        }
        if (!m_cameraIds.contains(cameraId))
        {
            return;
        }

        ImageFrame normalizedFrame(frame);
        normalizedFrame.cameraId = cameraId;
        m_latestRawHeaders.insert(cameraId, header);
        m_latestRawFrames.insert(cameraId, normalizedFrame);
        emit newRawFrameReady(normalizedFrame);
        scheduleHistogramStats(cameraId, false, normalizedFrame);
        updateLineProfile(cameraId, false, normalizedFrame);

        if (isRealTimeProcessingEnabled())
        {
            processFrameAsync(normalizedFrame);
        }
    }

    bool ScopeOneCore::getLatestRawFrame(const QString& cameraId, ImageFrame& frame) const
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty())
        {
            frame = ImageFrame{};
            return false;
        }

        SharedFrameHeader header{};
        QByteArray data;
        if (!getLatestRawTransport(trimmedCameraId, header, data))
        {
            frame = ImageFrame{};
            return false;
        }
        frame = ImageFrame::fromSharedFrame(trimmedCameraId, header, data);
        return frame.isValid();
    }

    bool ScopeOneCore::computeHistogramStats(const ImageFrame& frame, HistogramStats& stats)
    {
        return computeHistogramStatsInternal(frame, stats);
    }

    bool ScopeOneCore::getRawImageStatistics(const QString& cameraId, HistogramStats& stats) const
    {
        const auto cached = m_latestHistogramStats.constFind(histogramStreamKey(cameraId, false));
        if (cached != m_latestHistogramStats.constEnd() && cached.value().hasData())
        {
            stats = cached.value();
            return true;
        }

        ImageFrame frame;
        if (!getLatestRawFrame(cameraId, frame))
        {
            return false;
        }
        return computeHistogramStats(frame, stats);
    }

    void ScopeOneCore::scheduleHistogramStats(const QString& cameraId,
                                              bool processed,
                                              const ImageFrame& frame)
    {
        // Throttle histogram work per stream
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty() || !frame.isValid())
        {
            return;
        }

        const QString cacheKey = histogramStreamKey(trimmedCameraId, processed);
        HistogramJobState& state = m_histogramJobStates[cacheKey];
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (state.inFlight || (state.lastScheduledMs > 0 && (nowMs - state.lastScheduledMs) <
            kHistogramRefreshIntervalMs))
        {
            return;
        }

        state.inFlight = true;
        state.lastScheduledMs = nowMs;

        auto* watcher = new QFutureWatcher<HistogramStats>(this);
        connect(watcher, &QFutureWatcher<HistogramStats>::finished, this,
                [this, watcher, trimmedCameraId, processed, cacheKey]()
                {
                    HistogramStats stats = watcher->result();
                    m_histogramJobStates[cacheKey].inFlight = false;
                    if (stats.hasData())
                    {
                        m_latestHistogramStats.insert(cacheKey, stats);
                        emit imageHistogramReady(trimmedCameraId, processed, stats);
                    }
                    watcher->deleteLater();
                });
        watcher->setFuture(QtConcurrent::run([frame]()
        {
            HistogramStats stats;
            ScopeOneCore::computeHistogramStats(frame, stats);
            return stats;
        }));
    }

    void ScopeOneCore::updateLineProfile(const QString& cameraId,
                                         bool processed,
                                         const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return;
        }

        if (!m_activeLineProfile.active
            || m_activeLineProfile.processed != processed
            || m_activeLineProfile.cameraId != cameraId)
        {
            return;
        }

        QVector<int> values;
        if (!sampleLine(m_activeLineProfile.start, m_activeLineProfile.end, values,
                        [&](const QPoint& point, int& value)
                        {
                            return sampleFrameValue(frame, point, value);
                        }))
        {
            return;
        }

        emit lineProfileUpdated(cameraId, processed, values);
    }

    QStringList ScopeOneCore::xyStageDevices() const
    {
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            return toQStringList(handle->getLoadedDevicesOfType(MM::XYStageDevice));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QStringList ScopeOneCore::zStageDevices() const
    {
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            return toQStringList(handle->getLoadedDevicesOfType(MM::StageDevice));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString ScopeOneCore::currentXYStageDevice() const
    {
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            return QString::fromStdString(handle->getXYStageDevice());
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString ScopeOneCore::currentFocusDevice() const
    {
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            return QString::fromStdString(handle->getFocusDevice());
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    bool ScopeOneCore::readXYPosition(const QString& xyStageLabel, double& x, double& y) const
    {
        x = 0.0;
        y = 0.0;
        const QString label = xyStageLabel.trimmed();
        auto handle = core();
        if (!handle || label.isEmpty())
        {
            return false;
        }
        try
        {
            handle->getXYPosition(label.toStdString().c_str(), x, y);
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    bool ScopeOneCore::readZPosition(const QString& zStageLabel, double& z) const
    {
        z = 0.0;
        const QString label = zStageLabel.trimmed();
        auto handle = core();
        if (!handle || label.isEmpty())
        {
            return false;
        }
        try
        {
            z = handle->getPosition(label.toStdString().c_str());
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    bool ScopeOneCore::moveXYRelative(const QString& xyStageLabel, double dx, double dy)
    {
        const QString label = xyStageLabel.trimmed();
        auto handle = core();
        if (!handle || label.isEmpty())
        {
            return false;
        }
        try
        {
            handle->setRelativeXYPosition(label.toStdString().c_str(), dx, dy);
            handle->waitForDevice(label.toStdString().c_str());
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    bool ScopeOneCore::moveZRelative(const QString& zStageLabel, double dz)
    {
        const QString label = zStageLabel.trimmed();
        auto handle = core();
        if (!handle || label.isEmpty())
        {
            return false;
        }
        try
        {
            handle->setRelativePosition(label.toStdString().c_str(), dz);
            handle->waitForDevice(label.toStdString().c_str());
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    bool ScopeOneCore::readExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        exposureMs = 0.0;
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }

        if (m_managers && m_managers->mpcm)
        {
            QString resolvedTarget = target;
            if (resolvedTarget.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
            {
                if (m_cameraIds.isEmpty())
                {
                    return false;
                }
                resolvedTarget = m_cameraIds.first();
            }
            if (m_cameraIds.contains(resolvedTarget) && m_managers->mpcm->getExposure(resolvedTarget, exposureMs))
            {
                return true;
            }
        }

        auto handle = core();
        if (!handle)
        {
            return false;
        }
        try
        {
            if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
            {
                exposureMs = handle->getExposure();
            }
            else
            {
                exposureMs = handle->getExposure(target.toStdString().c_str());
            }
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    QStringList ScopeOneCore::loadedDevices() const
    {
        auto handle = core();
        QStringList devices;
        if (handle)
        {
            try
            {
                devices = toQStringList(handle->getLoadedDevices());
            }
            catch (const CMMError&)
            {
                devices.clear();
            }
        }

        // Agent cameras are not always loaded in the UI-side MMCore instance
        for (const QString& cameraId : m_cameraIds)
        {
            if (!devices.contains(cameraId))
            {
                devices.append(cameraId);
            }
        }
        return devices;
    }

    QList<ScopeOneCore::DevicePropertyInfo> ScopeOneCore::deviceProperties(const QString& deviceLabel,
                                                                           bool fromCache) const
    {
        // Build one property snapshot list for the UI
        QList<DevicePropertyInfo> properties;
        const QStringList names = devicePropertyNames(deviceLabel);
        properties.reserve(names.size());
        for (const QString& name : names)
        {
            DevicePropertyInfo info;
            info.setName(name);
            info.setValue(getPropertyValue(deviceLabel, name, fromCache));
            info.setType(propertyTypeString(deviceLabel, name));
            info.setReadOnly(isPropertyReadOnly(deviceLabel, name));
            info.setPreInit(isPropertyPreInit(deviceLabel, name));
            info.setAllowedValues(getAllowedPropertyValues(deviceLabel, name));
            double lower = 0.0;
            double upper = 0.0;
            if (getPropertyLimits(deviceLabel, name, lower, upper))
            {
                info.setLimits(lower, upper);
            }
            properties.append(std::move(info));
        }
        return properties;
    }

    QStringList ScopeOneCore::devicePropertyNames(const QString& deviceLabel) const
    {
        const QString device = deviceLabel.trimmed();
        if (device.isEmpty())
        {
            return {};
        }
        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm)
            {
                return {};
            }
            return m_managers->mpcm->listProperties(device);
        }
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            return toQStringList(handle->getDevicePropertyNames(device.toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString ScopeOneCore::getPropertyValue(const QString& deviceLabel, const QString& name, bool fromCache) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty())
        {
            return {};
        }
        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm)
            {
                return {};
            }
            return m_managers->mpcm->getProperty(device, property);
        }
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            if (fromCache)
            {
                return QString::fromStdString(
                    handle->getPropertyFromCache(device.toStdString().c_str(),
                                                 property.toStdString().c_str()));
            }
            return QString::fromStdString(
                handle->getProperty(device.toStdString().c_str(), property.toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    QString ScopeOneCore::propertyTypeString(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty())
        {
            return QStringLiteral("Unknown");
        }
        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm)
            {
                return QStringLiteral("Unknown");
            }
            return m_managers->mpcm->getPropertyType(device, property);
        }
        auto handle = core();
        if (!handle)
        {
            return QStringLiteral("Unknown");
        }
        try
        {
            const MM::PropertyType type = handle->getPropertyType(device.toStdString().c_str(),
                                                                  property.toStdString().c_str());
            switch (type)
            {
            case MM::String: return QStringLiteral("String");
            case MM::Float: return QStringLiteral("Float");
            case MM::Integer: return QStringLiteral("Integer");
            default: return QStringLiteral("Unknown");
            }
        }
        catch (const CMMError&)
        {
            return QStringLiteral("Unknown");
        }
    }

    bool ScopeOneCore::isPropertyReadOnly(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty())
        {
            return true;
        }
        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm)
            {
                return true;
            }
            return m_managers->mpcm->isPropertyReadOnly(device, property);
        }
        auto handle = core();
        if (!handle)
        {
            return true;
        }
        try
        {
            return handle->isPropertyReadOnly(device.toStdString().c_str(), property.toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return true;
        }
    }

    bool ScopeOneCore::isPropertyPreInit(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty() || isAgentCamera(device))
        {
            return false;
        }
        auto handle = core();
        if (!handle)
        {
            return false;
        }
        try
        {
            return handle->isPropertyPreInit(device.toStdString().c_str(), property.toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    QStringList ScopeOneCore::getAllowedPropertyValues(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty())
        {
            return {};
        }
        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm)
            {
                return {};
            }
            return m_managers->mpcm->getAllowedPropertyValues(device, property);
        }
        auto handle = core();
        if (!handle)
        {
            return {};
        }
        try
        {
            return toQStringList(
                handle->getAllowedPropertyValues(device.toStdString().c_str(), property.toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    bool ScopeOneCore::getPropertyLimits(const QString& deviceLabel,
                                         const QString& name,
                                         double& lower,
                                         double& upper) const
    {
        lower = 0.0;
        upper = 0.0;

        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty())
        {
            return false;
        }
        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm
                || !m_managers->mpcm->hasPropertyLimits(device, property))
            {
                return false;
            }
            lower = m_managers->mpcm->getPropertyLowerLimit(device, property);
            upper = m_managers->mpcm->getPropertyUpperLimit(device, property);
            return true;
        }

        auto handle = core();
        if (!handle)
        {
            return false;
        }
        try
        {
            if (!handle->hasPropertyLimits(device.toStdString().c_str(), property.toStdString().c_str()))
            {
                return false;
            }
            lower = handle->getPropertyLowerLimit(device.toStdString().c_str(), property.toStdString().c_str());
            upper = handle->getPropertyUpperLimit(device.toStdString().c_str(), property.toStdString().c_str());
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    bool ScopeOneCore::setPropertyValue(const QString& deviceLabel,
                                        const QString& name,
                                        const QString& value,
                                        QString* errorMessage)
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Invalid property target");
            }
            return false;
        }

        if (isAgentCamera(device))
        {
            if (!m_managers || !m_managers->mpcm
                || !m_managers->mpcm->setProperty(device, property, value))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Agent setProperty failed");
                }
                return false;
            }
            return true;
        }

        auto handle = core();
        if (!handle)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("MMCore not available");
            }
            return false;
        }

        try
        {
            handle->setProperty(device.toStdString().c_str(),
                                property.toStdString().c_str(),
                                value.toStdString().c_str());
            return true;
        }
        catch (const CMMError& e)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(e.getMsg());
            }
            return false;
        }
    }

    bool ScopeOneCore::isRealTimeProcessingEnabled() const
    {
        return m_managers && m_managers->imageProcessingManager
            && m_managers->imageProcessingManager->isRealTimeProcessingEnabled();
    }

    void ScopeOneCore::setRealTimeProcessingEnabled(bool enabled)
    {
        if (!m_managers || !m_managers->imageProcessingManager)
        {
            return;
        }
        m_managers->imageProcessingManager->enableRealTimeProcessing(enabled);
    }

    void ScopeOneCore::processFrameAsync(const ImageFrame& frame)
    {
        if (!m_managers || !m_managers->imageProcessingManager || !frame.isValid())
        {
            return;
        }
        m_managers->imageProcessingManager->processFrameAsync(frame);
    }

    QList<scopeone::core::ScopeOneCore::ProcessingModuleInfo> ScopeOneCore::processingModules() const
    {
        QList<ProcessingModuleInfo> out;
        ProcessingPipeline* pipeline = processingPipeline(
            m_managers ? m_managers->imageProcessingManager : nullptr);
        if (!pipeline)
        {
            return out;
        }

        const int count = pipeline->getModuleCount();
        out.reserve(count);
        for (int i = 0; i < count; ++i)
        {
            ProcessingModule* module = pipeline->getModule(i);
            if (!module)
            {
                continue;
            }
            ProcessingModuleInfo info;
            info.setKind(processingModuleKind(module));
            info.setName(module->getModuleName());
            info.setParameters(module->getParameters());
            out.append(std::move(info));
        }
        return out;
    }

    bool ScopeOneCore::addProcessingModule(ProcessingModuleKind kind)
    {
        ProcessingPipeline* pipeline = processingPipeline(
            m_managers ? m_managers->imageProcessingManager : nullptr);
        if (!pipeline)
        {
            return false;
        }

        std::unique_ptr<ProcessingModule> module;
        switch (kind)
        {
        case ProcessingModuleKind::FFT:
            module = std::make_unique<FFTModule>(pipeline);
            break;
        case ProcessingModuleKind::MedianFilter:
            module = std::make_unique<MedianFilterModule>(pipeline);
            break;
        case ProcessingModuleKind::BackgroundCalibration:
            module = std::make_unique<BackgroundCalibrationModule>(pipeline);
            break;
        case ProcessingModuleKind::SpatiotemporalBinning:
            module = std::make_unique<SpatiotemporalBinningModule>(pipeline);
            break;
        case ProcessingModuleKind::GaussianBlur:
            module = std::make_unique<GaussianBlurModule>(pipeline);
            break;
        case ProcessingModuleKind::Unknown:
            return false;
        }

        pipeline->addModule(std::move(module));
        emit processingModulesChanged();
        return true;
    }

    bool ScopeOneCore::removeProcessingModule(int index)
    {
        ProcessingPipeline* pipeline = processingPipeline(
            m_managers ? m_managers->imageProcessingManager : nullptr);
        if (!pipeline || index < 0 || index >= pipeline->getModuleCount())
        {
            return false;
        }
        pipeline->removeModule(index);
        emit processingModulesChanged();
        return true;
    }

    bool ScopeOneCore::setProcessingModuleParameters(int index, const QVariantMap& parameters)
    {
        ProcessingPipeline* pipeline = processingPipeline(
            m_managers ? m_managers->imageProcessingManager : nullptr);
        if (!pipeline)
        {
            return false;
        }
        ProcessingModule* module = pipeline->getModule(index);
        if (!module)
        {
            return false;
        }
        module->setParameters(parameters);
        emit processingModulesChanged();
        return true;
    }

    bool ScopeOneCore::resetProcessingModuleState(int index)
    {
        ProcessingPipeline* pipeline = processingPipeline(
            m_managers ? m_managers->imageProcessingManager : nullptr);
        if (!pipeline)
        {
            return false;
        }
        ProcessingModule* module = pipeline->getModule(index);
        if (!module)
        {
            return false;
        }
        if (auto* median = qobject_cast<MedianFilterModule*>(module))
        {
            median->resetBuffer();
            emit processingModulesChanged();
            return true;
        }
        if (auto* background = qobject_cast<BackgroundCalibrationModule*>(module))
        {
            background->resetCalibration();
            emit processingModulesChanged();
            return true;
        }
        return false;
    }

    void ScopeOneCore::setRecordingMaxPendingWriteBytes(qint64 bytes)
    {
        if (m_managers && m_managers->recordingManager)
        {
            m_managers->recordingManager->setRecordedMaxBytes(bytes);
        }
    }

    qint64 ScopeOneCore::recordingMaxPendingWriteBytes() const
    {
        if (!m_managers || !m_managers->recordingManager)
        {
            return 0;
        }
        return m_managers->recordingManager->recordedMaxBytes();
    }

    bool ScopeOneCore::startRecording(const RecordingSettings& settings, const QStringList& activeCameraIds)
    {
        if (!m_managers || !m_managers->recordingManager)
        {
            return false;
        }

        RecordingSettings settingsSnapshot = settings;
        if (settingsSnapshot.metadataFileName.trimmed().isEmpty())
        {
            settingsSnapshot.metadataFileName = recordingMetadataFileName(settingsSnapshot.baseName);
        }
        if (settingsSnapshot.sessionMetadataJson.isEmpty())
        {
            settingsSnapshot.sessionMetadataJson = buildDevicePropertyMetadataJson(*this);
        }
        return m_managers->recordingManager->start(
            toRecordingManagerSettings(settingsSnapshot),
            activeCameraIds);
    }

    void ScopeOneCore::stopRecording()
    {
        if (m_managers && m_managers->recordingManager)
        {
            m_managers->recordingManager->stop();
        }
    }

    bool ScopeOneCore::isRecording() const
    {
        return m_managers && m_managers->recordingManager
            && m_managers->recordingManager->isRecording();
    }

    QString ScopeOneCore::saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session) const
    {
        if (!session)
        {
            return QStringLiteral("Error: no session data");
        }
        RecordingCapturePlanData capturePlan = session->capturePlan();
        if (capturePlan.metadataFileName.trimmed().isEmpty())
        {
            capturePlan.metadataFileName = recordingMetadataFileName(capturePlan.baseName);
        }
        if (capturePlan.sessionMetadataJson.isEmpty())
        {
            capturePlan.sessionMetadataJson = buildDevicePropertyMetadataJson(*this);
        }
        session->setCapturePlan(capturePlan);
        return RecordingManager::saveSessionToDisk(session);
    }

    void ScopeOneCore::saveRecordingSessionAsync(const std::shared_ptr<RecordingSessionData>& session)
    {
        if (!session)
        {
            emit recordingSessionSaveFinished(session);
            return;
        }
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, session]()
        {
            emit recordingSessionSaveFinished(session);
            watcher->deleteLater();
        });

        const auto future = QtConcurrent::run([this, session]()
        {
            return saveRecordingSession(session);
        });
        watcher->setFuture(future);
    }
} // namespace scopeone::core
