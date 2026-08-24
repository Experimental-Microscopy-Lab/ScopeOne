#include "scopeone/ScopeOneCore.h"
#include "scopeone/ImageSceneModel.h"

#include "internal/AcquisitionEngine.h"
#include "internal/ImageProcessingFramework.h"
#include "internal/ProcessingModuleRegistry.h"
#include "internal/FrameRouter.h"
#include "internal/DriverHostProviderProxy.h"
#include "internal/HardwareRuntime.h"
#include "internal/MMCoreManager.h"
#include "internal/MicroManagerProvider.h"
#include "internal/CameraManager.h"
#include "internal/ParticleAnalysis.h"
#include "internal/RecordingManager.h"
#include "internal/StageMosaicManager.h"
#include "MMCore.h"
#include <scopewriter/ScopeWriter.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QPluginLoader>
#include <QStringList>
#include <QSysInfo>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>
#include <opencv2/core/version.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace
{
    // Histogram bins are fixed to keep UI cost stable
    constexpr int kHistogramBinCount = 256;
    // Auto stretch ignores a small tail on each side
    constexpr double kHistogramAutoStretchIgnoredQuantile = 0.001;
    // Histogram refresh is throttled to keep preview responsive
    constexpr qint64 kHistogramRefreshIntervalMs = 250;
    // Live line profiles update fast enough for interaction without following camera rate
    constexpr qint64 kLineProfileRefreshIntervalMs = 16;
    // Display delivery is bounded independently from acquisition and processing throughput
    constexpr qint64 kPreviewRefreshIntervalMs = 16;
    // Change to true temporarily when pipeline FPS diagnostics are needed
    constexpr bool kFrameRateDiagnosticsEnabled = false;
    constexpr int kFrameRateDiagnosticIntervalMs = 3000;

    struct ProviderRegistrationResult
    {
        scopeone::core::HardwareProviderPtr provider;
        QString errorMessage;
    };

    struct OfflineProcessingResult
    {
        QList<scopeone::core::ImageFrame> frames;
        scopeone::core::ExperimentPlan plan;
        QString errorMessage;
        bool canceled{false};
    };

    // Convert a histogram bin to its lower source value
    int histogramBinLowerValue(int binIndex, int maxValue)
    {
        if (maxValue <= 0 || binIndex <= 0)
        {
            return 0;
        }
        const qint64 numerator = static_cast<qint64>(binIndex) * (maxValue + 1);
        return qBound(0, static_cast<int>(numerator / kHistogramBinCount), maxValue);
    }

    // Convert a histogram bin to its upper source value
    int histogramBinUpperValue(int binIndex, int maxValue)
    {
        if (maxValue <= 0)
        {
            return 0;
        }
        const qint64 numerator = static_cast<qint64>(binIndex + 1) * (maxValue + 1);
        return qBound(0, static_cast<int>((numerator - 1) / kHistogramBinCount), maxValue);
    }

    // Estimate display levels while ignoring small outlier tails
    void computeAutoLevels(scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        stats.autoMinLevel = 0;
        stats.autoMaxLevel = stats.maxValue > 0 ? stats.maxValue : 255;
        if (stats.totalPixels <= 0 || stats.histogram.empty())
        {
            return;
        }

        const double outlierPixels = stats.totalPixels * kHistogramAutoStretchIgnoredQuantile;

        qint64 cumulative = 0;
        for (int i = 0; i < static_cast<int>(stats.histogram.size()); ++i)
        {
            cumulative += stats.histogram[static_cast<size_t>(i)];
            if (cumulative > outlierPixels)
            {
                stats.autoMinLevel = histogramBinLowerValue(i, stats.maxValue);
                break;
            }
        }

        cumulative = 0;
        for (int i = static_cast<int>(stats.histogram.size()) - 1; i >= 0; --i)
        {
            cumulative += stats.histogram[static_cast<size_t>(i)];
            if (cumulative > outlierPixels)
            {
                stats.autoMaxLevel = histogramBinUpperValue(i, stats.maxValue);
                break;
            }
        }

        const int rangeMin = 0;
        const int rangeMax = stats.maxValue > 0 ? stats.maxValue : 255;
        if (stats.autoMaxLevel - stats.autoMinLevel < 2)
        {
            const int mid = (stats.autoMinLevel + stats.autoMaxLevel) / 2;
            if (mid <= rangeMin)
            {
                stats.autoMinLevel = rangeMin;
                stats.autoMaxLevel = qMin(rangeMax, rangeMin + 2);
            }
            else if (mid >= rangeMax)
            {
                stats.autoMaxLevel = rangeMax;
                stats.autoMinLevel = qMax(rangeMin, rangeMax - 2);
            }
            else
            {
                stats.autoMinLevel = mid - 1;
                stats.autoMaxLevel = mid + 1;
            }
        }
    }

    // Build the cache key for raw and processed histogram layers
    QString histogramLayerKey(const QString& cameraId, bool processed)
    {
        return processed
                   ? scopeone::core::ScopeOneCore::processedLayerKey(cameraId)
                   : scopeone::core::ScopeOneCore::rawLayerKey(cameraId);
    }

    // Derive a stable metadata file name from the recording base name
    QString recordingMetadataFileName(const QString& baseName)
    {
        const QString trimmedBaseName = baseName.trimmed();
        if (trimmedBaseName.isEmpty())
        {
            return QStringLiteral("recording_metadata.json");
        }
        return trimmedBaseName + QStringLiteral("_metadata.json");
    }

    bool processingRecipesEqual(const scopeone::core::ProcessingRecipe& left,
                                const scopeone::core::ProcessingRecipe& right)
    {
        if (left.bitDepth != right.bitDepth || left.modules.size() != right.modules.size())
        {
            return false;
        }
        for (qsizetype index = 0; index < left.modules.size(); ++index)
        {
            const auto& leftModule = left.modules.at(index);
            const auto& rightModule = right.modules.at(index);
            if (leftModule.moduleId != rightModule.moduleId
                || leftModule.schemaVersion != rightModule.schemaVersion
                || leftModule.parameters != rightModule.parameters)
            {
                return false;
            }
        }
        return true;
    }

    template <typename Operation>
    // Apply camera changes while active previews are temporarily stopped
    bool withSuspendedPreviews(scopeone::core::ScopeOneCore* core, const QStringList& cameraIds, Operation&& operation)
    {
        for (const QString& cameraId : cameraIds)
        {
            core->stopPreview(cameraId);
        }
        const bool ok = operation();
        for (const QString& cameraId : cameraIds)
        {
            core->startPreview(cameraId);
        }
        return ok;
    }

    // Compute intensity statistics for mono preview frames
    bool computeHistogramStatsInternal(const scopeone::core::ImageFrame& frame,
                                       scopeone::core::ScopeOneCore::HistogramStats& stats)
    {
        if (!frame.isValid())
        {
            return false;
        }

        const qint64 totalPixels64 = static_cast<qint64>(frame.width) * static_cast<qint64>(frame.height);
        if (totalPixels64 <= 0 || totalPixels64 > (std::numeric_limits<int>::max)())
        {
            return false;
        }
        const int totalPixels = static_cast<int>(totalPixels64);

        const bool mono16 = frame.isMono16();
        const bool mono8 = frame.isMono8();
        if (!mono16 && !mono8)
        {
            return false;
        }

        const int bytesPerPixel = mono16 ? 2 : 1;
        const qint64 minimumStride = static_cast<qint64>(frame.width) * bytesPerPixel;
        if (minimumStride <= 0 || frame.payloadByteCount() <= 0 || frame.stride < minimumStride)
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
        stats.histogram.assign(kHistogramBinCount, 0);

        const uchar* bytes = reinterpret_cast<const uchar*>(frame.bytes.constData());
        quint64 sum = 0;
        quint64 sumSq = 0;
        int minimum = (std::numeric_limits<int>::max)();
        int maximum = 0;

        if (mono16)
        {
            const int histogramShift = stats.bitDepth - 8;
            for (int y = 0; y < frame.height; ++y)
            {
                const quint16* row = reinterpret_cast<const quint16*>(bytes + static_cast<qint64>(y) * frame.stride);
                for (int x = 0; x < frame.width; ++x)
                {
                    const int value = static_cast<int>(row[x]);
                    const int bin = histogramShift >= 0
                                        ? value >> histogramShift
                                        : value << -histogramShift;
                    sum += static_cast<quint64>(value);
                    sumSq += static_cast<quint64>(value) * static_cast<quint64>(value);
                    minimum = (std::min)(minimum, value);
                    maximum = (std::max)(maximum, value);
                    stats.histogram[static_cast<size_t>(
                        qBound(0, bin, kHistogramBinCount - 1))] += 1;
                }
            }
        }
        else
        {
            stats.bitDepth = 8;
            stats.maxValue = 255;
            for (int y = 0; y < frame.height; ++y)
            {
                const uchar* row = bytes + static_cast<qint64>(y) * frame.stride;
                for (int x = 0; x < frame.width; ++x)
                {
                    const int value = static_cast<int>(row[x]);
                    sum += static_cast<quint64>(value);
                    sumSq += static_cast<quint64>(value) * static_cast<quint64>(value);
                    minimum = (std::min)(minimum, value);
                    maximum = (std::max)(maximum, value);
                    stats.histogram[static_cast<size_t>(value)] += 1;
                }
            }
        }

        stats.minVal = static_cast<double>(minimum);
        stats.maxVal = static_cast<double>(maximum);
        stats.mean = static_cast<double>(sum) / (std::max)(1, totalPixels);
        const double variance = static_cast<double>(sumSq) / (std::max)(1, totalPixels)
                                - (stats.mean * stats.mean);
        stats.stdDev = std::sqrt((std::max)(0.0, variance));
        computeAutoLevels(stats);
        return true;
    }

    // Read one mono sample from a frame at image coordinates
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

        const char* rowData = frame.bytes.constData() + static_cast<qint64>(frame.stride) * point.y();
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
    // Sample a straight line through an image using the supplied sampler
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

    // Translate manager load results into the public facade result
    scopeone::core::ScopeOneCore::LoadConfigResult toFacadeLoadConfigResult(
        const scopeone::core::internal::MMCoreManager::LoadConfigResult& result)
    {
        scopeone::core::ScopeOneCore::LoadConfigResult facade;
        facade.cameraIds = result.cameraIds;
        facade.failedDevices = result.failedDevices;
        facade.successCount = result.successCount;
        facade.failCount = result.failCount;
        facade.skippedCameraCount = result.skippedCameraCount;
        facade.foundCamera = result.foundCamera;
        facade.devices = result.devices;
        return facade;
    }

    struct ConfigurationTaskResult
    {
        bool success{false};
        scopeone::core::internal::MMCoreManager::LoadConfigResult loadResult;
        QString errorMessage;
    };

    struct StageTaskResult
    {
        bool success{false};
        QString errorMessage;
    };

    // Capture cached device properties for recording metadata
    QJsonObject buildDevicePropertyMetadata(const scopeone::core::ScopeOneCore& core)
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
                core.deviceProperties(trimmedDeviceLabel, true);
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

        return devicePropertiesObject;
    }

    // Keep only presentation entries backed by a valid image layer
    void filterRecordingPresentation(
        const scopeone::core::ExperimentDocument& presentation,
        QList<scopeone::core::DocumentLayer>& layers,
        QList<scopeone::core::DocumentMarkup>& markups)
    {
        layers.clear();
        markups.clear();
        QSet<QString> layerIds;
        for (const auto& layer : presentation.layers)
        {
            if (layer.width > 0 && layer.height > 0)
            {
                layers.append(layer);
                layerIds.insert(layer.id);
            }
        }
        for (const auto& markup : presentation.markups)
        {
            if (layerIds.contains(markup.layerId))
            {
                markups.append(markup);
            }
        }
    }

}

namespace scopeone::core
{
    using scopeone::core::internal::ImageProcessingManager;
    using scopeone::core::internal::HardwareRuntime;
    using scopeone::core::internal::FrameRouter;
    using scopeone::core::internal::MMCoreManager;
    using scopeone::core::internal::CameraManager;
    using scopeone::core::internal::CameraRuntimeControl;
    using scopeone::core::internal::MicroManagerProvider;
    using scopeone::core::internal::ProcessingModule;
    using scopeone::core::internal::ProcessingPipelineDefinition;
    using scopeone::core::internal::RecordingManager;
    using scopeone::core::internal::StageMosaicManager;

    static bool equalCanonicalParameter(const QVariant& actual, const QVariant& expected)
    {
        return actual.metaType().id() == expected.metaType().id()
            && actual == expected;
    }

    static bool equalCanonicalParameters(const QVariantMap& actual, const QVariantMap& expected)
    {
        if (actual.size() != expected.size())
        {
            return false;
        }
        for (auto it = expected.constBegin(); it != expected.constEnd(); ++it)
        {
            const auto actualIt = actual.constFind(it.key());
            if (actualIt == actual.constEnd()
                || !equalCanonicalParameter(actualIt.value(), it.value()))
            {
                return false;
            }
        }
        return true;
    }

    // Clear all frame graph state
    void ScopeOneCore::FrameGraph::clear()
    {
        m_rawFrames.clear();
        m_processedFrames.clear();
        m_staticFrames.clear();
        m_externalFrames.clear();
    }

    // Store the latest valid frame for one graph stream
    bool ScopeOneCore::FrameGraph::publishLatest(FrameGraphStream stream, const ImageFrame& frame)
    {
        return publishLatest(stream, frame.cameraId, frame);
    }

    // Store one frame using an explicit graph source id
    bool ScopeOneCore::FrameGraph::publishLatest(FrameGraphStream stream,
                                                 const QString& sourceId,
                                                 const ImageFrame& frame)
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (!frame.isValid() || trimmedSourceId.isEmpty())
        {
            return false;
        }

        ImageFrame storedFrame(frame);
        storedFrame.cameraId = trimmedSourceId;
        latestMap(stream).insert(trimmedSourceId, std::move(storedFrame));
        return true;
    }

    // Return one latest frame from the graph
    ImageFrame ScopeOneCore::FrameGraph::latest(FrameGraphStream stream, const QString& sourceId) const
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (trimmedSourceId.isEmpty())
        {
            return {};
        }

        const auto it = latestMap(stream).constFind(trimmedSourceId);
        return it != latestMap(stream).constEnd() && it.value().isValid() ? it.value() : ImageFrame{};
    }

    // Remove one graph source
    void ScopeOneCore::FrameGraph::remove(FrameGraphStream stream, const QString& sourceId)
    {
        latestMap(stream).remove(sourceId.trimmed());
    }

    // Clear one graph stream
    void ScopeOneCore::FrameGraph::clear(FrameGraphStream stream)
    {
        latestMap(stream).clear();
    }

    QHash<QString, ImageFrame>& ScopeOneCore::FrameGraph::latestMap(FrameGraphStream stream)
    {
        if (stream == FrameGraphStream::Processed)
        {
            return m_processedFrames;
        }
        if (stream == FrameGraphStream::Static)
        {
            return m_staticFrames;
        }
        if (stream == FrameGraphStream::External)
        {
            return m_externalFrames;
        }
        return m_rawFrames;
    }

    const QHash<QString, ImageFrame>& ScopeOneCore::FrameGraph::latestMap(FrameGraphStream stream) const
    {
        if (stream == FrameGraphStream::Processed)
        {
            return m_processedFrames;
        }
        if (stream == FrameGraphStream::Static)
        {
            return m_staticFrames;
        }
        if (stream == FrameGraphStream::External)
        {
            return m_externalFrames;
        }
        return m_rawFrames;
    }

    // Return a stored frame from memory or saved disk output
    ImageFrame ScopeOneCore::RecordingSessionData::imageFrameAt(const QString& cameraId, int index) const
    {
        const QString trimmedCameraId = cameraId.trimmed();
        const ImageFrame* frame = frameAt(trimmedCameraId, index);
        return frame ? *frame : outputImageFrameAt(trimmedCameraId, index);
    }

    // Build a frame-backed recording session for gallery and save sinks
    std::shared_ptr<ScopeOneCore::RecordingSessionData> ScopeOneCore::RecordingSessionData::fromImageFrames(
        const QList<ImageFrame>& frames,
        const ExperimentPlan& capturePlan)
    {
        auto session = std::make_shared<RecordingSessionData>();
        ExperimentPlan framePlan = capturePlan;
        framePlan.streamToDisk = false;
        framePlan.burstMode = false;
        framePlan.targetBursts = 1;
        if (framePlan.experimentId.trimmed().isEmpty())
        {
            framePlan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }

        QList<ImageFrame> normalizedFrames;
        normalizedFrames.reserve(frames.size());
        QHash<QString, int> cameraFrameCounts;
        for (const ImageFrame& frame : frames)
        {
            const QString cameraId = frame.cameraId.trimmed();
            if (!frame.isValid() || cameraId.isEmpty())
            {
                continue;
            }
            if (!framePlan.cameraIds.contains(cameraId))
            {
                framePlan.cameraIds.append(cameraId);
            }
            ImageFrame normalizedFrame(frame);
            normalizedFrame.cameraId = cameraId;
            if (normalizedFrame.timestampNs == 0)
            {
                normalizedFrame.timestampNs =
                    static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
            }
            normalizedFrames.append(std::move(normalizedFrame));
            cameraFrameCounts[cameraId] += 1;
        }
        for (auto it = cameraFrameCounts.constBegin(); it != cameraFrameCounts.constEnd(); ++it)
        {
            framePlan.framesPerBurst = qMax(framePlan.framesPerBurst, it.value());
        }
        session->setCapturePlan(framePlan);
        if (normalizedFrames.isEmpty())
        {
            return {};
        }
        for (const ImageFrame& frame : normalizedFrames)
        {
            session->appendImageFrame(frame);
        }
        QHash<QString, int> timeIndices;
        quint64 sequenceIndex = 0;
        quint64 firstTimestampNs = (std::numeric_limits<quint64>::max)();
        quint64 lastTimestampNs = 0;
        for (const ImageFrame& frame : normalizedFrames)
        {
            const QString& cameraId = frame.cameraId;
            AcquisitionEventRecord record;
            record.event.sequenceIndex = sequenceIndex++;
            record.event.timeIndex = timeIndices[cameraId]++;
            record.event.exposureMs = framePlan.exposureMs;
            record.event.cameraIds = QStringList{cameraId};
            record.startedTimestampNs = frame.timestampNs;
            record.completedTimestampNs = record.startedTimestampNs;
            record.succeeded = true;
            record.frames.insert(cameraId, frameRecordFromImageFrame(frame));
            session->appendEventRecord(record);
            firstTimestampNs = (std::min)(firstTimestampNs, record.startedTimestampNs);
            lastTimestampNs = (std::max)(lastTimestampNs, record.completedTimestampNs);
        }
        if (sequenceIndex > 0)
        {
            session->setStartedTimestampNs(firstTimestampNs);
            session->setRunState(ExperimentRunState::Completed, lastTimestampNs);
        }
        session->prepareForSave(false);
        return session;
    }

    // Return one buffered frame by camera and index
    const ImageFrame* ScopeOneCore::RecordingSessionData::frameAt(const QString& cameraId, int index) const
    {
        const auto it = m_frames.constFind(cameraId.trimmed());
        if (it == m_frames.constEnd()
            || index < 0
            || index >= static_cast<int>(it.value().size()))
        {
            return nullptr;
        }
        return &it.value().at(static_cast<size_t>(index));
    }

    // Read one frame from a saved output file
    ImageFrame ScopeOneCore::RecordingSessionData::outputImageFrameAt(const QString& cameraId, int index) const
    {
        if (index < 0)
        {
            return {};
        }

        const auto manifestIt = m_manifest.output.files.constFind(cameraId);
        if (manifestIt == m_manifest.output.files.constEnd())
        {
            return {};
        }
        const RecordingFileManifest& fileManifest = manifestIt.value();
        if (fileManifest.framesWritten > 0 && index >= fileManifest.framesWritten)
        {
            return {};
        }
        if (fileManifest.rawPath.isEmpty())
        {
            return {};
        }

        const bool omeFormat = m_manifest.plan.format == RecordingFormat::OmeTiff
            || m_manifest.plan.format == RecordingFormat::OmeZarr;
        const AcquisitionEventRecord* selectedRecord = nullptr;
        if (omeFormat)
        {
            int storedFrameIndex = 0;
            for (const AcquisitionEventRecord& record : m_manifest.events)
            {
                if (!record.succeeded || !record.frames.contains(cameraId))
                {
                    continue;
                }
                if (storedFrameIndex++ == index)
                {
                    selectedRecord = &record;
                    break;
                }
            }
            if (!selectedRecord)
            {
                return {};
            }
        }

        scopewriter::DatasetFrameLocation location;
        location.frameIndex = static_cast<std::uint64_t>(index);
#if defined(_WIN32)
        location.dataPath = std::filesystem::path(fileManifest.rawPath.toStdWString());
        location.frameMetadataPath =
            std::filesystem::path(fileManifest.frameInfoPath.toStdWString());
#else
        location.dataPath = std::filesystem::path(fileManifest.rawPath.toStdString());
        location.frameMetadataPath =
            std::filesystem::path(fileManifest.frameInfoPath.toStdString());
#endif

        switch (m_manifest.plan.format)
        {
        case RecordingFormat::OmeZarr:
        {
            const int positionIndex = selectedRecord->event.positionIndex;
            const bool multiPosition = m_manifest.plan.positions.size() > 1;
            if (positionIndex < 0
                || (multiPosition
                    && positionIndex >= static_cast<int>(m_manifest.plan.positions.size()))
                || (!multiPosition && positionIndex != 0))
            {
                return {};
            }
            QString arrayPath = fileManifest.rawPath;
            if (multiPosition)
            {
                arrayPath = QDir(arrayPath).filePath(
                    QStringLiteral("Position %1").arg(positionIndex + 1));
            }
            arrayPath = QDir(arrayPath).filePath(QStringLiteral("0"));
#if defined(_WIN32)
            location.dataPath = std::filesystem::path(arrayPath.toStdWString());
#else
            location.dataPath = std::filesystem::path(arrayPath.toStdString());
#endif
            location.format = scopewriter::Format::OmeZarr;
            location.t = static_cast<std::int64_t>(selectedRecord->event.burstIndex)
                    * (std::max)(1, m_manifest.plan.framesPerBurst)
                + selectedRecord->event.timeIndex;
            location.c = 0;
            location.z = selectedRecord->event.zIndex;
            break;
        }
        case RecordingFormat::OmeTiff:
        {
            location.format = scopewriter::Format::OmeTiff;
            const AcquisitionEvent& event = selectedRecord->event;
            const std::uint64_t framesPerBurst = static_cast<std::uint64_t>(
                (std::max)(1, m_manifest.plan.framesPerBurst));
            const std::uint64_t time = static_cast<std::uint64_t>(event.burstIndex)
                    * framesPerBurst
                + static_cast<std::uint64_t>(event.timeIndex);
            const std::uint64_t timeCount = framesPerBurst
                * static_cast<std::uint64_t>(m_manifest.plan.burstMode
                                                  ? (std::max)(1, m_manifest.plan.targetBursts)
                                                  : 1);
            const std::uint64_t z = static_cast<std::uint64_t>(event.zIndex);
            const std::uint64_t zCount = (std::max)(
                std::uint64_t{1}, static_cast<std::uint64_t>(m_manifest.plan.zPositions.size()));

            const auto timeAxis = std::find(m_manifest.plan.order.begin(),
                                            m_manifest.plan.order.end(),
                                            RecordingAxis::Time);
            const auto zAxis = std::find(m_manifest.plan.order.begin(),
                                         m_manifest.plan.order.end(),
                                         RecordingAxis::Z);
            location.frameIndex = zAxis < timeAxis
                                      ? z * timeCount + time
                                      : time * zCount + z;

            if (m_manifest.plan.positions.size() > 1)
            {
                const int positionIndex = event.positionIndex;
                if (positionIndex < 0
                    || positionIndex >= static_cast<int>(m_manifest.plan.positions.size()))
                {
                    return {};
                }
                const QFileInfo rootInfo(fileManifest.rawPath);
                const QString tiffPath = QDir(fileManifest.rawPath).filePath(
                    QStringLiteral("%1_p%2.ome.tiff")
                        .arg(rootInfo.fileName())
                        .arg(positionIndex, 3, 10, QChar('0')));
#if defined(_WIN32)
                location.dataPath = std::filesystem::path(tiffPath.toStdWString());
#else
                location.dataPath = std::filesystem::path(tiffPath.toStdString());
#endif
            }
            break;
        }
        case RecordingFormat::Tiff:
            location.format = scopewriter::Format::Tiff;
            break;
        case RecordingFormat::Binary:
            if (fileManifest.frameInfoPath.isEmpty())
            {
                return {};
            }
            location.format = scopewriter::Format::Binary;
            break;
        }

        scopewriter::DatasetFrame stored;
        std::string datasetError;
        if (!scopewriter::datasetFrame(location, stored, datasetError)
            || stored.metadata.stride > static_cast<std::size_t>(
                (std::numeric_limits<int>::max)())
            || stored.bytes.size() > static_cast<std::size_t>(
                (std::numeric_limits<qsizetype>::max)()))
        {
            return {};
        }

        ImageFrame frame;
        frame.cameraId = QString::fromUtf8(stored.metadata.cameraId);
        if (frame.cameraId.trimmed().isEmpty())
        {
            frame.cameraId = cameraId;
        }
        frame.width = stored.width;
        frame.height = stored.height;
        frame.stride = static_cast<int>(stored.metadata.stride);
        frame.pixelFormat = stored.pixelType == scopewriter::PixelType::UInt8
            ? ImagePixelFormat::Mono8
            : ImagePixelFormat::Mono16;
        frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(
            frame.pixelFormat, stored.significantBits);
        frame.frameIndex = stored.metadata.frameIndex;
        frame.timestampNs = stored.metadata.timestampNs;
        frame.sourceRoiX = stored.metadata.sourceRoiX;
        frame.sourceRoiY = stored.metadata.sourceRoiY;
        frame.sourceRoiWidth = stored.metadata.sourceRoiWidth;
        frame.sourceRoiHeight = stored.metadata.sourceRoiHeight;
        frame.bytes = QByteArray(reinterpret_cast<const char*>(stored.bytes.data()),
                                 static_cast<qsizetype>(stored.bytes.size()));

        if (selectedRecord)
        {
            const FrameRecord& storedMetadata =
                selectedRecord->frames.constFind(cameraId).value();
            frame.frameIndex = storedMetadata.frameIndex;
            frame.timestampNs = storedMetadata.timestampNs;
            frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(
                frame.pixelFormat, storedMetadata.bitsPerSample);
            frame.sourceRoiX = storedMetadata.sourceRoiX;
            frame.sourceRoiY = storedMetadata.sourceRoiY;
            frame.sourceRoiWidth = storedMetadata.sourceRoiWidth;
            frame.sourceRoiHeight = storedMetadata.sourceRoiHeight;
        }
        return frame.isValid() ? frame : ImageFrame{};
    }

    struct ScopeOneCore::Managers
    {
        struct FrameRateCounters
        {
            quint64 acquired{0};
            quint64 input{0};
            quint64 processed{0};
        };

        struct PendingProcessedFrame
        {
            ImageFrame frame;
            quint64 completedCount{0};
        };

        HardwareRuntime* hardwareRuntime{nullptr};
        CameraProvider* cameraProvider{nullptr};
        CameraRuntimeControl* cameraRuntimeControl{nullptr};
        std::shared_ptr<MicroManagerProvider> microManagerProvider;
        MMCoreManager* mmcoreManager{nullptr};
        CameraManager* cameraManager{nullptr};
        RecordingManager* recordingManager{nullptr};
        ImageProcessingManager* imageProcessingManager{nullptr};
        std::unique_ptr<internal::ProcessingModuleRegistry> processingModuleRegistry;
        StageMosaicManager* stageMosaicManager{nullptr};
        QHash<QString, ExperimentDocument> experiments;
        QHash<QString, std::shared_ptr<RecordingSessionData>> sessions;
        QString activeExperimentId;
        QStringList experimentStartedPreviewCameraIds;
        bool experimentCancelRequested{false};
        RecordingProgress recordingProgress;
        RecordingWriterStatus recordingWriterStatus;
        QHash<QString, double> cameraPixelSizesUm;
        QMutex frameRateCountersMutex;
        QHash<QString, FrameRateCounters> frameRateCounters;
        QElapsedTimer frameRateTimer;
        QMutex processedDeliveryMutex;
        QHash<QString, PendingProcessedFrame> pendingProcessedFrames;
        bool processedFlushQueued{false};
    };

    // Return the compiled core version string
    QString ScopeOneCore::getVersion()
    {
        return QStringLiteral(SCOPEONE_CORE_VERSION_STRING);
    }

    // Return the unified device catalog owned by the hardware runtime
    QList<HardwareDeviceDescriptor> ScopeOneCore::hardwareDevices() const
    {
        return m_managers->hardwareRuntime->deviceRegistry()->devices();
    }

    bool ScopeOneCore::registerHardwareProvider(const HardwareProviderPtr& provider)
    {
        const QString providerId = provider ? provider->descriptor().id.trimmed() : QString{};
        if (!provider
            || providerId.isEmpty()
            || providerId == QStringLiteral("micro-manager")
            || m_managers->hardwareRuntime->deviceRegistry()->provider(providerId)
            || m_configurationOperationRunning
            || m_pendingStageCommands > 0
            || !m_managers->activeExperimentId.isEmpty()
            || isRecording())
        {
            return false;
        }
        return m_managers->hardwareRuntime->registerProvider(provider);
    }

    bool ScopeOneCore::registerDriverHostProvider(const QString& providerId,
                                                  const QString& modulePath,
                                                  const QVariantMap& options,
                                                  QString* errorMessage)
    {
        if (errorMessage) errorMessage->clear();
        const QString normalizedId = providerId.trimmed();
        if (normalizedId.isEmpty()
            || normalizedId == QStringLiteral("micro-manager")
            || m_configurationOperationRunning
            || m_pendingStageCommands > 0
            || !m_managers->activeExperimentId.isEmpty()
            || isRecording()
            || m_pendingProviderRegistrations.contains(normalizedId)
            || m_managers->hardwareRuntime->deviceRegistry()->provider(normalizedId))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Provider cannot be registered in the current state");
            }
            return false;
        }

        m_pendingProviderRegistrations.insert(normalizedId);
        auto* watcher = new QFutureWatcher<ProviderRegistrationResult>(this);
        connect(watcher, &QFutureWatcher<ProviderRegistrationResult>::finished,
                this, [this, watcher, normalizedId]()
                {
                    ProviderRegistrationResult result = watcher->result();
                    m_pendingProviderRegistrations.remove(normalizedId);
                    bool success = static_cast<bool>(result.provider);
                    if (success
                        && !m_managers->hardwareRuntime->registerProvider(result.provider))
                    {
                        success = false;
                        result.errorMessage = QStringLiteral(
                            "Provider device catalog conflicts with registered hardware");
                    }
                    emit hardwareProviderRegistrationFinished(normalizedId,
                                                              success,
                                                              result.errorMessage);
                    watcher->deleteLater();
                });
        const QJsonObject providerOptions = QJsonObject::fromVariantMap(options);
        watcher->setFuture(QtConcurrent::run(
            m_hardwareThreadPool.get(),
            [normalizedId, modulePath, providerOptions]()
            {
                ProviderRegistrationResult result;
                result.provider = internal::createDriverHostProviderProxy(
                    normalizedId, modulePath, providerOptions, &result.errorMessage);
                return result;
            }));
        return true;
    }

    bool ScopeOneCore::unregisterHardwareProvider(const QString& providerId)
    {
        const QString normalizedId = providerId.trimmed();
        if (normalizedId.isEmpty()
            || normalizedId == QStringLiteral("micro-manager")
            || m_configurationOperationRunning
            || m_pendingStageCommands > 0
            || !m_managers->activeExperimentId.isEmpty()
            || isRecording()
            || m_pendingProviderRegistrations.contains(normalizedId))
        {
            return false;
        }
        if (!m_managers->hardwareRuntime->deviceRegistry()->provider(normalizedId))
        {
            return false;
        }
        m_managers->hardwareRuntime->unregisterProvider(normalizedId);
        return true;
    }

    // Return the linked MMCore version
    QString ScopeOneCore::getMMCoreVersion()
    {
        return QStringLiteral("%1.%2.%3")
            .arg(CMMCore::getMMCoreVersionMajor())
            .arg(CMMCore::getMMCoreVersionMinor())
            .arg(CMMCore::getMMCoreVersionPatch());
    }

    // Return the compiled OpenCV version
    QString ScopeOneCore::getOpenCVVersion()
    {
        return QStringLiteral(CV_VERSION);
    }

    // Return the ScopeWriter libtiff version
    QString ScopeOneCore::getLibTiffVersion()
    {
        return QString::fromStdString(scopewriter::libTiffVersion());
    }

    // Return the ScopeWriter zlib version
    QString ScopeOneCore::getZlibVersion()
    {
        return QString::fromStdString(scopewriter::zlibVersion());
    }

    // Build the graph layer key for one raw source
    QString ScopeOneCore::rawLayerKey(const QString& cameraId)
    {
        return QStringLiteral("raw:%1").arg(cameraId.trimmed());
    }

    // Build the graph layer key for one processed source
    QString ScopeOneCore::processedLayerKey(const QString& cameraId)
    {
        return QStringLiteral("proc:%1").arg(cameraId.trimmed());
    }

    // Build the graph layer key for one static source
    QString ScopeOneCore::staticLayerKey(const QString& sourceId)
    {
        return QStringLiteral("static:%1").arg(sourceId.trimmed());
    }

    // Extract the source id encoded in a graph layer key
    QString ScopeOneCore::sourceIdFromLayerKey(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        const int separator = trimmedLayerKey.indexOf(QLatin1Char(':'));
        return separator >= 0 ? trimmedLayerKey.mid(separator + 1) : trimmedLayerKey;
    }

    bool ScopeOneCore::isRawLayerKey(const QString& layerKey)
    {
        return layerKey.trimmed().startsWith(QStringLiteral("raw:"));
    }

    bool ScopeOneCore::isProcessedLayerKey(const QString& layerKey)
    {
        return layerKey.trimmed().startsWith(QStringLiteral("proc:"));
    }

    bool ScopeOneCore::isStaticLayerKey(const QString& layerKey)
    {
        return layerKey.trimmed().startsWith(QStringLiteral("static:"));
    }

    void ScopeOneCore::ensureSceneLayer(const QString& layerKey,
                                        const QString& sourceId,
                                        const QString& name,
                                        DocumentLayerKind kind)
    {
        DocumentLayer layer;
        layer.id = layerKey.trimmed();
        layer.sourceId = sourceId.trimmed();
        layer.name = name.trimmed();
        layer.kind = kind;
        if (kind == DocumentLayerKind::Processed)
        {
            layer.display.colormap = QStringLiteral("Green");
        }
        m_imageSceneModel->ensureLayer(layer);
    }

    // Wire core managers and public signals into one facade object
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
        qRegisterMetaType<scopeone::core::ScopeOneCore::LoadConfigResult>(
            "scopeone::core::ScopeOneCore::LoadConfigResult");
        qRegisterMetaType<scopeone::core::ScopeOneCore::ParticleDetectionResult>(
            "scopeone::core::ScopeOneCore::ParticleDetectionResult");
        qRegisterMetaType<scopeone::core::ImageFrame>("scopeone::core::ImageFrame");
        m_managers->processingModuleRegistry =
            std::make_unique<internal::ProcessingModuleRegistry>();
        const QStringList processingPluginErrors = m_managers->processingModuleRegistry->loadPlugins(
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins/processing")));
        for (const QString& error : processingPluginErrors)
        {
            qWarning().noquote() << QStringLiteral("Failed to load processing plugin %1").arg(error);
        }
        m_histogramThreadPool = std::make_unique<QThreadPool>();
        m_histogramThreadPool->setMaxThreadCount(1);
        m_hardwareThreadPool = std::make_unique<QThreadPool>();
        m_hardwareThreadPool->setMaxThreadCount(1);
        m_analysisThreadPool = std::make_unique<QThreadPool>();
        m_analysisThreadPool->setMaxThreadCount(1);
        m_sessionFrameThreadPool = std::make_unique<QThreadPool>();
        m_sessionFrameThreadPool->setMaxThreadCount(1);
        m_offlineProcessingThreadPool = std::make_unique<QThreadPool>();
        m_offlineProcessingThreadPool->setMaxThreadCount(1);
        m_previewFlushTimer = new QTimer(this);
        m_previewFlushTimer->setSingleShot(true);
        m_previewFlushTimer->setTimerType(Qt::PreciseTimer);
        connect(m_previewFlushTimer, &QTimer::timeout,
                this, &ScopeOneCore::flushPreviewFrames);
        if constexpr (kFrameRateDiagnosticsEnabled)
        {
            auto* frameRateDiagnosticTimer = new QTimer(this);
            frameRateDiagnosticTimer->setInterval(kFrameRateDiagnosticIntervalMs);
            connect(frameRateDiagnosticTimer, &QTimer::timeout, this, [this]()
            {
                if (!isRealTimeProcessingEnabled())
                {
                    QMutexLocker locker(&m_managers->frameRateCountersMutex);
                    m_managers->frameRateCounters.clear();
                    m_managers->frameRateTimer.restart();
                    return;
                }

                QHash<QString, Managers::FrameRateCounters> counters;
                qint64 elapsedNs = 0;
                {
                    QMutexLocker locker(&m_managers->frameRateCountersMutex);
                    elapsedNs = m_managers->frameRateTimer.nsecsElapsed();
                    counters.swap(m_managers->frameRateCounters);
                    m_managers->frameRateTimer.restart();
                }
                if (elapsedNs <= 0)
                {
                    return;
                }
                const double elapsedSeconds = static_cast<double>(elapsedNs) / 1000000000.0;
                QStringList cameraIds = counters.keys();
                cameraIds.sort(Qt::CaseInsensitive);
                for (const QString& cameraId : cameraIds)
                {
                    const Managers::FrameRateCounters frameCounters = counters.value(cameraId);
                    qDebug().noquote()
                        << QString("Frame pipeline FPS [%1]: acquired=%2, input=%3, processed=%4")
                               .arg(cameraId)
                               .arg(frameCounters.acquired / elapsedSeconds, 0, 'f', 1)
                               .arg(frameCounters.input / elapsedSeconds, 0, 'f', 1)
                               .arg(frameCounters.processed / elapsedSeconds, 0, 'f', 1);
                }
            });
            m_managers->frameRateTimer.start();
            frameRateDiagnosticTimer->start();
        }
        m_imageSceneModel = new ImageSceneModel(this);
        connect(m_imageSceneModel, &ImageSceneModel::markupsChanged,
                this, &ScopeOneCore::syncLineProfileFromScene);
        m_managers->mmcoreManager = new MMCoreManager(this);
        m_managers->cameraManager = new CameraManager(this);
        m_managers->microManagerProvider =
            std::make_shared<MicroManagerProvider>(m_managers->mmcoreManager->getCore(),
                                                   m_managers->cameraManager,
                                                   m_managers->cameraManager);
        m_managers->hardwareRuntime = new HardwareRuntime(this);
        m_managers->cameraProvider = m_managers->hardwareRuntime;
        m_managers->cameraRuntimeControl = m_managers->hardwareRuntime;
        connect(m_managers->hardwareRuntime, &HardwareRuntime::devicesChanged,
                this, [this]()
                {
                    synchronizeCameraIdsFromRegistry();
                    emit hardwareDevicesChanged();
                });
        connect(m_managers->hardwareRuntime, &HardwareRuntime::previewStateChanged,
                this, &ScopeOneCore::previewStateChanged);
        m_managers->hardwareRuntime->registerProvider(m_managers->microManagerProvider);
        m_managers->recordingManager = new RecordingManager(this);
        m_managers->imageProcessingManager = new ImageProcessingManager(this);
        m_managers->stageMosaicManager = new StageMosaicManager(this, this);
        m_managers->recordingWriterStatus.reset(
            m_managers->recordingManager->recordedMaxBytes());
        m_managers->recordingManager->setCameraProvider(m_managers->cameraProvider);
        m_managers->recordingManager->setStageProvider(m_managers->hardwareRuntime);
        m_managers->recordingManager->setCameraRuntimeControl(m_managers->cameraRuntimeControl);
        m_managers->recordingManager->setLatestFrameFetcher(
            [this](const QString& cameraId, ImageFrame& frame)
            {
                frame = graphFrame(rawLayerKey(cameraId));
                return frame.isValid();
            });
        m_managers->recordingManager->setSessionPreparationCallback(
            [this](RecordingSessionData& session)
            {
                QList<DocumentLayer> layers;
                QList<DocumentMarkup> markups;
                filterRecordingPresentation(m_imageSceneModel->document(), layers, markups);
                session.setPresentationState(layers, markups);
            });

        connect(m_managers->hardwareRuntime->frameRouter(), &FrameRouter::frameReady,
                this, &ScopeOneCore::handleIncomingRawFrame);
        connect(m_managers->hardwareRuntime->frameRouter(), &FrameRouter::frameReady,
                this, [this](const ImageFrame& frame)
                {
                    const HardwareDeviceDescriptor device =
                        m_managers->hardwareRuntime->deviceRegistry()->device(frame.cameraId);
                    if (device.providerId == QStringLiteral("micro-manager"))
                    {
                        return;
                    }
                    emit rawFramesAcquired(frame.cameraId, 1);
                    submitProcessingFrame(frame);
                    m_managers->recordingManager->onRawFramesReady(QList<ImageFrame>{frame});
                });
        connect(m_managers->cameraManager, &CameraManager::processingFrameReady,
                this, &ScopeOneCore::submitProcessingFrame,
                Qt::DirectConnection);
        connect(m_managers->cameraManager, &CameraManager::rawFramesAcquired,
                this, [this](const QString& cameraId, quint64 frameCount)
                {
                    if constexpr (kFrameRateDiagnosticsEnabled)
                    {
                        if (isRealTimeProcessingEnabled())
                        {
                            QMutexLocker locker(&m_managers->frameRateCountersMutex);
                            m_managers->frameRateCounters[cameraId].acquired += frameCount;
                        }
                    }
                    emit rawFramesAcquired(cameraId, frameCount);
                });
        connect(m_managers->cameraManager, &CameraManager::recordingFramesReady,
                m_managers->recordingManager, &RecordingManager::onRawFramesReady);
        connect(m_managers->cameraManager, &CameraManager::frameDeliveryFailed,
                m_managers->recordingManager, &RecordingManager::onFrameDeliveryFailed);
        connect(m_managers->cameraManager, &CameraManager::driverHostControlServerListening,
                this, &ScopeOneCore::driverHostControlServerListening);

        connect(m_managers->recordingManager, &RecordingManager::mdaRawFrameReady,
                this, [this](const ImageFrame& frame)
                {
                    handleIncomingRawFrame(frame);
                    submitProcessingFrame(frame);
                },
                Qt::QueuedConnection);

        connect(m_managers->recordingManager, &RecordingManager::progressChanged,
                this,
                [this](int phase,
                       qint64 frameCurrent,
                       qint64 frameTarget,
                       int burstCurrent,
                       int burstTarget,
                       qint64 waitRemainingMs,
                       int timeIndex,
                       int timeCount,
                       int zIndex,
                       int zCount,
                       int positionIndex,
                       int positionCount,
                       bool hasXY,
                       double x,
                       double y,
                       bool hasZ,
                       double z)
                {
                    RecordingProgress& progress = m_managers->recordingProgress;
                    progress.phase = phase;
                    progress.frameCurrent = frameCurrent;
                    progress.frameTarget = frameTarget;
                    progress.burstCurrent = burstCurrent;
                    progress.burstTarget = burstTarget;
                    progress.waitRemainingMs = waitRemainingMs;
                    progress.timeIndex = timeIndex;
                    progress.timeCount = timeCount;
                    progress.zIndex = zIndex;
                    progress.zCount = zCount;
                    progress.positionIndex = positionIndex;
                    progress.positionCount = positionCount;
                    progress.hasXY = hasXY;
                    progress.x = x;
                    progress.y = y;
                    progress.hasZ = hasZ;
                    progress.z = z;
                    emit recordingProgressChanged(phase,
                                                  frameCurrent,
                                                  frameTarget,
                                                  burstCurrent,
                                                  burstTarget,
                                                  waitRemainingMs,
                                                  timeIndex,
                                                  timeCount,
                                                  zIndex,
                                                  zCount,
                                                  positionIndex,
                                                  positionCount,
                                                  hasXY,
                                                  x,
                                                  y,
                                                  hasZ,
                                                  z);
                });
        connect(m_managers->recordingManager, &RecordingManager::writerStatusChanged,
                this, [this](const RecordingWriterStatus& status)
                {
                    m_managers->recordingWriterStatus = status;
                    emit recordingWriterStatusChanged(status);
                });
        connect(m_managers->recordingManager, &RecordingManager::recordingStateChanged,
                this, &ScopeOneCore::recordingStateChanged);
        connect(m_managers->recordingManager, &RecordingManager::recordingStopped,
                this, [this](const std::shared_ptr<RecordingSessionData>& session)
                {
                    finalizeActiveExperiment(session);
                    emit recordingStopped(session);
                });
        connect(m_managers->stageMosaicManager, &StageMosaicManager::progressChanged,
                this, &ScopeOneCore::stageMosaicProgress);
        connect(m_managers->stageMosaicManager, &StageMosaicManager::frameUpdated,
                this, &ScopeOneCore::stageMosaicFrameUpdated);
        connect(m_managers->stageMosaicManager, &StageMosaicManager::finished,
                this, &ScopeOneCore::stageMosaicFinished);

        connect(m_managers->imageProcessingManager, &ImageProcessingManager::imageProcessed,
                this, &ScopeOneCore::handleProcessedFrame,
                Qt::DirectConnection);
        connect(m_managers->imageProcessingManager, &ImageProcessingManager::processingFrameFinished,
                m_managers->hardwareRuntime, &HardwareRuntime::finishProcessingFrame,
                Qt::DirectConnection);
        connect(m_managers->imageProcessingManager, &ImageProcessingManager::processingError,
                this, &ScopeOneCore::processingError);

        const QDir hardwarePlugins(
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins/hardware")));
        for (const QFileInfo& file : hardwarePlugins.entryInfoList(QDir::Files, QDir::Name))
        {
            QPluginLoader loader(file.absoluteFilePath());
            const QJsonObject metadata = loader.metaData().value(QStringLiteral("MetaData")).toObject();
            if (!metadata.value(QStringLiteral("autoLoad")).toBool())
            {
                continue;
            }
            const QString providerId = metadata.value(QStringLiteral("providerId")).toString().trimmed();
            QString errorMessage;
            if (providerId.isEmpty()
                || !registerDriverHostProvider(providerId, file.absoluteFilePath(), {}, &errorMessage))
            {
                qWarning().noquote()
                    << QStringLiteral("Failed to discover hardware plugin %1: %2")
                           .arg(file.fileName(), errorMessage);
            }
        }
    }

    // Release loaded devices before the facade is destroyed
    ScopeOneCore::~ScopeOneCore()
    {
        m_hardwareThreadPool->waitForDone();
        m_analysisThreadPool->waitForDone();
        m_sessionFrameThreadPool->waitForDone();
        for (const auto& token : std::as_const(m_processingRequestCancelTokens))
        {
            token->store(true);
        }
        m_offlineProcessingThreadPool->waitForDone();
        m_managers->recordingManager->shutdown();
        m_pendingStageCommands = 0;
        m_configurationOperationRunning = false;
        unloadConfigurationForShutdown();
        m_histogramThreadPool->waitForDone();
        delete m_managers->imageProcessingManager;
        m_managers->imageProcessingManager = nullptr;
    }

    // Return the public configuration lifecycle state
    QString ScopeOneCore::configurationState() const
    {
        switch (m_configurationState)
        {
        case ConfigurationState::Unloaded:
            return QStringLiteral("unloaded");
        case ConfigurationState::Loading:
            return QStringLiteral("loading");
        case ConfigurationState::Loaded:
            return QStringLiteral("loaded");
        case ConfigurationState::PartiallyLoaded:
            return QStringLiteral("partially_loaded");
        case ConfigurationState::Unloading:
            return QStringLiteral("unloading");
        case ConfigurationState::Failed:
            return QStringLiteral("failed");
        }
        return QStringLiteral("failed");
    }

    // Expose the native MMCore handle for low level callers
    std::shared_ptr<CMMCore> ScopeOneCore::core() const
    {
        return m_managers->mmcoreManager->getCore();
    }

    // Collect camera ids that currently have active previews
    QStringList ScopeOneCore::runningPreviewCameraIds() const
    {
        QStringList running;
        for (const QString& cameraId : m_cameraIds)
        {
            if (m_managers->cameraProvider->isPreviewRunning(cameraId))
            {
                running.append(cameraId);
            }
        }
        return running;
    }

    // Return the effective scale for one camera
    double ScopeOneCore::cameraPixelSizeUm(const QString& cameraId) const
    {
        const QString camera = cameraId.trimmed();
        const auto overrideIt = m_managers->cameraPixelSizesUm.constFind(camera);
        if (overrideIt != m_managers->cameraPixelSizesUm.constEnd())
        {
            return overrideIt.value();
        }
        const HardwareDeviceDescriptor device =
            m_managers->hardwareRuntime->deviceRegistry()->device(camera);
        if (device.providerId == QStringLiteral("micro-manager")
            && !m_managers->cameraManager->usesDriverHostBackend()
            && m_cameraIds.size() == 1
            && m_cameraIds.first() == camera)
        {
            return core()->getPixelSizeUm();
        }
        return 0.0;
    }

    // Assign a session scale override to one camera
    bool ScopeOneCore::setCameraPixelSizeUm(const QString& cameraId, double pixelSizeUm)
    {
        const QString camera = cameraId.trimmed();
        if (camera.isEmpty() || !std::isfinite(pixelSizeUm) || pixelSizeUm < 0.0)
        {
            return false;
        }
        if (pixelSizeUm == 0.0)
        {
            m_managers->cameraPixelSizesUm.remove(camera);
        }
        else
        {
            m_managers->cameraPixelSizesUm.insert(camera, pixelSizeUm);
        }
        return true;
    }

    // Return external device adapter directories searched after the application directory
    QStringList ScopeOneCore::additionalDeviceAdapterSearchPaths() const
    {
        return m_managers->mmcoreManager->additionalDeviceAdapterSearchPaths();
    }

    // Set external device adapter directories for subsequent configuration loads
    bool ScopeOneCore::setAdditionalDeviceAdapterSearchPaths(const QStringList& paths)
    {
        if (m_configurationOperationRunning)
        {
            return false;
        }
        QStringList normalizedPaths;
        for (const QString& path : paths)
        {
            const QFileInfo directory(path.trimmed());
            if (!directory.isDir())
            {
                return false;
            }
            const QString normalizedPath = QDir::cleanPath(directory.absoluteFilePath());
            if (!normalizedPaths.contains(normalizedPath, Qt::CaseInsensitive))
            {
                normalizedPaths.append(normalizedPath);
            }
        }
        m_managers->mmcoreManager->setAdditionalDeviceAdapterSearchPaths(normalizedPaths);
        return true;
    }

    // Applies a completed device load to the frame graph and public state
    bool ScopeOneCore::applyLoadedConfiguration(const QString& configPath,
                                                const LoadConfigResult& result)
    {
        m_managers->microManagerProvider->setDevices(result.devices);
        if (!m_managers->hardwareRuntime->refreshProvider(QStringLiteral("micro-manager")))
        {
            m_managers->microManagerProvider->setDevices({});
            m_managers->hardwareRuntime->refreshProvider(QStringLiteral("micro-manager"));
            return false;
        }
        m_configurationFailedDevices = result.failedDevices;
        m_configurationError.clear();
        m_configurationState = result.failedDevices.isEmpty()
                                  ? ConfigurationState::Loaded
                                  : ConfigurationState::PartiallyLoaded;
        const QFileInfo configFile(configPath);
        m_loadedConfigPath = configPath.trimmed().isEmpty()
                                 ? QString()
                                 : configFile.absoluteFilePath();
        m_loadedConfigSha256.clear();
        QFile file(m_loadedConfigPath);
        if (file.open(QIODevice::ReadOnly))
        {
            m_loadedConfigSha256 = QString::fromLatin1(
                QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());
        }
        return true;
    }

    void ScopeOneCore::synchronizeCameraIdsFromRegistry()
    {
        QStringList nextCameraIds;
        for (const HardwareDeviceDescriptor& device
             : m_managers->hardwareRuntime->deviceRegistry()->devices())
        {
            const QString logicalId = device.logicalId.trimmed();
            if (device.kind == HardwareDeviceKind::Camera
                && !logicalId.isEmpty()
                && !nextCameraIds.contains(logicalId))
            {
                nextCameraIds.append(logicalId);
            }
        }
        nextCameraIds.sort(Qt::CaseInsensitive);

        for (const QString& cameraId : m_cameraIds)
        {
            if (!nextCameraIds.contains(cameraId))
            {
                clearLiveFrames(cameraId);
                m_imageSceneModel->removeLayer(rawLayerKey(cameraId));
                m_imageSceneModel->removeLayer(processedLayerKey(cameraId));
            }
        }
        m_cameraIds = nextCameraIds;
        if (!m_realTimeProcessingSource.isEmpty()
            && !m_cameraIds.contains(m_realTimeProcessingSource))
        {
            m_realTimeProcessingSource.clear();
            emit processingSettingsChanged();
        }
        for (const QString& cameraId : m_cameraIds)
        {
            ensureSceneLayer(rawLayerKey(cameraId),
                             cameraId,
                             QStringLiteral("%1 Raw").arg(cameraId),
                             DocumentLayerKind::Raw);
            ensureSceneLayer(processedLayerKey(cameraId),
                             cameraId,
                             QStringLiteral("%1 Processed").arg(cameraId),
                             DocumentLayerKind::Processed);
        }
    }

    // Complete a failed configuration load and publish one consistent result
    void ScopeOneCore::finishConfigurationLoadFailure(const LoadConfigResult& result,
                                                      const QString& errorMessage)
    {
        m_configurationState = ConfigurationState::Failed;
        m_configurationError = errorMessage.isEmpty()
                                   ? QStringLiteral("Failed to load configuration")
                                   : errorMessage;
        m_configurationFailedDevices = result.failedDevices;
        m_configurationOperationRunning = false;
        emit hardwareConfigurationChanged();
        emit configurationLoadFinished(false, result, m_configurationError);
    }

    // Releases hardware synchronously during facade destruction
    void ScopeOneCore::unloadConfigurationForShutdown()
    {
        applySystemShutdownPreset();
        clearConfigurationRuntime(false, true);
        auto handle = core();
        try
        {
            handle->unloadAllDevices();
        }
        catch (const CMMError&)
        {
        }
        m_managers->hardwareRuntime->clear();
    }

    // Apply the configured hardware shutdown state before releasing devices
    void ScopeOneCore::applySystemShutdownPreset()
    {
        m_managers->hardwareRuntime->stopPreviewForProvider(QStringLiteral("micro-manager"));
        auto handle = core();
        try
        {
            if (!handle->isConfigDefined(
                    MM::g_CFGGroup_System, MM::g_CFGGroup_System_Shutdown))
            {
                return;
            }
            m_managers->microManagerProvider->setConfig(
                MM::g_CFGGroup_System, MM::g_CFGGroup_System_Shutdown, nullptr);
        }
        catch (const CMMError& error)
        {
            qWarning().noquote()
                << QString("Failed to inspect System/Shutdown preset: %1")
                       .arg(QString::fromStdString(error.getMsg()));
        }
    }

    // Stops camera runtime and clears state owned by the facade thread
    void ScopeOneCore::clearConfigurationRuntime(bool notify, bool shutdownCameraBackend)
    {
        ++m_analysisGeneration;
        m_managers->stageMosaicManager->cancel();
        const bool processingWasEnabled = isRealTimeProcessingEnabled();
        m_managers->imageProcessingManager->enableRealTimeProcessing(false);
        const QStringList cameraIds = m_cameraIds;
        m_managers->hardwareRuntime->stopPreviewForProvider(QStringLiteral("micro-manager"));
        if (shutdownCameraBackend)
        {
            m_managers->cameraManager->shutdownNow();
        }

        for (const QString& cameraId : cameraIds)
        {
            clearLiveFrames(cameraId);
        }
        clearProcessedFrames();
        clearStaticFrames();
        m_cameraIds.clear();
        m_loadedConfigPath.clear();
        m_loadedConfigSha256.clear();
        m_frameGraph.clear();
        m_previewFlushTimer->stop();
        m_previewPublishTimer.invalidate();
        m_histogramJobStates.clear();
        m_latestHistogramStats.clear();
        m_activeHistogramLayerKey.clear();
        m_imageSceneModel->reset();
        m_managers->microManagerProvider->setDevices(QList<HardwareDeviceDescriptor>{});
        m_managers->hardwareRuntime->refreshProvider(QStringLiteral("micro-manager"));
        if (notify && processingWasEnabled)
        {
            emit processingSettingsChanged();
        }
        if (notify)
        {
            emit hardwareConfigurationChanged();
        }
    }

    // Loads a configuration on the serialized hardware worker
    bool ScopeOneCore::loadConfiguration(const QString& configPath)
    {
        const QString path = configPath.trimmed();
        if (path.isEmpty())
        {
            m_configurationError = QStringLiteral("Configuration path is empty");
            return false;
        }
        if (!QFileInfo(path).isFile())
        {
            m_configurationError = QStringLiteral("Configuration file does not exist: %1").arg(path);
            return false;
        }
        if (m_configurationOperationRunning)
        {
            m_configurationError = QStringLiteral("Another configuration operation is running");
            return false;
        }
        if (!m_pendingProviderRegistrations.isEmpty())
        {
            m_configurationError = QStringLiteral("A hardware provider is still loading");
            return false;
        }
        if (m_pendingStageCommands > 0)
        {
            m_configurationError = QStringLiteral("A stage command is running");
            return false;
        }
        if (isRecording() || m_managers->recordingManager->isFinalizing())
        {
            m_configurationError = QStringLiteral("Recording is still active or finalizing");
            return false;
        }

        m_configurationError.clear();
        m_configurationFailedDevices.clear();
        m_configurationState = ConfigurationState::Loading;
        applySystemShutdownPreset();
        m_configurationOperationRunning = true;
        clearConfigurationRuntime(true, false);
        m_managers->cameraManager->shutdown(
            [this, path](const QString& errorMessage)
            {
                if (!errorMessage.isEmpty())
                {
                    finishConfigurationLoadFailure({}, errorMessage);
                    return;
                }
                startConfigurationLoadTask(path);
            });
        return true;
    }

    // Starts MMCore configuration loading after camera teardown
    void ScopeOneCore::startConfigurationLoadTask(const QString& path)
    {
        auto* watcher = new QFutureWatcher<ConfigurationTaskResult>(this);
        connect(watcher, &QFutureWatcher<ConfigurationTaskResult>::finished,
                this, [this, watcher, path]()
        {
            ConfigurationTaskResult task = watcher->result();
            LoadConfigResult result;
            if (task.success)
            {
                const bool backendsStarted = m_managers->mmcoreManager->startCameraBackends(
                    *m_managers->cameraManager, task.loadResult);
                result = toFacadeLoadConfigResult(task.loadResult);
                if (!backendsStarted)
                {
                    const QString errorMessage = result.failedDevices.isEmpty()
                                                     ? QStringLiteral("Failed to initialize camera backend")
                                                     : QStringLiteral("Failed to initialize camera backend: %1")
                                                           .arg(result.failedDevices.join(QStringLiteral(", ")));
                    m_managers->cameraManager->shutdown(
                        [this, result, errorMessage](const QString& shutdownError)
                        {
                            if (!shutdownError.isEmpty())
                            {
                                finishConfigurationLoadFailure(
                                    result,
                                    QStringLiteral("%1; camera cleanup failed: %2")
                                        .arg(errorMessage, shutdownError));
                                return;
                            }
                            startConfigurationLoadCleanupTask(result, errorMessage);
                        });
                    watcher->deleteLater();
                    return;
                }
                if (!applyLoadedConfiguration(path, result))
                {
                    const QString errorMessage =
                        QStringLiteral("Micro-Manager device catalog conflicts with registered hardware");
                    m_managers->cameraManager->shutdown(
                        [this, result, errorMessage](const QString& shutdownError)
                        {
                            if (!shutdownError.isEmpty())
                            {
                                finishConfigurationLoadFailure(
                                    result,
                                    QStringLiteral("%1; camera cleanup failed: %2")
                                        .arg(errorMessage, shutdownError));
                                return;
                            }
                            startConfigurationLoadCleanupTask(result, errorMessage);
                        });
                    watcher->deleteLater();
                    return;
                }
                m_configurationOperationRunning = false;
                emit hardwareConfigurationChanged();
                emit configurationLoadFinished(true, result, {});
            }
            else
            {
                result = toFacadeLoadConfigResult(task.loadResult);
                finishConfigurationLoadFailure(result, task.errorMessage);
            }
            watcher->deleteLater();
        });

        auto* manager = m_managers->mmcoreManager;
        const auto future = QtConcurrent::run(m_hardwareThreadPool.get(), [manager, path]()
        {
            ConfigurationTaskResult task;
            auto handle = manager->getCore();
            try
            {
                handle->unloadAllDevices();
            }
            catch (const CMMError& error)
            {
                task.errorMessage = QString::fromStdString(error.getMsg());
                return task;
            }

            task.success = manager->loadConfigurationDevices(
                path, task.loadResult, task.errorMessage);
            if (!task.success)
            {
                try
                {
                    handle->unloadAllDevices();
                }
                catch (const CMMError& error)
                {
                    task.errorMessage = QStringLiteral("%1; cleanup failed: %2")
                                            .arg(task.errorMessage,
                                                 QString::fromStdString(error.getMsg()));
                }
            }
            return task;
        });
        watcher->setFuture(future);
    }

    // Removes devices after a camera backend startup failure
    void ScopeOneCore::startConfigurationLoadCleanupTask(const LoadConfigResult& result,
                                                         const QString& errorMessage)
    {
        auto* watcher = new QFutureWatcher<StageTaskResult>(this);
        connect(watcher, &QFutureWatcher<StageTaskResult>::finished,
                this, [this, watcher, result, errorMessage]()
        {
            const StageTaskResult task = watcher->result();
            finishConfigurationLoadFailure(
                result,
                task.errorMessage.isEmpty()
                    ? errorMessage
                    : QStringLiteral("%1; cleanup failed: %2")
                          .arg(errorMessage, task.errorMessage));
            watcher->deleteLater();
        });

        const auto handle = core();
        watcher->setFuture(QtConcurrent::run(m_hardwareThreadPool.get(), [handle]()
        {
            StageTaskResult task;
            try
            {
                handle->unloadAllDevices();
            }
            catch (const CMMError& error)
            {
                task.errorMessage = QString::fromStdString(error.getMsg());
            }
            return task;
        }));
    }

    // Unloads devices on the serialized hardware worker
    bool ScopeOneCore::unloadConfiguration()
    {
        if (m_configurationOperationRunning)
        {
            m_configurationError = QStringLiteral("Another configuration operation is running");
            return false;
        }
        if (!m_pendingProviderRegistrations.isEmpty())
        {
            m_configurationError = QStringLiteral("A hardware provider is still loading");
            return false;
        }
        if (m_pendingStageCommands > 0)
        {
            m_configurationError = QStringLiteral("A stage command is running");
            return false;
        }
        if (isRecording() || m_managers->recordingManager->isFinalizing())
        {
            m_configurationError = QStringLiteral("Recording is still active or finalizing");
            return false;
        }

        m_configurationError.clear();
        m_configurationFailedDevices.clear();
        m_configurationState = ConfigurationState::Unloading;
        applySystemShutdownPreset();
        m_configurationOperationRunning = true;
        clearConfigurationRuntime(true, false);
        m_managers->cameraManager->shutdown(
            [this](const QString& errorMessage)
            {
                if (!errorMessage.isEmpty())
                {
                    m_configurationState = ConfigurationState::Failed;
                    m_configurationError = errorMessage;
                    m_configurationOperationRunning = false;
                    emit hardwareConfigurationChanged();
                    emit configurationUnloadFinished(false, errorMessage);
                    return;
                }
                startConfigurationUnloadTask();
            });
        return true;
    }

    // Starts MMCore device unloading after camera teardown
    void ScopeOneCore::startConfigurationUnloadTask()
    {
        auto* watcher = new QFutureWatcher<StageTaskResult>(this);
        connect(watcher, &QFutureWatcher<StageTaskResult>::finished,
                this, [this, watcher]()
        {
            const StageTaskResult task = watcher->result();
            m_configurationState = task.success
                                       ? ConfigurationState::Unloaded
                                       : ConfigurationState::Failed;
            m_configurationError = task.success
                                       ? QString()
                                       : (task.errorMessage.isEmpty()
                                              ? QStringLiteral("Failed to unload configuration")
                                              : task.errorMessage);
            if (task.success)
            {
                m_configurationFailedDevices.clear();
            }
            m_configurationOperationRunning = false;
            emit hardwareConfigurationChanged();
            emit configurationUnloadFinished(task.success, m_configurationError);
            watcher->deleteLater();
        });

        const auto handle = core();
        watcher->setFuture(QtConcurrent::run(m_hardwareThreadPool.get(), [handle]()
        {
            StageTaskResult task;
            try
            {
                handle->unloadAllDevices();
                task.success = true;
            }
            catch (const CMMError& error)
            {
                task.errorMessage = QString::fromStdString(error.getMsg());
            }
            return task;
        }));
    }

    // Start preview for one camera or the full camera set
    bool ScopeOneCore::startPreview(const QString& cameraIdOrAll)
    {
        // Route preview to one camera or all cameras
        const QString target = cameraIdOrAll.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }
        const bool all = target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0;
        const bool started = all
                                 ? m_managers->hardwareRuntime->startPreview()
                                 : m_managers->hardwareRuntime->startPreviewFor(target);
        if (started)
        {
            emit previewStateChanged(true);
        }
        return started;
    }

    // Stop preview for one camera or the full camera set
    bool ScopeOneCore::stopPreview(const QString& cameraIdOrAll)
    {
        const QString target = cameraIdOrAll.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }
        const bool all = target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0;
        const bool stopped = all
                                 ? m_managers->hardwareRuntime->stopPreview()
                                 : m_managers->hardwareRuntime->stopPreviewFor(target);
        if (stopped)
        {
            emit previewStateChanged(!runningPreviewCameraIds().isEmpty());
        }
        return stopped;
    }

    // Submit exposure changes through the active camera manager
    bool ScopeOneCore::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        const QString target = cameraIdOrAll.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }
        const bool ok = m_managers->cameraProvider->setExposure(target, exposureMs);
        if (ok)
        {
            emit deviceStateChanged();
        }
        return ok;
    }

    // Apply an ROI rectangle to a camera
    bool ScopeOneCore::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        const QString target = cameraId.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }
        const bool ok = m_managers->cameraProvider->setROI(target, x, y, width, height);
        if (ok)
        {
            clearLiveFrames(target);
            emit deviceStateChanged();
        }
        return ok;
    }

    // Apply a centered ROI with half the current width and height
    bool ScopeOneCore::setHalfROI(const QString& cameraId)
    {
        const QString target = cameraId.trimmed();
        if (m_configurationOperationRunning
            || target.isEmpty()
            || target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0)
        {
            return false;
        }

        int originX = 0;
        int originY = 0;
        int sourceWidth = 0;
        int sourceHeight = 0;
        if (!getROI(target, originX, originY, sourceWidth, sourceHeight)
            || sourceWidth <= 0
            || sourceHeight <= 0)
        {
            const ImageFrame frame = graphFrame(rawLayerKey(target));
            if (!frame.isValid())
            {
                return false;
            }
            sourceWidth = frame.width;
            sourceHeight = frame.height;
        }

        const int width = qMax(1, sourceWidth / 2);
        const int height = qMax(1, sourceHeight / 2);
        const int x = originX + (sourceWidth - width) / 2;
        const int y = originY + (sourceHeight - height) / 2;
        return setROI(target, x, y, width, height);
    }

    bool ScopeOneCore::clearROI(const QString& cameraId)
    {
        const QString target = cameraId.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }
        const QStringList targets = target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0
                                        ? m_cameraIds
                                        : QStringList{target};
        if (targets.isEmpty())
        {
            return false;
        }

        bool ok = true;
        bool changed = false;
        for (const QString& cameraId : targets)
        {
            if (!m_managers->cameraProvider->clearROI(cameraId))
            {
                ok = false;
                continue;
            }
            clearLiveFrames(cameraId);
            changed = true;
        }
        if (changed)
        {
            emit deviceStateChanged();
        }
        return ok;
    }

    // Read the active hardware ROI rectangle from a camera
    bool ScopeOneCore::getROI(const QString& cameraId, int& x, int& y, int& width, int& height)
    {
        const QString target = cameraId.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }
        return m_managers->cameraProvider->getROI(target, x, y, width, height);
    }

    // Track the active line profile request for future frames
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

        m_activeLineProfile.sourceId = trimmedCameraId;
        m_activeLineProfile.start = start;
        m_activeLineProfile.end = end;
        m_activeLineProfile.processed = processed;
        m_activeLineProfile.staticSource = false;
        m_activeLineProfile.active = true;
        m_lineProfileUpdateTimer.invalidate();

        if (processed)
        {
            const ImageFrame frame = graphFrame(processedLayerKey(trimmedCameraId));
            if (frame.isValid())
            {
                updateLineProfile(trimmedCameraId, true, frame);
            }
            return;
        }

        const ImageFrame frame = graphFrame(rawLayerKey(trimmedCameraId));
        if (frame.isValid())
        {
            updateLineProfile(trimmedCameraId, false, frame);
        }
    }

    // Compute a line profile from a static graph source
    void ScopeOneCore::setStaticLineProfile(const QString& sourceId, const QPoint& start, const QPoint& end)
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (trimmedSourceId.isEmpty())
        {
            clearLineProfile();
            return;
        }

        m_activeLineProfile = ActiveLineProfile{};
        m_activeLineProfile.sourceId = trimmedSourceId;
        m_activeLineProfile.start = start;
        m_activeLineProfile.end = end;
        m_activeLineProfile.staticSource = true;
        m_activeLineProfile.active = true;

        const ImageFrame frame = graphFrame(staticLayerKey(trimmedSourceId));
        if (!frame.isValid())
        {
            clearLineProfile();
            return;
        }

        if (!updateStaticLineProfile(trimmedSourceId, frame))
        {
            clearLineProfile();
        }
    }

    void ScopeOneCore::clearLineProfile()
    {
        const bool wasActive = m_activeLineProfile.active;
        m_activeLineProfile = ActiveLineProfile{};
        m_lineProfileUpdateTimer.invalidate();
        m_imageSceneModel->clearRole(DocumentMarkupRole::CrossSection);
        if (wasActive)
        {
            emit lineProfileCleared();
        }
    }

    // Keep line profile analysis synchronized with the active scene markup
    void ScopeOneCore::syncLineProfileFromScene()
    {
        for (const ImageSceneModel::Markup& markup : m_imageSceneModel->markups())
        {
            if (markup.role != DocumentMarkupRole::CrossSection
                || markup.type != DocumentMarkupType::Line)
            {
                continue;
            }
            if (markup.layerKind == DocumentLayerKind::Static
                || markup.layerKind == DocumentLayerKind::Gallery)
            {
                setStaticLineProfile(markup.sourceId, markup.start, markup.end);
            }
            else
            {
                setLineProfile(markup.sourceId,
                               markup.start,
                               markup.end,
                               markup.layerKind == DocumentLayerKind::Processed);
            }
            return;
        }
        clearLineProfile();
    }

    // Publish raw frames and start dependent processing work
    void ScopeOneCore::handleIncomingRawFrame(const ImageFrame& frame)
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
        const QString layerKey = rawLayerKey(cameraId);
        m_imageSceneModel->updateLayerFrame(layerKey, normalizedFrame);
        m_frameGraph.publishLatest(FrameGraphStream::Raw, normalizedFrame);
        emit newRawFrameReady(normalizedFrame);
        queuePreviewRawFrame(normalizedFrame);
        scheduleHistogramStats(layerKey, normalizedFrame);
        updateLineProfile(cameraId, false, normalizedFrame);
    }

    // Submits one acquisition frame without crossing the UI event queue
    void ScopeOneCore::submitProcessingFrame(const ImageFrame& frame, quint64 processingToken)
    {
        if (!frame.isValid()
            || !m_managers->imageProcessingManager->isRealTimeProcessingEnabled()
            || (!m_realTimeProcessingSource.isEmpty()
                && frame.cameraId != m_realTimeProcessingSource))
        {
            return;
        }
        if constexpr (kFrameRateDiagnosticsEnabled)
        {
            QMutexLocker locker(&m_managers->frameRateCountersMutex);
            ++m_managers->frameRateCounters[frame.cameraId].input;
        }
        if (processingToken == 0)
        {
            m_managers->imageProcessingManager->processFrameAsync(frame);
            return;
        }
        const QString cameraId = frame.cameraId;
        m_managers->imageProcessingManager->processFrameAsync(
            frame,
            processingToken,
            [this, cameraId, processingToken]()
            {
                return m_managers->cameraRuntimeControl->isProcessingFrameTokenCurrent(
                    cameraId,
                    processingToken);
            });
    }

    // Publishes every completed frame and coalesces main thread state updates
    void ScopeOneCore::handleProcessedFrame(const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return;
        }

        bool queueFlush = false;
        {
            QMutexLocker locker(&m_managers->processedDeliveryMutex);
            if (!m_managers->imageProcessingManager->isRealTimeProcessingEnabled())
            {
                return;
            }
            Managers::PendingProcessedFrame& pending =
                m_managers->pendingProcessedFrames[frame.cameraId];
            pending.frame = frame;
            ++pending.completedCount;
            if (!m_managers->processedFlushQueued)
            {
                m_managers->processedFlushQueued = true;
                queueFlush = true;
            }
        }
        if constexpr (kFrameRateDiagnosticsEnabled)
        {
            QMutexLocker locker(&m_managers->frameRateCountersMutex);
            ++m_managers->frameRateCounters[frame.cameraId].processed;
        }
        emit processedFrameReady(frame);

        if (queueFlush)
        {
            QMetaObject::invokeMethod(this,
                                      [this]() { schedulePreviewFlush(); },
                                      Qt::QueuedConnection);
        }
    }

    // Applies only the latest processed frame per camera on the main thread
    void ScopeOneCore::flushProcessedFrames()
    {
        QHash<QString, Managers::PendingProcessedFrame> frames;
        {
            QMutexLocker locker(&m_managers->processedDeliveryMutex);
            frames.swap(m_managers->pendingProcessedFrames);
            m_managers->processedFlushQueued = false;
        }

        for (auto it = frames.constBegin(); it != frames.constEnd(); ++it)
        {
            const ImageFrame& frame = it.value().frame;
            const QString layerKey = processedLayerKey(frame.cameraId);
            m_imageSceneModel->updateLayerFrame(layerKey, frame);
            m_frameGraph.publishLatest(FrameGraphStream::Processed, frame);
            emit processedFramesCompleted(frame.cameraId, it.value().completedCount);
            m_pendingPreviewProcessedFrames.insert(frame.cameraId, frame);
            scheduleHistogramStats(layerKey, frame);
            updateLineProfile(frame.cameraId, true, frame);
        }
    }

    // Queue the newest raw frame for the display path
    void ScopeOneCore::queuePreviewRawFrame(const ImageFrame& frame)
    {
        m_pendingPreviewRawFrames.insert(frame.cameraId, frame);
        schedulePreviewFlush();
    }

    // Schedule one latest frame display update without following camera rate
    void ScopeOneCore::schedulePreviewFlush()
    {
        if (m_previewFlushTimer->isActive())
        {
            return;
        }

        qint64 delayMs = 0;
        if (m_previewPublishTimer.isValid())
        {
            delayMs = qMax(qint64{0},
                           kPreviewRefreshIntervalMs - m_previewPublishTimer.elapsed());
        }
        m_previewFlushTimer->start(static_cast<int>(delayMs));
    }

    // Flush the newest raw and processed frames in one display update
    void ScopeOneCore::flushPreviewFrames()
    {
        m_previewPublishTimer.restart();
        flushProcessedFrames();
        QHash<QString, ImageFrame> rawFrames;
        QHash<QString, ImageFrame> processedFrames;
        rawFrames.swap(m_pendingPreviewRawFrames);
        processedFrames.swap(m_pendingPreviewProcessedFrames);
        for (auto it = rawFrames.constBegin(); it != rawFrames.constEnd(); ++it)
        {
            emit previewRawFrameReady(it.value());
        }
        for (auto it = processedFrames.constBegin(); it != processedFrames.constEnd(); ++it)
        {
            emit previewProcessedFrameReady(it.value());
        }
    }

    // Return the latest frame for one graph layer key
    ImageFrame ScopeOneCore::graphFrame(const QString& layerKey) const
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        const QString sourceId = sourceIdFromLayerKey(trimmedLayerKey).trimmed();
        if (sourceId.isEmpty())
        {
            return {};
        }

        if (isRawLayerKey(trimmedLayerKey))
        {
            return m_frameGraph.latest(FrameGraphStream::Raw, sourceId);
        }
        if (isProcessedLayerKey(trimmedLayerKey))
        {
            return m_frameGraph.latest(FrameGraphStream::Processed, sourceId);
        }
        if (isStaticLayerKey(trimmedLayerKey))
        {
            return m_frameGraph.latest(FrameGraphStream::Static, sourceId);
        }
        if (trimmedLayerKey.startsWith(QStringLiteral("external:")))
        {
            return m_frameGraph.latest(FrameGraphStream::External, sourceId);
        }
        return {};
    }

    // Return latest valid frames for several graph layer keys
    QList<ImageFrame> ScopeOneCore::graphFrames(const QStringList& layerKeys) const
    {
        QList<ImageFrame> frames;
        frames.reserve(layerKeys.size());
        for (const QString& layerKey : layerKeys)
        {
            ImageFrame frame = graphFrame(layerKey);
            if (frame.isValid())
            {
                frames.append(std::move(frame));
            }
        }
        return frames;
    }

    // Read one pixel from a named frame graph layer
    bool ScopeOneCore::graphPixelValue(const QString& layerKey, const QPoint& imagePos, int& value) const
    {
        return sampleFrameValue(graphFrame(layerKey), imagePos, value);
    }

    // Build a recording session from frames
    std::shared_ptr<ScopeOneCore::RecordingSessionData> ScopeOneCore::createFrameSession(
        const QList<ImageFrame>& frames,
        const ExperimentPlan& capturePlan)
    {
        ExperimentPlan plan = capturePlan;
        QStringList frameCameraIds;
        for (const ImageFrame& frame : frames)
        {
            const QString cameraId = frame.cameraId.trimmed();
            if (!cameraId.isEmpty() && !frameCameraIds.contains(cameraId))
            {
                frameCameraIds.append(cameraId);
            }
        }
        plan.pixelSizeUm = frameCameraIds.size() == 1 ? cameraPixelSizeUm(frameCameraIds.first()) : 0.0;
        plan.configPath = m_loadedConfigPath;
        plan.configSha256 = m_loadedConfigSha256;
        plan.processing = processingRecipe();
        const QJsonObject deviceProperties = buildDevicePropertyMetadata(*this);
        auto session = RecordingSessionData::fromImageFrames(frames, plan);
        if (session)
        {
            QHash<QString, double> pixelSizesUm;
            for (const QString& cameraId : session->cameraIds())
            {
                const double pixelSizeUm = cameraPixelSizeUm(cameraId);
                if (pixelSizeUm > 0.0)
                {
                    pixelSizesUm.insert(cameraId, pixelSizeUm);
                }
            }
            session->setCameraPixelSizesUm(pixelSizesUm);
            SoftwareSnapshot software;
            software.applicationVersion = QCoreApplication::applicationVersion();
            software.coreVersion = getVersion();
            software.mmCoreVersion = getMMCoreVersion();
            software.libTiffVersion = getLibTiffVersion();
            software.zlibVersion = getZlibVersion();
            software.operatingSystem = QSysInfo::prettyProductName();
            session->setSoftwareSnapshot(software);
            session->setDeviceProperties(deviceProperties);
            registerRecordingSession(session);
        }
        return session;
    }

    // Publish a static frame source to the central graph
    ImageFrame ScopeOneCore::publishStaticFrame(const QString& sourceId,
                                                const ImageFrame& frame,
                                                const QString& displayName)
    {
        if (!m_frameGraph.publishLatest(FrameGraphStream::Static, sourceId, frame))
        {
            return {};
        }
        const ImageFrame storedFrame = graphFrame(staticLayerKey(sourceId));
        const QString layerKey = staticLayerKey(storedFrame.cameraId);
        const DocumentLayerKind kind = storedFrame.cameraId.startsWith(QStringLiteral("gallery:"))
                                           ? DocumentLayerKind::Gallery
                                           : DocumentLayerKind::Static;
        ensureSceneLayer(layerKey,
                         storedFrame.cameraId,
                         displayName.trimmed().isEmpty() ? storedFrame.cameraId : displayName.trimmed(),
                         kind);
        m_imageSceneModel->setLayerName(
            layerKey,
            displayName.trimmed().isEmpty() ? storedFrame.cameraId : displayName.trimmed());
        m_imageSceneModel->updateLayerFrame(layerKey, storedFrame);
        m_imageSceneModel->setLayerVisible(layerKey, true);
        m_latestHistogramStats.remove(layerKey);
        scheduleHistogramStats(layerKey, storedFrame);
        if (!updateStaticLineProfile(storedFrame.cameraId, storedFrame))
        {
            clearLineProfile();
        }
        emit staticFramePublished(storedFrame.cameraId, displayName.trimmed(), storedFrame);
        return storedFrame;
    }

    // Publish an externally supplied frame to the central graph
    ImageFrame ScopeOneCore::publishExternalFrame(const QString& sourceId, const ImageFrame& frame)
    {
        if (!m_frameGraph.publishLatest(FrameGraphStream::External, sourceId, frame))
        {
            return {};
        }
        return graphFrame(QStringLiteral("external:%1").arg(sourceId.trimmed()));
    }

    // Remove one static frame graph source
    void ScopeOneCore::removeStaticFrame(const QString& sourceId)
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (trimmedSourceId.isEmpty())
        {
            return;
        }

        const QString layerKey = staticLayerKey(trimmedSourceId);
        m_frameGraph.remove(FrameGraphStream::Static, trimmedSourceId);
        m_imageSceneModel->removeLayer(layerKey);
        clearLayerAnalysis(layerKey);
        if (m_activeLineProfile.active
            && m_activeLineProfile.staticSource
            && m_activeLineProfile.sourceId == trimmedSourceId)
        {
            clearLineProfile();
        }
        emit staticFrameRemoved(trimmedSourceId);
    }

    // Clear all static frame graph sources
    void ScopeOneCore::clearStaticFrames()
    {
        m_frameGraph.clear(FrameGraphStream::Static);
        for (const QString& layerId : m_imageSceneModel->layerIds())
        {
            DocumentLayer layer;
            if (m_imageSceneModel->findLayer(layerId, layer)
                && (layer.kind == DocumentLayerKind::Static
                    || layer.kind == DocumentLayerKind::Gallery))
            {
                m_imageSceneModel->removeLayer(layerId);
            }
        }
        clearLayerAnalysisByPrefix(QStringLiteral("static:"));
        if (m_activeLineProfile.active && m_activeLineProfile.staticSource)
        {
            clearLineProfile();
        }
        emit staticFramesCleared();
    }

    // Clear derived analysis data for one layer
    void ScopeOneCore::clearLayerAnalysis(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty())
        {
            return;
        }

        m_latestHistogramStats.remove(trimmedLayerKey);
        m_histogramJobStates.remove(trimmedLayerKey);
        emit layerAnalysisCleared(trimmedLayerKey);
    }

    // Clear derived analysis data for one graph stream
    void ScopeOneCore::clearLayerAnalysisByPrefix(const QString& prefix)
    {
        const QString trimmedPrefix = prefix.trimmed();
        if (trimmedPrefix.isEmpty())
        {
            return;
        }

        QStringList clearedKeys;
        for (const QString& key : m_latestHistogramStats.keys())
        {
            if (key.startsWith(trimmedPrefix))
            {
                m_latestHistogramStats.remove(key);
                if (!clearedKeys.contains(key))
                {
                    clearedKeys.append(key);
                }
            }
        }
        for (const QString& key : m_histogramJobStates.keys())
        {
            if (key.startsWith(trimmedPrefix))
            {
                m_histogramJobStates.remove(key);
                if (!clearedKeys.contains(key))
                {
                    clearedKeys.append(key);
                }
            }
        }
        for (const QString& key : clearedKeys)
        {
            emit layerAnalysisCleared(key);
        }
    }

    // Clear stale live frames for a camera source
    void ScopeOneCore::clearLiveFrames(const QString& cameraId)
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty())
        {
            return;
        }

        m_frameGraph.remove(FrameGraphStream::Raw, trimmedCameraId);
        m_frameGraph.remove(FrameGraphStream::Processed, trimmedCameraId);
        {
            QMutexLocker locker(&m_managers->processedDeliveryMutex);
            m_managers->pendingProcessedFrames.remove(trimmedCameraId);
        }
        m_pendingPreviewRawFrames.remove(trimmedCameraId);
        m_pendingPreviewProcessedFrames.remove(trimmedCameraId);
        const QString rawLayerKey = histogramLayerKey(trimmedCameraId, false);
        const QString processedLayerKey = histogramLayerKey(trimmedCameraId, true);
        m_imageSceneModel->clear(rawLayerKey);
        m_imageSceneModel->clear(processedLayerKey);
        clearLayerAnalysis(rawLayerKey);
        clearLayerAnalysis(processedLayerKey);
        if (m_activeLineProfile.active
            && !m_activeLineProfile.staticSource
            && m_activeLineProfile.sourceId == trimmedCameraId)
        {
            clearLineProfile();
        }
        emit liveFramesCleared(trimmedCameraId);
    }

    // Clear all processed graph frames
    void ScopeOneCore::clearProcessedFrames()
    {
        {
            QMutexLocker locker(&m_managers->processedDeliveryMutex);
            m_managers->pendingProcessedFrames.clear();
            m_managers->processedFlushQueued = false;
        }
        m_frameGraph.clear(FrameGraphStream::Processed);
        m_pendingPreviewProcessedFrames.clear();
        for (const QString& cameraId : m_cameraIds)
        {
            const QString layerKey = processedLayerKey(cameraId);
            m_imageSceneModel->clear(layerKey);
            m_imageSceneModel->setLayerVisible(layerKey, false);
        }
        clearLayerAnalysisByPrefix(QStringLiteral("proc:"));
        if (m_activeLineProfile.active
            && !m_activeLineProfile.staticSource
            && m_activeLineProfile.processed)
        {
            clearLineProfile();
        }
        emit processedFramesCleared();
    }

    // Compute public histogram statistics for a frame
    bool ScopeOneCore::computeHistogramStats(const ImageFrame& frame, HistogramStats& stats)
    {
        return computeHistogramStatsInternal(frame, stats);
    }

    // Return cached raw statistics or compute them from the latest frame
    bool ScopeOneCore::getRawImageStatistics(const QString& cameraId, HistogramStats& stats) const
    {
        const auto cached = m_latestHistogramStats.constFind(histogramLayerKey(cameraId, false));
        if (cached != m_latestHistogramStats.constEnd() && cached.value().hasData())
        {
            stats = cached.value();
            return true;
        }

        const ImageFrame frame = graphFrame(rawLayerKey(cameraId));
        if (!frame.isValid())
        {
            return false;
        }
        return computeHistogramStats(frame, stats);
    }

    // Return histogram statistics for any current frame graph layer
    bool ScopeOneCore::getLayerHistogram(const QString& layerKey, HistogramStats& stats) const
    {
        const QString key = layerKey.trimmed();
        const auto cached = m_latestHistogramStats.constFind(key);
        if (cached != m_latestHistogramStats.constEnd() && cached->hasData())
        {
            stats = cached.value();
            return true;
        }
        return computeHistogramStats(graphFrame(key), stats);
    }

    // Select the layer whose live histogram is consumed by the frontend
    void ScopeOneCore::setActiveHistogramLayer(const QString& layerKey)
    {
        const QString key = layerKey.trimmed();
        if (m_activeHistogramLayerKey == key)
        {
            return;
        }

        const QString previousKey = m_activeHistogramLayerKey;
        m_activeHistogramLayerKey = key;
        if (!previousKey.isEmpty()
            && !m_imageSceneModel->layerAutoStretchEnabled(previousKey))
        {
            m_latestHistogramStats.remove(previousKey);
            auto it = m_histogramJobStates.find(previousKey);
            if (it != m_histogramJobStates.end())
            {
                it->queuedFrame = ImageFrame{};
            }
        }
        if (!key.isEmpty() && !m_imageSceneModel->layerAutoStretchEnabled(key))
        {
            m_latestHistogramStats.remove(key);
        }

        const ImageFrame frame = graphFrame(key);
        if (frame.isValid())
        {
            scheduleHistogramStats(key, frame);
        }
    }

    bool ScopeOneCore::autoLayerLevels(const QString& layerKey)
    {
        const QString key = layerKey.trimmed();
        HistogramStats stats;
        if (!computeHistogramStats(graphFrame(key), stats))
        {
            return false;
        }
        m_latestHistogramStats.insert(key, stats);
        return m_imageSceneModel->setLayerDisplayLevels(
            key, stats.autoMinLevel, stats.autoMaxLevel, stats.maxValue);
    }

    bool ScopeOneCore::fullLayerLevels(const QString& layerKey)
    {
        const QString key = layerKey.trimmed();
        const ImageFrame frame = graphFrame(key);
        if (!frame.isValid())
        {
            return false;
        }
        const int maxValue = frame.maxValue();
        m_imageSceneModel->setLayerAutoStretchEnabled(key, false);
        return m_imageSceneModel->setLayerDisplayLevels(key, 0, maxValue, maxValue);
    }

    bool ScopeOneCore::setLayerAutoStretchEnabled(const QString& layerKey, bool enabled)
    {
        const QString key = layerKey.trimmed();
        if (!m_imageSceneModel->setLayerAutoStretchEnabled(key, enabled))
        {
            return false;
        }
        if (enabled)
        {
            autoLayerLevels(key);
            const ImageFrame frame = graphFrame(key);
            if (frame.isValid())
            {
                scheduleHistogramStats(key, frame);
            }
        }
        else if (m_activeHistogramLayerKey != key)
        {
            auto it = m_histogramJobStates.find(key);
            if (it != m_histogramJobStates.end())
            {
                it->queuedFrame = ImageFrame{};
            }
        }
        return true;
    }

    bool ScopeOneCore::layerAutoStretchEnabled(const QString& layerKey) const
    {
        return m_imageSceneModel->layerAutoStretchEnabled(layerKey);
    }

    // Sample an image-space line from any current frame graph layer
    bool ScopeOneCore::getLineProfile(const QString& layerKey,
                                      const QPoint& start,
                                      const QPoint& end,
                                      QVector<int>& values) const
    {
        values.clear();
        const ImageFrame frame = graphFrame(layerKey);
        if (!frame.isValid())
        {
            return false;
        }
        return sampleLine(start, end, values,
                          [&frame](const QPoint& point, int& value)
                          {
                              return sampleFrameValue(frame, point, value);
                          });
    }

    // Runs particle detection on the analysis worker
    quint64 ScopeOneCore::detectParticles(const QString& layerKey,
                                          int threshold,
                                          int minArea,
                                          int maxArea,
                                          int maxParticles)
    {
        const QString key = layerKey.trimmed();
        return detectParticles(graphFrame(key), key,
                               threshold, minArea, maxArea, maxParticles);
    }

    quint64 ScopeOneCore::detectParticles(const ImageFrame& frame,
                                          const QString& resultLayerKey,
                                          int threshold,
                                          int minArea,
                                          int maxArea,
                                          int maxParticles)
    {
        const QString key = resultLayerKey.trimmed();
        if (key.isEmpty() || !frame.isValid())
        {
            return 0;
        }

        const quint64 requestId = ++m_nextAnalysisRequestId;
        const quint64 generation = m_analysisGeneration;
        auto* watcher = new QFutureWatcher<ParticleDetectionResult>(this);
        connect(watcher, &QFutureWatcher<ParticleDetectionResult>::finished,
                this, [this, watcher, requestId, key, generation]()
        {
            const ParticleDetectionResult result = watcher->result();
            QString error;
            if (generation != m_analysisGeneration)
            {
                error = QStringLiteral("Particle analysis invalidated by configuration change");
            }
            else if (!result.mask.isValid())
            {
                error = QStringLiteral("Particle analysis failed");
            }
            emit particleDetectionFinished(requestId, key, result, error);
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(
            m_analysisThreadPool.get(),
            [frame, threshold, minArea, maxArea, maxParticles]()
            {
                ParticleDetectionResult result;
                internal::detectParticles(frame,
                                          threshold,
                                          minArea,
                                          maxArea,
                                          result,
                                          maxParticles);
                return result;
            }));
        return requestId;
    }

    bool ScopeOneCore::startStageMosaic(const StageMosaicPlan& plan,
                                        QString* errorMessage)
    {
        if (m_configurationOperationRunning || m_pendingStageCommands > 0)
        {
            if (errorMessage)
            {
                *errorMessage = m_configurationOperationRunning
                                    ? QStringLiteral("A configuration operation is running")
                                    : QStringLiteral("A stage command is running");
            }
            return false;
        }
        StageMosaicPlan effectivePlan = plan;
        effectivePlan.pixelSizeUm = cameraPixelSizeUm(plan.cameraId);
        if (effectivePlan.pixelSizeUm <= 0.0)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Camera scale is not set");
            }
            return false;
        }
        return m_managers->stageMosaicManager->start(effectivePlan, errorMessage);
    }

    void ScopeOneCore::cancelStageMosaic()
    {
        m_managers->stageMosaicManager->cancel();
    }

    bool ScopeOneCore::isStageMosaicRunning() const
    {
        return m_managers->stageMosaicManager->isRunning();
    }

    ScopeOneCore::StageMosaicStatus ScopeOneCore::stageMosaicStatus() const
    {
        return m_managers->stageMosaicManager->status();
    }

    // Check whether one layer currently needs continuously refreshed statistics
    bool ScopeOneCore::histogramUpdatesEnabled(const QString& layerKey) const
    {
        const QString key = layerKey.trimmed();
        return !key.isEmpty()
            && (key == m_activeHistogramLayerKey
                || m_imageSceneModel->layerAutoStretchEnabled(key));
    }

    // Schedule rate limited histogram work for one graph layer
    void ScopeOneCore::scheduleHistogramStats(const QString& layerKey, const ImageFrame& frame)
    {
        const QString key = layerKey.trimmed();
        if (key.isEmpty() || !frame.isValid())
        {
            return;
        }
        if (!histogramUpdatesEnabled(key))
        {
            m_latestHistogramStats.remove(key);
            return;
        }

        HistogramJobState& state = m_histogramJobStates[key];
        if (state.inFlight)
        {
            state.queuedFrame = frame;
            return;
        }

        const qint64 elapsedMs = state.lastScheduledTimer.isValid()
                                     ? state.lastScheduledTimer.elapsed()
                                     : kHistogramRefreshIntervalMs;
        if (elapsedMs < kHistogramRefreshIntervalMs)
        {
            state.queuedFrame = frame;
            if (!state.retryScheduled)
            {
                state.retryScheduled = true;
                const int delayMs = static_cast<int>(
                    (std::max)(qint64{1}, kHistogramRefreshIntervalMs - elapsedMs));
                QTimer::singleShot(delayMs, this, [this, key]()
                {
                    auto it = m_histogramJobStates.find(key);
                    if (it == m_histogramJobStates.end())
                    {
                        return;
                    }
                    it->retryScheduled = false;
                    const ImageFrame queuedFrame = it->queuedFrame;
                    it->queuedFrame = ImageFrame{};
                    if (queuedFrame.isValid())
                    {
                        scheduleHistogramStats(key, queuedFrame);
                    }
                });
            }
            return;
        }

        state.inFlight = true;
        state.queuedFrame = ImageFrame{};
        state.lastScheduledTimer.start();
        const quint64 sequence = ++m_nextHistogramSequence;
        state.activeSequence = sequence;

        auto* watcher = new QFutureWatcher<HistogramStats>(this);
        connect(watcher, &QFutureWatcher<HistogramStats>::finished, this,
                [this, watcher, key, sequence]()
                {
                    HistogramStats stats = watcher->result();
                    ImageFrame queuedFrame;

                    auto it = m_histogramJobStates.find(key);
                    if (it != m_histogramJobStates.end() && it->activeSequence == sequence)
                    {
                        it->inFlight = false;
                        if (histogramUpdatesEnabled(key) && stats.hasData())
                        {
                            m_latestHistogramStats.insert(key, stats);
                            if (m_imageSceneModel->layerAutoStretchEnabled(key))
                            {
                                m_imageSceneModel->setLayerDisplayLevels(
                                    key, stats.autoMinLevel, stats.autoMaxLevel, stats.maxValue);
                            }
                            if (isRawLayerKey(key) || isProcessedLayerKey(key))
                            {
                                emit imageHistogramReady(
                                    sourceIdFromLayerKey(key), isProcessedLayerKey(key), stats);
                            }
                            emit layerHistogramReady(key, stats);
                        }
                        if (histogramUpdatesEnabled(key) && it->queuedFrame.isValid())
                        {
                            queuedFrame = it->queuedFrame;
                        }
                        it->queuedFrame = ImageFrame{};
                    }
                    watcher->deleteLater();

                    if (queuedFrame.isValid())
                    {
                        scheduleHistogramStats(key, queuedFrame);
                    }
                });
        watcher->setFuture(QtConcurrent::run(m_histogramThreadPool.get(), [frame]()
        {
            HistogramStats stats;
            ScopeOneCore::computeHistogramStats(frame, stats);
            return stats;
        }));
    }

    // Emit a line profile when the active request matches this frame
    void ScopeOneCore::updateLineProfile(const QString& cameraId,
                                         bool processed,
                                         const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return;
        }

        if (!m_activeLineProfile.active
            || m_activeLineProfile.staticSource
            || m_activeLineProfile.processed != processed
            || m_activeLineProfile.sourceId != cameraId)
        {
            return;
        }
        if (m_lineProfileUpdateTimer.isValid()
            && m_lineProfileUpdateTimer.elapsed() < kLineProfileRefreshIntervalMs)
        {
            return;
        }
        m_lineProfileUpdateTimer.restart();

        QVector<int> values;
        if (!sampleLine(m_activeLineProfile.start, m_activeLineProfile.end, values,
                        [&](const QPoint& point, int& value)
                        {
                            return sampleFrameValue(frame, point, value);
                        }))
        {
            return;
        }

        const QString layerKey = histogramLayerKey(cameraId, processed);
        emit lineProfileUpdated(cameraId, processed, values);
        emit layerLineProfileUpdated(layerKey, values);
    }

    // Emit a line profile when the active request matches this static source
    bool ScopeOneCore::updateStaticLineProfile(const QString& sourceId, const ImageFrame& frame)
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (!m_activeLineProfile.active
            || !m_activeLineProfile.staticSource
            || m_activeLineProfile.sourceId != trimmedSourceId)
        {
            return true;
        }
        if (!frame.isValid())
        {
            return false;
        }

        QVector<int> values;
        if (!sampleLine(m_activeLineProfile.start, m_activeLineProfile.end, values,
                        [&](const QPoint& point, int& value)
                        {
                            return sampleFrameValue(frame, point, value);
                        }))
        {
            return false;
        }

        emit layerLineProfileUpdated(staticLayerKey(trimmedSourceId), values);
        return true;
    }

    QStringList ScopeOneCore::xyStageDevices() const
    {
        if (m_configurationOperationRunning)
        {
            return {};
        }
        QStringList devices;
        for (const HardwareDeviceDescriptor& device : hardwareDevices())
        {
            if (device.kind == HardwareDeviceKind::XYStage)
            {
                devices.append(device.logicalId);
            }
        }
        return devices;
    }

    QStringList ScopeOneCore::zStageDevices() const
    {
        if (m_configurationOperationRunning)
        {
            return {};
        }
        QStringList devices;
        for (const HardwareDeviceDescriptor& device : hardwareDevices())
        {
            if (device.kind == HardwareDeviceKind::ZStage)
            {
                devices.append(device.logicalId);
            }
        }
        return devices;
    }

    QString ScopeOneCore::currentXYStageDevice() const
    {
        if (m_configurationOperationRunning)
        {
            return {};
        }
        return m_managers->hardwareRuntime->defaultXYStage();
    }

    QString ScopeOneCore::currentFocusDevice() const
    {
        if (m_configurationOperationRunning)
        {
            return {};
        }
        return m_managers->hardwareRuntime->defaultZStage();
    }

    // Read the current XY stage position through its provider
    bool ScopeOneCore::readXYPosition(const QString& xyStageLabel, double& x, double& y) const
    {
        x = 0.0;
        y = 0.0;
        const QString label = xyStageLabel.trimmed();
        if (m_configurationOperationRunning || label.isEmpty())
        {
            return false;
        }
        return m_managers->hardwareRuntime->getXYPosition(label, x, y, nullptr);
    }

    // Read the current Z stage position through its provider
    bool ScopeOneCore::readZPosition(const QString& zStageLabel, double& z) const
    {
        z = 0.0;
        const QString label = zStageLabel.trimmed();
        if (m_configurationOperationRunning || label.isEmpty())
        {
            return false;
        }
        return m_managers->hardwareRuntime->getZPosition(label, z, nullptr);
    }

    // Queues one stage command on the serialized hardware worker
    quint64 ScopeOneCore::queueStageMove(
        const QString& deviceLabel,
        std::function<bool(QString*)> command)
    {
        const QString label = deviceLabel.trimmed();
        if (label.isEmpty()
            || !command
            || m_configurationOperationRunning
            || isRecording()
            || !m_managers->activeExperimentId.isEmpty())
        {
            return 0;
        }

        const quint64 commandId = ++m_nextStageCommandId;
        ++m_pendingStageCommands;
        auto* watcher = new QFutureWatcher<StageTaskResult>(this);
        connect(watcher, &QFutureWatcher<StageTaskResult>::finished,
                this, [this, watcher, commandId, label]()
        {
            const StageTaskResult task = watcher->result();
            --m_pendingStageCommands;
            if (task.success)
            {
                emit stagePositionChanged();
            }
            emit stageMoveFinished(commandId, label, task.success, task.errorMessage);
            watcher->deleteLater();
        });

        watcher->setFuture(QtConcurrent::run(
            m_hardwareThreadPool.get(),
            [command = std::move(command)]()
            {
                StageTaskResult task;
                task.success = command(&task.errorMessage);
                return task;
            }));
        return commandId;
    }

    quint64 ScopeOneCore::moveXYRelative(const QString& xyStageLabel, double dx, double dy)
    {
        HardwareRuntime* const runtime = m_managers->hardwareRuntime;
        const QString device = xyStageLabel.trimmed();
        return queueStageMove(device, [runtime, device, dx, dy](QString* errorMessage)
        {
            return runtime->setRelativeXYPosition(device, dx, dy, errorMessage);
        });
    }

    quint64 ScopeOneCore::moveZRelative(const QString& zStageLabel, double dz)
    {
        HardwareRuntime* const runtime = m_managers->hardwareRuntime;
        const QString device = zStageLabel.trimmed();
        return queueStageMove(device, [runtime, device, dz](QString* errorMessage)
        {
            return runtime->setRelativeZPosition(device, dz, errorMessage);
        });
    }

    quint64 ScopeOneCore::moveXYTo(const QString& xyStageLabel, double x, double y)
    {
        HardwareRuntime* const runtime = m_managers->hardwareRuntime;
        const QString device = xyStageLabel.trimmed();
        return queueStageMove(device, [runtime, device, x, y](QString* errorMessage)
        {
            return runtime->setXYPosition(device, x, y, errorMessage);
        });
    }

    quint64 ScopeOneCore::moveZTo(const QString& zStageLabel, double z)
    {
        HardwareRuntime* const runtime = m_managers->hardwareRuntime;
        const QString device = zStageLabel.trimmed();
        return queueStageMove(device, [runtime, device, z](QString* errorMessage)
        {
            return runtime->setZPosition(device, z, errorMessage);
        });
    }

    bool ScopeOneCore::readShutterOpen(const QString& shutterLabel, bool& open) const
    {
        open = false;
        const QString device = shutterLabel.trimmed();
        return !m_configurationOperationRunning
            && !device.isEmpty()
            && m_managers->hardwareRuntime->isShutterOpen(device, open, nullptr);
    }

    bool ScopeOneCore::setShutterOpen(const QString& shutterLabel,
                                      bool open,
                                      QString* errorMessage)
    {
        if (errorMessage)
        {
            errorMessage->clear();
        }
        const QString device = shutterLabel.trimmed();
        if (m_configurationOperationRunning || device.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Invalid shutter target");
            }
            return false;
        }
        const bool ok = m_managers->hardwareRuntime->setShutterOpen(
            device, open, errorMessage);
        if (ok)
        {
            emit deviceStateChanged();
        }
        return ok;
    }

    bool ScopeOneCore::readDeviceState(const QString& deviceLabel, long& state) const
    {
        state = 0;
        const QString device = deviceLabel.trimmed();
        return !m_configurationOperationRunning
            && !device.isEmpty()
            && m_managers->hardwareRuntime->getState(device, state, nullptr);
    }

    bool ScopeOneCore::setDeviceState(const QString& deviceLabel,
                                      long state,
                                      QString* errorMessage)
    {
        if (errorMessage)
        {
            errorMessage->clear();
        }
        const QString device = deviceLabel.trimmed();
        if (m_configurationOperationRunning || device.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Invalid state device target");
            }
            return false;
        }
        const bool ok = m_managers->hardwareRuntime->setState(device, state, errorMessage);
        if (ok)
        {
            emit deviceStateChanged();
        }
        return ok;
    }

    QString ScopeOneCore::deviceStateLabel(const QString& deviceLabel, long state) const
    {
        const QString device = deviceLabel.trimmed();
        return !m_configurationOperationRunning && !device.isEmpty()
                   ? m_managers->hardwareRuntime->stateLabel(device, state)
                   : QString{};
    }

    // List configuration groups exposed by registered providers
    QStringList ScopeOneCore::availableConfigGroups() const
    {
        if (m_configurationOperationRunning)
        {
            return {};
        }
        return m_managers->hardwareRuntime->availableConfigGroups();
    }

    // List presets in a provider configuration group
    QStringList ScopeOneCore::availableConfigs(const QString& configGroup) const
    {
        if (m_configurationOperationRunning || configGroup.isEmpty())
        {
            return {};
        }
        return m_managers->hardwareRuntime->availableConfigs(configGroup);
    }

    // Read the current preset for a configuration group
    QString ScopeOneCore::currentConfig(const QString& groupName) const
    {
        if (m_configurationOperationRunning || groupName.isEmpty())
        {
            return {};
        }
        return m_managers->hardwareRuntime->currentConfig(groupName);
    }

    // Apply a configuration preset while camera previews are paused
    bool ScopeOneCore::setConfig(const QString& groupName,
                                 const QString& configName,
                                 QString* errorMessage)
    {
        if (errorMessage)
        {
            errorMessage->clear();
        }
        if (m_configurationOperationRunning
            || m_pendingStageCommands > 0
            || groupName.isEmpty()
            || configName.isEmpty())
        {
            if (errorMessage)
            {
                if (m_configurationOperationRunning)
                {
                    *errorMessage = QStringLiteral("A configuration operation is running");
                }
                else if (m_pendingStageCommands > 0)
                {
                    *errorMessage = QStringLiteral("A stage command is running");
                }
                else
                {
                    *errorMessage = QStringLiteral("Invalid config preset");
                }
            }
            return false;
        }
        const QStringList runningPreviewIds = runningPreviewCameraIds();
        const bool ok = withSuspendedPreviews(this, runningPreviewIds, [&]()
        {
            return m_managers->hardwareRuntime->setConfig(groupName,
                                                          configName,
                                                          errorMessage);
        });
        if (ok)
        {
            emit deviceStateChanged();
        }
        return ok;
    }

    // Read exposure from the active camera path
    bool ScopeOneCore::readExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        exposureMs = 0.0;
        const QString target = cameraIdOrAll.trimmed();
        if (m_configurationOperationRunning || target.isEmpty())
        {
            return false;
        }

        return m_managers->cameraProvider->getExposure(target, exposureMs);
    }

    // List devices from the unified registry
    QStringList ScopeOneCore::loadedDevices() const
    {
        if (m_configurationOperationRunning)
        {
            return {};
        }
        QStringList devices;
        for (const HardwareDeviceDescriptor& device : hardwareDevices())
        {
            devices.append(device.logicalId);
        }
        return devices;
    }

    // Build one property snapshot list for the UI
    QList<ScopeOneCore::DevicePropertyInfo> ScopeOneCore::deviceProperties(const QString& deviceLabel,
                                                                           bool fromCache) const
    {
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

    // List property names through the matching device backend
    QStringList ScopeOneCore::devicePropertyNames(const QString& deviceLabel) const
    {
        const QString device = deviceLabel.trimmed();
        if (m_configurationOperationRunning || device.isEmpty())
        {
            return {};
        }
        return m_managers->hardwareRuntime->listProperties(device);
    }

    // Read a property value from hardware or cache
    QString ScopeOneCore::getPropertyValue(const QString& deviceLabel, const QString& name, bool fromCache) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning || device.isEmpty() || property.isEmpty())
        {
            return {};
        }
        return m_managers->hardwareRuntime->getProperty(device, property, fromCache);
    }

    // Convert backend property types into UI strings
    QString ScopeOneCore::propertyTypeString(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning || device.isEmpty() || property.isEmpty())
        {
            return QStringLiteral("Unknown");
        }
        return m_managers->hardwareRuntime->getPropertyType(device, property);
    }

    // Check whether a property can be edited
    bool ScopeOneCore::isPropertyReadOnly(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning || device.isEmpty() || property.isEmpty())
        {
            return true;
        }
        return m_managers->hardwareRuntime->isPropertyReadOnly(device, property);
    }

    // Check whether a property must be set before initialization
    bool ScopeOneCore::isPropertyPreInit(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning || device.isEmpty() || property.isEmpty())
        {
            return false;
        }
        return m_managers->hardwareRuntime->isPropertyPreInit(device, property);
    }

    // Return allowed values for enumerated properties
    QStringList ScopeOneCore::getAllowedPropertyValues(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning || device.isEmpty() || property.isEmpty())
        {
            return {};
        }
        return m_managers->hardwareRuntime->getAllowedPropertyValues(device, property);
    }

    // Return numeric limits for range constrained properties
    bool ScopeOneCore::getPropertyLimits(const QString& deviceLabel,
                                         const QString& name,
                                         double& lower,
                                         double& upper) const
    {
        lower = 0.0;
        upper = 0.0;

        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning || device.isEmpty() || property.isEmpty())
        {
            return false;
        }
        if (!m_managers->hardwareRuntime->hasPropertyLimits(device, property))
        {
            return false;
        }
        lower = m_managers->hardwareRuntime->getPropertyLowerLimit(device, property);
        upper = m_managers->hardwareRuntime->getPropertyUpperLimit(device, property);
        return true;
    }

    // Set a property and refresh backend state after the device accepts it
    bool ScopeOneCore::setPropertyValue(const QString& deviceLabel,
                                        const QString& name,
                                        const QString& value,
                                        QString* errorMessage)
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (m_configurationOperationRunning
            || m_pendingStageCommands > 0
            || device.isEmpty()
            || property.isEmpty())
        {
            if (errorMessage)
            {
                if (m_configurationOperationRunning)
                {
                    *errorMessage = QStringLiteral("A configuration operation is running");
                }
                else if (m_pendingStageCommands > 0)
                {
                    *errorMessage = QStringLiteral("A stage command is running");
                }
                else
                {
                    *errorMessage = QStringLiteral("Invalid property target");
                }
            }
            return false;
        }

        const bool ok = m_managers->hardwareRuntime->setProperty(
            device, property, value, errorMessage);
        if (ok)
        {
            emit deviceStateChanged();
        }
        return ok;
    }

    bool ScopeOneCore::isRealTimeProcessingEnabled() const
    {
        return m_managers->imageProcessingManager->isRealTimeProcessingEnabled();
    }

    // Toggle live processing without changing the module list
    bool ScopeOneCore::setRealTimeProcessingEnabled(bool enabled)
    {
        if (m_configurationOperationRunning)
        {
            return false;
        }
        if (enabled && m_managers->imageProcessingManager->definition().moduleCount() == 0)
        {
            return false;
        }
        if constexpr (kFrameRateDiagnosticsEnabled)
        {
            QMutexLocker locker(&m_managers->frameRateCountersMutex);
            m_managers->frameRateCounters.clear();
            m_managers->frameRateTimer.restart();
        }
        if (m_managers->imageProcessingManager->isRealTimeProcessingEnabled() == enabled)
        {
            if (!m_managers->cameraRuntimeControl->setHighRateFrameDeliveryEnabled(
                    m_cameraIds, enabled))
            {
                return false;
            }
            if (!enabled)
            {
                clearProcessedFrames();
            }
            return true;
        }
        if (enabled)
        {
            m_managers->imageProcessingManager->enableRealTimeProcessing(true);
            if (!m_managers->cameraRuntimeControl->setHighRateFrameDeliveryEnabled(
                    m_cameraIds, true))
            {
                m_managers->imageProcessingManager->enableRealTimeProcessing(false);
                return false;
            }
        }
        else
        {
            if (!m_managers->cameraRuntimeControl->setHighRateFrameDeliveryEnabled(
                    m_cameraIds, false))
            {
                return false;
            }
            m_managers->imageProcessingManager->enableRealTimeProcessing(false);
            clearProcessedFrames();
        }
        emit processingSettingsChanged();
        return true;
    }

    // Return the configured processing precision exposed to the UI
    ScopeOneCore::ProcessingBitDepth ScopeOneCore::processingBitDepth() const
    {
        return m_managers->imageProcessingManager->processingBitDepth() >= 16
                   ? ProcessingBitDepth::Bit16
                   : ProcessingBitDepth::Bit8;
    }

    // Change processing precision and rebuild runtime pipelines
    bool ScopeOneCore::setProcessingBitDepth(ProcessingBitDepth bitDepth)
    {
        if (isRealTimeProcessingEnabled())
        {
            return false;
        }
        const int nextBitDepth = bitDepth == ProcessingBitDepth::Bit16 ? 16 : 8;
        if (m_managers->imageProcessingManager->processingBitDepth() == nextBitDepth)
        {
            return true;
        }

        m_managers->imageProcessingManager->setProcessingBitDepth(nextBitDepth);
        emit processingSettingsChanged();
        return true;
    }

    QString ScopeOneCore::realTimeProcessingSource() const
    {
        return m_realTimeProcessingSource;
    }

    bool ScopeOneCore::setRealTimeProcessingSource(const QString& cameraId)
    {
        const QString source = cameraId.trimmed();
        if (isRealTimeProcessingEnabled()
            || (!source.isEmpty() && !m_cameraIds.contains(source)))
        {
            return false;
        }
        if (m_realTimeProcessingSource == source)
        {
            return true;
        }
        m_realTimeProcessingSource = source;
        emit processingSettingsChanged();
        return true;
    }

    // Captures the ordered processing pipeline as a replayable recipe
    ProcessingRecipe ScopeOneCore::processingRecipe() const
    {
        ProcessingRecipe recipe;
        recipe.bitDepth = processingBitDepth();
        const QList<ProcessingModuleInfo> modules = processingModules();
        recipe.modules.reserve(modules.size());
        for (const ProcessingModuleInfo& module : modules)
        {
            ProcessingModuleRecipe entry;
            entry.moduleId = module.id();
            entry.schemaVersion = module.descriptor().schemaVersion;
            entry.parameters = module.parameters();
            recipe.modules.append(std::move(entry));
        }
        return recipe;
    }

    // Replaces the current pipeline from a validated recipe
    bool ScopeOneCore::applyProcessingRecipe(const ProcessingRecipe& recipe, QString* errorMessage)
    {
        if (errorMessage) errorMessage->clear();
        if (isRealTimeProcessingEnabled())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Stop real-time processing before editing the pipeline");
            return false;
        }
        if (recipe.bitDepth != ProcessingBitDepth::Bit8
            && recipe.bitDepth != ProcessingBitDepth::Bit16)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Unsupported processing bit depth");
            return false;
        }

        std::vector<std::unique_ptr<ProcessingModule>> modules;
        modules.reserve(static_cast<size_t>(recipe.modules.size()));
        for (const ProcessingModuleRecipe& entry : recipe.modules)
        {
            const ProcessingModuleDescriptor descriptor =
                m_managers->processingModuleRegistry->descriptor(entry.moduleId);
            if (descriptor.id.isEmpty())
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Unsupported processing module: %1")
                                        .arg(entry.moduleId);
                }
                return false;
            }
            if (entry.schemaVersion != descriptor.schemaVersion)
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral(
                        "Unsupported schema version %1 for processing module %2; expected %3")
                                        .arg(entry.schemaVersion)
                                        .arg(entry.moduleId)
                                        .arg(descriptor.schemaVersion);
                }
                return false;
            }
            std::unique_ptr<ProcessingModule> module =
                m_managers->processingModuleRegistry->create(entry.moduleId);
            if (!module)
            {
                if (errorMessage) *errorMessage = QStringLiteral("Failed to create processing module: %1")
                                                    .arg(entry.moduleId);
                return false;
            }
            module->setParameters(entry.parameters);
            if (!equalCanonicalParameters(module->parameters(), entry.parameters))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral(
                        "Processing module %1 (%2) parameters are not canonical; a key or value was ignored, clamped, converted, or normalized")
                                        .arg(static_cast<qulonglong>(modules.size()))
                                        .arg(module->name());
                }
                return false;
            }
            modules.push_back(std::move(module));
        }

        ProcessingPipelineDefinition& definition = m_managers->imageProcessingManager->definition();
        while (definition.moduleCount() > 0)
        {
            definition.removeModule(definition.moduleCount() - 1);
        }
        for (auto& module : modules)
        {
            definition.addModule(std::move(module));
        }
        m_managers->imageProcessingManager->setProcessingBitDepth(
            recipe.bitDepth == ProcessingBitDepth::Bit16 ? 16 : 8);
        m_managers->imageProcessingManager->clearRuntimePipelines();
        if (definition.moduleCount() == 0)
        {
            m_managers->imageProcessingManager->enableRealTimeProcessing(false);
        }
        emit processingModulesChanged();
        emit processingSettingsChanged();
        return true;
    }

    // Run one frame through the runtime pipeline synchronously
    ImageFrame ScopeOneCore::processFrame(const ImageFrame& frame) const
    {
        if (!frame.isValid())
        {
            return frame;
        }

        return m_managers->imageProcessingManager->processFrame(frame);
    }

    // Continue processing one frame from a pipeline module index
    ImageFrame ScopeOneCore::processFrameFrom(int startModuleIndex, const ImageFrame& frame) const
    {
        if (!frame.isValid())
        {
            return frame;
        }

        return m_managers->imageProcessingManager->processFrameFrom(startModuleIndex, frame);
    }

    // Process one frame through a pipeline module index
    ImageFrame ScopeOneCore::processFrameThrough(int endModuleIndex, const ImageFrame& frame) const
    {
        if (!frame.isValid())
        {
            return frame;
        }

        return m_managers->imageProcessingManager->processFrameThrough(endModuleIndex, frame);
    }

    // Lists every registered processing module type
    QList<ProcessingModuleDescriptor> ScopeOneCore::availableProcessingModules() const
    {
        return m_managers->processingModuleRegistry->descriptors();
    }

    // Export processing module descriptions for the UI
    QList<scopeone::core::ScopeOneCore::ProcessingModuleInfo> ScopeOneCore::processingModules() const
    {
        QList<ProcessingModuleInfo> out;
        ProcessingPipelineDefinition& definition = m_managers->imageProcessingManager->definition();

        out.reserve(definition.moduleCount());
        definition.forEachModule([this, &out](const ProcessingModule* module)
        {
            ProcessingModuleInfo info;
            info.setId(module->id());
            info.setName(module->name());
            info.setParameters(module->parameters());
            info.setDescriptor(m_managers->processingModuleRegistry->descriptor(module->id()));
            out.append(std::move(info));
        });
        return out;
    }

    // Add a processing module to the editable pipeline
    bool ScopeOneCore::addProcessingModule(const QString& moduleId)
    {
        if (isRealTimeProcessingEnabled())
        {
            return false;
        }
        ProcessingPipelineDefinition& definition = m_managers->imageProcessingManager->definition();
        std::unique_ptr<ProcessingModule> module =
            m_managers->processingModuleRegistry->create(moduleId);
        if (!module) return false;

        definition.addModule(std::move(module));
        m_managers->imageProcessingManager->clearRuntimePipelines();
        emit processingModulesChanged();
        return true;
    }

    // Remove a processing module and drop stale runtime clones
    bool ScopeOneCore::removeProcessingModule(int index)
    {
        if (isRealTimeProcessingEnabled())
        {
            return false;
        }
        ProcessingPipelineDefinition& definition = m_managers->imageProcessingManager->definition();
        if (!definition.removeModule(index))
        {
            return false;
        }
        m_managers->imageProcessingManager->clearRuntimePipelines();
        if (definition.moduleCount() == 0)
        {
            setRealTimeProcessingEnabled(false);
        }
        emit processingModulesChanged();
        return true;
    }

    // Update module parameters and rebuild per camera runtime modules
    bool ScopeOneCore::setProcessingModuleParameters(int index, const QVariantMap& parameters)
    {
        if (isRealTimeProcessingEnabled())
        {
            return false;
        }
        ProcessingPipelineDefinition& definition = m_managers->imageProcessingManager->definition();
        const bool updated = definition.withModule(index, [&parameters](ProcessingModule* module)
        {
            module->setParameters(parameters);
        });
        if (!updated)
        {
            return false;
        }
        m_managers->imageProcessingManager->clearRuntimePipelines();
        emit processingModuleParametersChanged(index);
        return true;
    }

    // Reset module state when the selected module owns runtime buffers
    bool ScopeOneCore::resetProcessingModuleState(int index)
    {
        if (isRealTimeProcessingEnabled())
        {
            return false;
        }
        ProcessingPipelineDefinition& definition = m_managers->imageProcessingManager->definition();
        bool resetRuntimeState = false;
        const bool found = definition.withModule(index, [&resetRuntimeState](ProcessingModule* module)
        {
            resetRuntimeState = module->resetState();
        });
        if (!found)
        {
            return false;
        }
        if (resetRuntimeState)
        {
            m_managers->imageProcessingManager->clearRuntimePipelines();
            emit processingModuleParametersChanged(index);
        }
        return true;
    }

    // Process one image without coupling the task to a UI layer
    quint64 ScopeOneCore::requestImageProcessing(const ImageFrame& frame,
                                                 const QString& sourceId)
    {
        if (!frame.isValid() || processingModules().isEmpty())
        {
            return 0;
        }

        const quint64 requestId = ++m_nextProcessingRequestId;
        auto cancelToken = std::make_shared<std::atomic_bool>(false);
        m_processingRequestCancelTokens.insert(requestId, cancelToken);
        auto pipeline = m_managers->imageProcessingManager->definition().createRuntime();
        const int bitDepth = static_cast<int>(processingBitDepth());
        auto* watcher = new QFutureWatcher<ProcessingResult>(this);
        connect(watcher, &QFutureWatcher<ProcessingResult>::finished,
                this, [this, watcher, requestId, sourceId, cancelToken]()
                {
                    ProcessingResult result = watcher->result();
                    m_processingRequestCancelTokens.remove(requestId);
                    emit imageProcessingFinished(
                        requestId,
                        sourceId,
                        cancelToken->load() ? ImageFrame{} : result.frame,
                        cancelToken->load() ? QStringLiteral("Processing canceled") : result.error);
                    watcher->deleteLater();
                });
        watcher->setFuture(QtConcurrent::run(
            m_offlineProcessingThreadPool.get(),
            [pipeline = std::move(pipeline), frame, bitDepth, cancelToken]()
            {
                if (cancelToken->load())
                {
                    return ProcessingResult{{}, QStringLiteral("Processing canceled")};
                }
                return pipeline->process(frame, bitDepth);
            }));
        return requestId;
    }

    // Process a complete recorded stack with one isolated stateful runtime
    quint64 ScopeOneCore::requestRecordingSessionStackProcessing(const QString& sessionId,
                                                                 const QString& cameraId)
    {
        const auto sourceSession = recordingSession(sessionId);
        const QString sourceCameraId = cameraId.trimmed();
        const qint64 frameCount = sourceSession
                                      ? sourceSession->recordedFrameCount(sourceCameraId)
                                      : 0;
        if (!sourceSession
            || sourceCameraId.isEmpty()
            || frameCount <= 0
            || frameCount > (std::numeric_limits<int>::max)()
            || processingModules().isEmpty())
        {
            return 0;
        }

        const quint64 requestId = ++m_nextProcessingRequestId;
        auto cancelToken = std::make_shared<std::atomic_bool>(false);
        m_processingRequestCancelTokens.insert(requestId, cancelToken);
        auto pipeline = m_managers->imageProcessingManager->definition().createRuntime();
        const int bitDepth = static_cast<int>(processingBitDepth());
        const ProcessingRecipe recipe = processingRecipe();
        auto* watcher = new QFutureWatcher<OfflineProcessingResult>(this);
        connect(watcher, &QFutureWatcher<OfflineProcessingResult>::finished,
                this, [this, watcher, requestId, cancelToken]()
                {
                    OfflineProcessingResult result = watcher->result();
                    m_processingRequestCancelTokens.remove(requestId);
                    result.canceled = result.canceled || cancelToken->load();
                    std::shared_ptr<RecordingSessionData> outputSession;
                    if (!result.canceled && result.errorMessage.isEmpty())
                    {
                        result.plan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                        result.plan.streamToDisk = false;
                        result.plan.saveDir.clear();
                        result.plan.baseName += QStringLiteral("_processed");
                        outputSession = createFrameSession(result.frames, result.plan);
                        if (!outputSession)
                        {
                            result.errorMessage = QStringLiteral("Failed to create processed stack");
                        }
                        else
                        {
                            outputSession->setCapturePlan(result.plan);
                        }
                    }
                    if (result.canceled)
                    {
                        result.errorMessage = QStringLiteral("Processing canceled");
                    }
                    emit stackProcessingFinished(requestId, outputSession, result.errorMessage);
                    watcher->deleteLater();
                });
        watcher->setFuture(QtConcurrent::run(
            m_offlineProcessingThreadPool.get(),
            [this, requestId, sourceSession, sourceCameraId, frameCount, bitDepth, recipe,
             pipeline = std::move(pipeline), cancelToken]()
            {
                OfflineProcessingResult result;
                result.plan = sourceSession->capturePlan();
                result.plan.cameraIds = {sourceCameraId};
                result.plan.processing = recipe;
                result.frames.reserve(static_cast<qsizetype>(frameCount));
                QElapsedTimer progressTimer;
                progressTimer.start();
                for (int index = 0; index < static_cast<int>(frameCount); ++index)
                {
                    if (cancelToken->load())
                    {
                        result.canceled = true;
                        break;
                    }
                    const ImageFrame frame = sourceSession->imageFrameAt(sourceCameraId, index);
                    if (!frame.isValid())
                    {
                        result.errorMessage = QStringLiteral("Failed to read stack frame %1")
                                                  .arg(index + 1);
                        break;
                    }
                    ProcessingResult processed = pipeline->process(frame, bitDepth);
                    if (!processed.succeeded())
                    {
                        result.errorMessage = processed.error;
                        break;
                    }
                    result.frames.append(std::move(processed.frame));
                    if (progressTimer.elapsed() >= 100 || index + 1 == frameCount)
                    {
                        const qint64 completed = index + 1;
                        QMetaObject::invokeMethod(this, [this, requestId, completed, frameCount]()
                        {
                            emit stackProcessingProgress(requestId, completed, frameCount);
                        });
                        progressTimer.restart();
                    }
                }
                return result;
            }));
        return requestId;
    }

    bool ScopeOneCore::cancelProcessingRequest(quint64 requestId)
    {
        const auto token = m_processingRequestCancelTokens.value(requestId);
        if (!token)
        {
            return false;
        }
        token->store(true);
        return true;
    }

    void ScopeOneCore::setRecordingMaxPendingWriteBytes(qint64 bytes)
    {
        m_managers->recordingManager->setRecordedMaxBytes(bytes);
        m_managers->recordingWriterStatus.setMaxPendingWriteBytes(
            m_managers->recordingManager->recordedMaxBytes());
        emit recordingWriterStatusChanged(m_managers->recordingWriterStatus);
    }

    qint64 ScopeOneCore::recordingMaxPendingWriteBytes() const
    {
        return m_managers->recordingManager->recordedMaxBytes();
    }

    // Return the latest recording progress snapshot
    ScopeOneCore::RecordingProgress ScopeOneCore::recordingProgress() const
    {
        return m_managers->recordingProgress;
    }

    // Return the latest recording writer snapshot
    ScopeOneCore::RecordingWriterStatus ScopeOneCore::recordingWriterStatus() const
    {
        return m_managers->recordingWriterStatus;
    }

    // Start recording and suspend preview during MDA motion
    bool ScopeOneCore::startRecording(const ExperimentPlan& plan, const QStringList& activeCameraIds)
    {
        if (m_configurationOperationRunning
            || m_pendingStageCommands > 0
            || m_managers->recordingManager->isRecording()
            || !m_managers->activeExperimentId.isEmpty()
            || isStageMosaicRunning())
        {
            return false;
        }

        ExperimentPlan planSnapshot = plan;
        if (planSnapshot.experimentId.trimmed().isEmpty())
        {
            planSnapshot.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        else
        {
            planSnapshot.experimentId = planSnapshot.experimentId.trimmed();
        }
        if (m_managers->experiments.contains(planSnapshot.experimentId))
        {
            return false;
        }
        m_managers->recordingProgress = RecordingProgress{};
        m_managers->recordingWriterStatus.reset(recordingMaxPendingWriteBytes());
        emit recordingWriterStatusChanged(m_managers->recordingWriterStatus);
        for (const QString& cameraId : activeCameraIds)
        {
            const QString normalizedCameraId = cameraId.trimmed();
            if (!normalizedCameraId.isEmpty() && !planSnapshot.cameraIds.contains(normalizedCameraId))
            {
                planSnapshot.cameraIds.append(normalizedCameraId);
            }
        }
        planSnapshot.pixelSizeUm = planSnapshot.cameraIds.size() == 1
                                       ? cameraPixelSizeUm(planSnapshot.cameraIds.first())
                                       : 0.0;

        const bool useMda = !planSnapshot.positions.empty() || !planSnapshot.zPositions.empty();
        QStringList suspendedPreviewIds;
        if (useMda)
        {
            const QStringList runningPreviewIds = runningPreviewCameraIds();
            for (const QString& cameraId : activeCameraIds)
            {
                if (runningPreviewIds.contains(cameraId))
                {
                    suspendedPreviewIds.append(cameraId);
                    stopPreview(cameraId);
                }
            }
        }

        if (planSnapshot.metadataFileName.trimmed().isEmpty())
        {
            planSnapshot.metadataFileName = recordingMetadataFileName(planSnapshot.baseName);
        }
        planSnapshot.configPath = m_loadedConfigPath;
        planSnapshot.configSha256 = m_loadedConfigSha256;
        planSnapshot.processing = processingRecipe();
        const QJsonObject deviceProperties = buildDevicePropertyMetadata(*this);

        ExperimentDocument runningDocument;
        runningDocument.plan = planSnapshot;
        runningDocument.runState = ExperimentRunState::Running;
        runningDocument.startedTimestampNs =
            static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        const ExperimentDocument& currentPresentation = m_imageSceneModel->document();
        runningDocument.layers = currentPresentation.layers;
        runningDocument.markups = currentPresentation.markups;
        m_managers->activeExperimentId = planSnapshot.experimentId;
        m_managers->experimentCancelRequested = false;
        m_managers->experiments.insert(planSnapshot.experimentId, runningDocument);

        QMetaObject::Connection restorePreviewConnection;
        if (!suspendedPreviewIds.isEmpty())
        {
            restorePreviewConnection = connect(
                m_managers->recordingManager,
                &RecordingManager::recordingStopped,
                this,
                [this, suspendedPreviewIds](const std::shared_ptr<RecordingSessionData>&)
                {
                    for (const QString& cameraId : suspendedPreviewIds)
                    {
                        startPreview(cameraId);
                    }
                },
                Qt::SingleShotConnection);
        }

        QHash<QString, double> pixelSizesUm;
        for (const QString& cameraId : planSnapshot.cameraIds)
        {
            const double pixelSizeUm = cameraPixelSizeUm(cameraId);
            if (pixelSizeUm > 0.0)
            {
                pixelSizesUm.insert(cameraId, pixelSizeUm);
            }
        }
        const bool started = m_managers->recordingManager->start(planSnapshot,
                                                                  activeCameraIds,
                                                                  deviceProperties,
                                                                  pixelSizesUm);
        if (!started)
        {
            if (restorePreviewConnection)
            {
                disconnect(restorePreviewConnection);
            }
            for (const QString& cameraId : suspendedPreviewIds)
            {
                startPreview(cameraId);
            }
            if (m_managers->activeExperimentId == planSnapshot.experimentId)
            {
                m_managers->experiments.remove(planSnapshot.experimentId);
                m_managers->activeExperimentId.clear();
                m_managers->experimentCancelRequested = false;
            }
            return false;
        }
        return true;
    }

    void ScopeOneCore::stopRecording()
    {
        m_managers->recordingManager->stop();
    }

    bool ScopeOneCore::isRecording() const
    {
        return m_managers->recordingManager->isRecording();
    }

    // Start one validated document through the shared recording lifecycle
    bool ScopeOneCore::startExperiment(const ExperimentDocument& document,
                                       QString* errorMessage)
    {
        if (m_configurationOperationRunning || m_pendingStageCommands > 0)
        {
            if (errorMessage)
            {
                *errorMessage = m_configurationOperationRunning
                                    ? QStringLiteral("A configuration operation is running")
                                    : QStringLiteral("A stage command is running");
            }
            return false;
        }
        if (!validateExperimentDocument(document, errorMessage))
        {
            return false;
        }
        if (document.runState != ExperimentRunState::Draft)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Only a Draft experiment can be started");
            return false;
        }

        const QString experimentId = document.plan.experimentId.trimmed();
        if (isRecording()
            || !m_managers->activeExperimentId.isEmpty()
            || isStageMosaicRunning())
        {
            if (errorMessage) *errorMessage = QStringLiteral("Another acquisition is already running");
            return false;
        }
        if (m_managers->experiments.contains(experimentId))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Experiment ID already exists: %1").arg(experimentId);
            }
            return false;
        }

        for (const QString& cameraId : document.plan.cameraIds)
        {
            if (!m_cameraIds.contains(cameraId))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Experiment camera is not available: %1").arg(cameraId);
                }
                return false;
            }
        }
        if (!document.plan.configSha256.isEmpty()
            && document.plan.configSha256 != m_loadedConfigSha256)
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral(
                    "Experiment configuration hash does not match the loaded configuration");
            }
            return false;
        }
        if (!processingRecipesEqual(processingRecipe(), document.plan.processing)
            && !applyProcessingRecipe(document.plan.processing, errorMessage))
        {
            return false;
        }

        QStringList startedPreviewCameraIds;
        const bool useMda = !document.plan.positions.empty() || !document.plan.zPositions.empty();
        if (!useMda)
        {
            const QStringList runningCameraIds = runningPreviewCameraIds();
            for (const QString& cameraId : document.plan.cameraIds)
            {
                if (runningCameraIds.contains(cameraId))
                {
                    continue;
                }
                if (!startPreview(cameraId))
                {
                    for (const QString& startedCameraId : startedPreviewCameraIds)
                    {
                        stopPreview(startedCameraId);
                    }
                    if (errorMessage)
                    {
                        *errorMessage = QStringLiteral("Failed to start preview for %1").arg(cameraId);
                    }
                    return false;
                }
                startedPreviewCameraIds.append(cameraId);
            }
        }

        m_managers->experimentStartedPreviewCameraIds = startedPreviewCameraIds;
        if (!startRecording(document.plan, document.plan.cameraIds))
        {
            for (const QString& cameraId : startedPreviewCameraIds)
            {
                stopPreview(cameraId);
            }
            m_managers->experimentStartedPreviewCameraIds.clear();
            if (errorMessage && errorMessage->isEmpty())
            {
                *errorMessage = useMda
                                    ? QStringLiteral("Failed to start MDA experiment")
                                    : QStringLiteral("Failed to start preview-frame experiment");
            }
            return false;
        }

        ExperimentDocument runningDocument = document;
        const auto registered = m_managers->experiments.constFind(experimentId);
        if (registered != m_managers->experiments.constEnd())
        {
            runningDocument.plan = registered->plan;
            runningDocument.startedTimestampNs = registered->startedTimestampNs;
        }
        runningDocument.runState = ExperimentRunState::Running;
        runningDocument.completedTimestampNs = 0;
        runningDocument.errorMessage.clear();
        runningDocument.events.clear();
        runningDocument.output = RecordingOutputManifest{};
        runningDocument.software = SoftwareSnapshot{};
        runningDocument.deviceProperties = QJsonObject{};
        m_managers->experiments.insert(experimentId, runningDocument);
        m_imageSceneModel->setDocument(runningDocument);
        return true;
    }

    bool ScopeOneCore::cancelExperiment(const QString& experimentId,
                                        QString* errorMessage)
    {
        const QString id = experimentId.trimmed();
        const auto experiment = m_managers->experiments.constFind(id);
        if (id.isEmpty() || experiment == m_managers->experiments.constEnd())
        {
            if (errorMessage) *errorMessage = id.isEmpty()
                                                  ? QStringLiteral("Missing experimentId")
                                                  : QStringLiteral("Unknown experiment");
            return false;
        }
        if (m_managers->activeExperimentId != id
            || experiment->runState != ExperimentRunState::Running)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Experiment is not running");
            return false;
        }
        m_managers->experimentCancelRequested = true;
        stopRecording();
        return true;
    }

    QString ScopeOneCore::activeExperimentId() const
    {
        return m_managers->activeExperimentId;
    }

    bool ScopeOneCore::experimentCancelRequested() const
    {
        return m_managers->experimentCancelRequested;
    }

    QStringList ScopeOneCore::experimentIds() const
    {
        return m_managers->experiments.keys();
    }

    bool ScopeOneCore::experimentDocument(const QString& experimentId,
                                          ExperimentDocument& document) const
    {
        const auto experiment = m_managers->experiments.constFind(experimentId.trimmed());
        if (experiment == m_managers->experiments.constEnd())
        {
            return false;
        }
        document = experiment.value();
        return true;
    }

    QStringList ScopeOneCore::recordingSessionIds() const
    {
        return m_managers->sessions.keys();
    }

    std::shared_ptr<ScopeOneCore::RecordingSessionData> ScopeOneCore::recordingSession(
        const QString& sessionId) const
    {
        return m_managers->sessions.value(sessionId.trimmed());
    }

    bool ScopeOneCore::closeRecordingSession(const QString& sessionId)
    {
        const QString id = sessionId.trimmed();
        const auto session = m_managers->sessions.value(id);
        if (!session)
        {
            return false;
        }
        m_managers->sessions.remove(id);
        emit recordingSessionClosed(id);
        emit recordingSessionsChanged();
        return true;
    }

    void ScopeOneCore::registerRecordingSession(
        const std::shared_ptr<RecordingSessionData>& session)
    {
        if (!session)
        {
            return;
        }
        const QString experimentId = session->capturePlan().experimentId.trimmed();
        if (experimentId.isEmpty())
        {
            return;
        }
        m_managers->sessions.insert(experimentId, session);
        m_managers->experiments.insert(experimentId, session->experimentDocument());
        emit recordingSessionsChanged();
    }

    // Finalize the shared experiment state before notifying API and UI clients
    void ScopeOneCore::finalizeActiveExperiment(
        const std::shared_ptr<RecordingSessionData>& session)
    {
        const QString activeExperimentId = m_managers->activeExperimentId;
        if (activeExperimentId.isEmpty())
        {
            registerRecordingSession(session);
            return;
        }

        const ExperimentDocument currentPresentation = m_imageSceneModel->document();
        ExperimentDocument completedDocument;
        if (session)
        {
            registerRecordingSession(session);
            completedDocument = session->experimentDocument();
        }
        else
        {
            completedDocument = m_managers->experiments.value(activeExperimentId);
            completedDocument.runState = ExperimentRunState::Failed;
            completedDocument.completedTimestampNs =
                static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
            completedDocument.errorMessage = QStringLiteral("Experiment stopped without session data");
            m_managers->experiments.insert(activeExperimentId, completedDocument);
        }

        if (currentPresentation.plan.experimentId == activeExperimentId)
        {
            if (session && session->isSaved())
            {
                m_imageSceneModel->applyValidatedDocument(completedDocument);
            }
            else
            {
                QString documentError;
                if (!m_imageSceneModel->setDocument(completedDocument, &documentError))
                {
                    qWarning().noquote()
                        << QStringLiteral("Failed to publish completed experiment: %1").arg(documentError);
                }
            }
        }

        for (const QString& cameraId : m_managers->experimentStartedPreviewCameraIds)
        {
            stopPreview(cameraId);
        }
        m_managers->experimentStartedPreviewCameraIds.clear();
        m_managers->activeExperimentId.clear();
        m_managers->experimentCancelRequested = false;
        emit stagePositionChanged();
    }

    // Attaches shared layer and markup state to one completed recording
    bool ScopeOneCore::setRecordingSessionPresentation(
        const std::shared_ptr<RecordingSessionData>& session,
        const ExperimentDocument& presentation,
        QString* errorMessage)
    {
        if (!session)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Missing recording session");
            return false;
        }

        ExperimentDocument candidate = session->experimentDocument();
        filterRecordingPresentation(presentation, candidate.layers, candidate.markups);
        if (!validateExperimentDocument(candidate, errorMessage))
        {
            return false;
        }

        session->setPresentationState(candidate.layers, candidate.markups);
        registerRecordingSession(session);

        if (session->streamedToDisk())
        {
            const ExperimentPlan& plan = candidate.plan;
            QString metadataName = plan.metadataFileName.trimmed();
            if (metadataName.isEmpty())
            {
                metadataName = recordingMetadataFileName(plan.baseName);
            }
            else if (!metadataName.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
            {
                metadataName += QStringLiteral(".json");
            }
            const QString outputDir = QDir(plan.saveDir).filePath(plan.baseName.trimmed());
            if (!saveExperimentDocument(QDir(outputDir).filePath(metadataName), candidate, errorMessage))
            {
                return false;
            }
        }
        return true;
    }

    // Queue one session copy for background writing
    bool ScopeOneCore::queueRecordingSessionSave(
        const std::shared_ptr<RecordingSessionData>& sourceSession,
        const std::shared_ptr<RecordingSessionData>& saveSession,
        const QString& cameraId)
    {
        if (!sourceSession || !saveSession || m_sessionsSaving.contains(sourceSession.get()))
        {
            return false;
        }
        saveSession->m_frames.clear();
        saveSession->clearOutputFiles();
        saveSession->m_manifest.output.streamedToDisk = false;
        m_sessionsSaving.insert(sourceSession.get());
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished,
                this, [this, watcher, sourceSession, saveSession, cameraId]()
                {
                    m_sessionsSaving.remove(sourceSession.get());
                    if (cameraId.isEmpty())
                    {
                        sourceSession->applySaveStateFrom(*saveSession);
                        registerRecordingSession(sourceSession);
                        emit recordingSessionSaveFinished(sourceSession);
                    }
                    else
                    {
                        emit recordingSessionCameraSaveFinished(
                            sourceSession,
                            cameraId,
                            saveSession->isSaved(),
                            saveSession->saveMessage());
                    }
                    watcher->deleteLater();
                });
        watcher->setFuture(QtConcurrent::run([saveSession, sourceSession]()
        {
            return RecordingManager::saveSessionToDisk(saveSession, sourceSession);
        }));
        return true;
    }

    // Save a completed session on a worker thread
    bool ScopeOneCore::saveRecordingSession(const std::shared_ptr<RecordingSessionData>& session)
    {
        if (!session)
        {
            return false;
        }
        auto saveSession = session->cloneForSave();
        ExperimentPlan plan = saveSession->capturePlan();
        if (plan.metadataFileName.trimmed().isEmpty())
        {
            plan.metadataFileName = recordingMetadataFileName(plan.baseName);
            saveSession->setCapturePlan(plan);
        }
        return queueRecordingSessionSave(session, saveSession);
    }

    // Apply output options to a detached session copy before writing
    bool ScopeOneCore::saveRecordingSession(
        const std::shared_ptr<RecordingSessionData>& session,
        const RecordingSaveOptions& saveOptions)
    {
        if (!session)
        {
            return false;
        }
        auto saveSession = session->cloneForSave();
        ExperimentPlan plan = saveSession->capturePlan();
        plan.format = saveOptions.format;
        plan.enableCompression = saveOptions.enableCompression;
        plan.compressionLevel = saveOptions.compressionLevel;
        if (!saveOptions.saveDir.trimmed().isEmpty())
        {
            plan.saveDir = saveOptions.saveDir;
        }
        if (!saveOptions.baseName.trimmed().isEmpty())
        {
            plan.baseName = saveOptions.baseName;
        }
        plan.metadataFileName = recordingMetadataFileName(plan.baseName);
        saveSession->setCapturePlan(plan);
        return queueRecordingSessionSave(session, saveSession);
    }

    // Save only the camera stack represented by one image document
    bool ScopeOneCore::saveRecordingSessionCamera(
        const std::shared_ptr<RecordingSessionData>& session,
        const QString& cameraId,
        const RecordingSaveOptions& saveOptions,
        const ExperimentDocument* presentation)
    {
        const QString sourceCameraId = cameraId.trimmed();
        if (!session || sourceCameraId.isEmpty()
            || session->recordedFrameCount(sourceCameraId) <= 0)
        {
            return false;
        }
        auto saveSession = session->cloneForSave();
        ExperimentPlan plan = saveSession->capturePlan();
        plan.cameraIds = {sourceCameraId};
        plan.format = saveOptions.format;
        plan.enableCompression = saveOptions.enableCompression;
        plan.compressionLevel = saveOptions.compressionLevel;
        if (!saveOptions.saveDir.trimmed().isEmpty())
        {
            plan.saveDir = saveOptions.saveDir;
        }
        if (!saveOptions.baseName.trimmed().isEmpty())
        {
            plan.baseName = saveOptions.baseName;
        }
        plan.metadataFileName = recordingMetadataFileName(plan.baseName);
        saveSession->setCapturePlan(plan);
        QList<AcquisitionEventRecord> cameraEvents;
        for (const AcquisitionEventRecord& record : saveSession->m_manifest.events)
        {
            const auto frame = record.frames.constFind(sourceCameraId);
            if (frame == record.frames.constEnd())
            {
                continue;
            }
            AcquisitionEventRecord cameraRecord = record;
            cameraRecord.event.cameraIds = {sourceCameraId};
            cameraRecord.frames = {{sourceCameraId, frame.value()}};
            cameraEvents.append(std::move(cameraRecord));
        }
        saveSession->m_manifest.events = std::move(cameraEvents);
        if (presentation)
        {
            filterRecordingPresentation(
                *presentation, saveSession->m_manifest.layers, saveSession->m_manifest.markups);
        }
        else
        {
            saveSession->m_manifest.layers.clear();
            saveSession->m_manifest.markups.clear();
        }
        return queueRecordingSessionSave(session, saveSession, sourceCameraId);
    }

    // Reads one recording frame on the serialized session IO worker
    quint64 ScopeOneCore::requestRecordingSessionFrame(
        const std::shared_ptr<RecordingSessionData>& session,
        const QString& cameraId,
        int index)
    {
        const QString camera = cameraId.trimmed();
        if (!session || camera.isEmpty() || index < 0)
        {
            return 0;
        }

        const quint64 requestId = ++m_nextSessionFrameRequestId;
        auto* watcher = new QFutureWatcher<ImageFrame>(this);
        connect(watcher, &QFutureWatcher<ImageFrame>::finished,
                this, [this, watcher, requestId, session, camera, index]()
        {
            emit recordingSessionFrameReady(
                requestId, session, camera, index, watcher->result());
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run(
            m_sessionFrameThreadPool.get(),
            [session, camera, index]()
            {
                return session->imageFrameAt(camera, index);
            }));
        return requestId;
    }
} // namespace scopeone::core
