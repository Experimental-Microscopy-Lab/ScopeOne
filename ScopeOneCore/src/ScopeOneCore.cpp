#include "scopeone/ScopeOneCore.h"

#include "internal/ImageProcessingFramework.h"
#include "internal/MMCoreManager.h"
#include "internal/MultiProcessCameraManager.h"
#include "internal/RecordingManager.h"
#include "MMCore.h"
#include <QDateTime>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QStringList>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <tiffio.h>

namespace
{
    // Histogram bins are fixed to keep UI cost stable
    constexpr int kHistogramBinCount = 256;
    // Auto stretch ignores a small tail on each side
    constexpr double kHistogramAutoStretchIgnoredQuantile = 0.001;
    // Histogram refresh is throttled to keep preview responsive
    constexpr qint64 kHistogramRefreshIntervalMs = 250;

    // Map a sample value into the shared histogram bin space
    int histogramBinForValue(int value, int maxValue)
    {
        if (maxValue <= 0)
        {
            return 0;
        }
        const qint64 scaled = static_cast<qint64>(value) * (kHistogramBinCount - 1);
        return qBound(0, static_cast<int>(scaled / maxValue), kHistogramBinCount - 1);
    }

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

    // Convert saved frame info metadata into an image pixel format
    scopeone::core::ImagePixelFormat pixelFormatFromFrameInfo(const QByteArray& name, int id)
    {
        if (id == 1 || name == "Mono16")
        {
            return scopeone::core::ImagePixelFormat::Mono16;
        }
        if (id == 0 || name == "Mono8")
        {
            return scopeone::core::ImagePixelFormat::Mono8;
        }
        return scopeone::core::ImagePixelFormat::Invalid;
    }

    // Opens a TIFF stack for frame readback
    void* openTiffForRead(const QString& path)
    {
        const char* mode = "r";
#if defined(_WIN32)
        std::wstring w = path.toStdWString();
#if defined(TIFFOpenW)
        return TIFFOpenW(reinterpret_cast<const wchar_t*>(w.c_str()), mode);
#else
        return TIFFOpen(path.toLocal8Bit().constData(), mode);
#endif
#else
        return TIFFOpen(path.toLocal8Bit().constData(), mode);
#endif
    }

    // Closes a TIFF handle after frame readback
    struct TiffReadCloser
    {
        void operator()(TIFF* tiff) const
        {
            if (tiff)
            {
                TIFFClose(tiff);
            }
        }
    };

    // Parses one CSV row from a binary frame info sidecar
    QList<QByteArray> parseFrameInfoCsvLine(const QByteArray& line)
    {
        QList<QByteArray> fields;
        QByteArray field;
        bool inQuotes = false;
        for (qsizetype i = 0; i < line.size(); ++i)
        {
            const char ch = line.at(i);
            if (inQuotes)
            {
                if (ch == '"')
                {
                    if (i + 1 < line.size() && line.at(i + 1) == '"')
                    {
                        field.append('"');
                        ++i;
                    }
                    else
                    {
                        inQuotes = false;
                    }
                }
                else
                {
                    field.append(ch);
                }
                continue;
            }

            if (ch == ',')
            {
                fields.append(field);
                field.clear();
            }
            else if (ch == '"' && field.isEmpty())
            {
                inQuotes = true;
            }
            else
            {
                field.append(ch);
            }
        }
        fields.append(field);
        return fields;
    }

    // Read one signed integer field from a frame info row
    bool readIntField(const QList<QByteArray>& fields, int index, int& value)
    {
        if (index < 0 || index >= fields.size())
        {
            return false;
        }
        bool ok = false;
        value = fields.at(index).toInt(&ok);
        return ok;
    }

    // Read one signed long integer field from a frame info row
    bool readInt64Field(const QList<QByteArray>& fields, int index, qint64& value)
    {
        if (index < 0 || index >= fields.size())
        {
            return false;
        }
        bool ok = false;
        value = fields.at(index).toLongLong(&ok);
        return ok;
    }

    // Read one unsigned long integer field from a frame info row
    bool readUInt64Field(const QList<QByteArray>& fields, int index, quint64& value)
    {
        if (index < 0 || index >= fields.size())
        {
            return false;
        }
        bool ok = false;
        value = fields.at(index).toULongLong(&ok);
        return ok;
    }

    // Read one unsigned long integer from stored JSON metadata
    quint64 readJsonUInt64(const QJsonObject& object, const QString& key, quint64 currentValue)
    {
        const QJsonValue value = object.value(key);
        if (value.isString())
        {
            bool ok = false;
            const quint64 parsed = value.toString().toULongLong(&ok);
            return ok ? parsed : currentValue;
        }
        if (value.isDouble())
        {
            const double parsed = value.toDouble(-1.0);
            return parsed >= 0.0 ? static_cast<quint64>(parsed) : currentValue;
        }
        return currentValue;
    }

    // Read one integer from stored JSON metadata
    int readJsonInt(const QJsonObject& object, const QString& key, int currentValue)
    {
        const QJsonValue value = object.value(key);
        if (value.isString())
        {
            bool ok = false;
            const int parsed = value.toString().toInt(&ok);
            return ok ? parsed : currentValue;
        }
        if (value.isDouble())
        {
            return value.toInt(currentValue);
        }
        return currentValue;
    }

    // Apply TIFF page metadata to a recorded image frame
    void applyTiffImageDescriptionMetadata(const QByteArray& imageDescription,
                                           scopeone::core::ImageFrame& frame)
    {
        if (imageDescription.isEmpty())
        {
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(imageDescription, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
        {
            return;
        }

        const QJsonObject object = document.object();
        const QString storedCameraId = object.value(QStringLiteral("camera_id")).toString().trimmed();
        if (!storedCameraId.isEmpty())
        {
            frame.cameraId = storedCameraId;
        }
        frame.frameIndex = readJsonUInt64(object, QStringLiteral("frame_index"), frame.frameIndex);
        frame.timestampNs = readJsonUInt64(object, QStringLiteral("timestamp_ns"), frame.timestampNs);
        const int storedBitsPerSample = readJsonInt(object, QStringLiteral("bits_per_sample"), frame.bitsPerSample);
        frame.bitsPerSample = scopeone::core::ImageFrame::normalizedBitsPerSample(frame.pixelFormat,
                                                                                  storedBitsPerSample);
        frame.sourceRoiX = readJsonInt(object, QStringLiteral("source_roi_x"), frame.sourceRoiX);
        frame.sourceRoiY = readJsonInt(object, QStringLiteral("source_roi_y"), frame.sourceRoiY);
        frame.sourceRoiWidth = readJsonInt(object, QStringLiteral("source_roi_width"), frame.sourceRoiWidth);
        frame.sourceRoiHeight = readJsonInt(object, QStringLiteral("source_roi_height"), frame.sourceRoiHeight);
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

    // Convert MMCore string vectors into Qt string lists
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

    // Build the cache key for raw and processed histogram layers
    QString histogramLayerKey(const QString& cameraId, bool processed)
    {
        return QStringLiteral("%1:%2")
            .arg(processed ? QStringLiteral("proc") : QStringLiteral("raw"),
                 cameraId.trimmed());
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

    template <typename Operation>
    // Run an MMCore operation only after validating the device label
    bool runWithTrimmedLabel(const QString& rawLabel, Operation&& operation)
    {
        const QString label = rawLabel.trimmed();
        if (label.isEmpty())
        {
            return false;
        }
        try
        {
            operation(label.toStdString());
            return true;
        }
        catch (const CMMError&)
        {
            return false;
        }
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

        stats.minVal = static_cast<double>(stats.maxValue);
        stats.maxVal = 0.0;

        const uchar* bytes = reinterpret_cast<const uchar*>(frame.bytes.constData());
        double sum = 0.0;
        double sumSq = 0.0;

        if (mono16)
        {
            for (int y = 0; y < frame.height; ++y)
            {
                const quint16* row = reinterpret_cast<const quint16*>(bytes + static_cast<qint64>(y) * frame.stride);
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
                const uchar* row = bytes + static_cast<qint64>(y) * frame.stride;
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
        facade.successCount = result.successCount;
        facade.failCount = result.failCount;
        facade.skippedCameraCount = result.skippedCameraCount;
        facade.foundCamera = result.foundCamera;
        return facade;
    }

    // Translate public recording settings into manager settings
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

    // Identify the public module kind from a concrete module instance
    scopeone::core::ScopeOneCore::ProcessingModuleKind processingModuleKind(
        const scopeone::core::internal::ProcessingModule* module)
    {
        if (qobject_cast<const scopeone::core::internal::FFTModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::FFT;
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
        if (qobject_cast<const scopeone::core::internal::DifferentialRollingModule*>(module))
        {
            return scopeone::core::ScopeOneCore::ProcessingModuleKind::DifferentialRolling;
        }
        return scopeone::core::ScopeOneCore::ProcessingModuleKind::Unknown;
    }

    // Capture current device properties for recording metadata
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
    using scopeone::core::internal::ImageProcessingManager;
    using scopeone::core::internal::MMCoreManager;
    using scopeone::core::internal::MultiProcessCameraManager;
    using scopeone::core::internal::ProcessingModule;
    using scopeone::core::internal::ProcessingPipeline;
    using scopeone::core::internal::RecordingManager;
    using scopeone::core::internal::DifferentialRollingModule;
    using scopeone::core::internal::SpatiotemporalBinningModule;

    // Return a stored frame from memory or saved disk output
    ImageFrame ScopeOneCore::RecordingSessionData::imageFrameAt(const QString& cameraId, int index) const
    {
        const QString trimmedCameraId = cameraId.trimmed();
        const ImageFrame* frame = frameAt(trimmedCameraId, index);
        return frame ? *frame : outputImageFrameAt(trimmedCameraId, index);
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

        if (m_manifest.plan.format == RecordingFormat::Tiff)
        {
            std::unique_ptr<TIFF, TiffReadCloser> tiff(
                reinterpret_cast<TIFF*>(openTiffForRead(fileManifest.rawPath)));
            if (!tiff)
            {
                return {};
            }

            if (!TIFFSetDirectory(tiff.get(), static_cast<tdir_t>(index)))
            {
                return {};
            }

            uint32_t width = 0;
            uint32_t height = 0;
            uint16_t bitsPerSample = 0;
            uint16_t samplesPerPixel = 1;
            uint16_t planarConfig = PLANARCONFIG_CONTIG;
            char* imageDescription = nullptr;
            if (!TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &width)
                || !TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &height)
                || !TIFFGetField(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bitsPerSample))
            {
                return {};
            }
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);
            TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planarConfig);
            QByteArray imageDescriptionBytes;
            if (TIFFGetField(tiff.get(), TIFFTAG_IMAGEDESCRIPTION, &imageDescription) && imageDescription)
            {
                imageDescriptionBytes = QByteArray(imageDescription);
            }
            if (width == 0 || height == 0 || samplesPerPixel != 1 || planarConfig != PLANARCONFIG_CONTIG)
            {
                return {};
            }

            scopeone::core::ImagePixelFormat pixelFormat = scopeone::core::ImagePixelFormat::Invalid;
            if (bitsPerSample <= 8)
            {
                pixelFormat = scopeone::core::ImagePixelFormat::Mono8;
                bitsPerSample = 8;
            }
            else if (bitsPerSample <= 16)
            {
                pixelFormat = scopeone::core::ImagePixelFormat::Mono16;
                bitsPerSample = 16;
            }
            else
            {
                return {};
            }

            const int bytesPerPixel = pixelFormat == scopeone::core::ImagePixelFormat::Mono16 ? 2 : 1;
            const qint64 stride = static_cast<qint64>(width) * bytesPerPixel;
            const qint64 payloadBytes = stride * static_cast<qint64>(height);
            if (width > static_cast<uint32_t>((std::numeric_limits<int>::max)())
                || height > static_cast<uint32_t>((std::numeric_limits<int>::max)())
                || stride > (std::numeric_limits<int>::max)()
                || payloadBytes <= 0
                || payloadBytes > (std::numeric_limits<qsizetype>::max)())
            {
                return {};
            }

            QByteArray bytes;
            bytes.resize(static_cast<qsizetype>(payloadBytes));
            for (uint32_t y = 0; y < height; ++y)
            {
                char* row = bytes.data() + static_cast<qint64>(y) * stride;
                if (TIFFReadScanline(tiff.get(), row, y, 0) < 0)
                {
                    return {};
                }
            }

            ImageFrame frame;
            frame.cameraId = cameraId;
            frame.width = static_cast<int>(width);
            frame.height = static_cast<int>(height);
            frame.stride = static_cast<int>(stride);
            frame.pixelFormat = pixelFormat;
            frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(frame.pixelFormat, bitsPerSample);
            frame.frameIndex = static_cast<quint64>(index + 1);
            frame.sourceRoiX = 0;
            frame.sourceRoiY = 0;
            frame.sourceRoiWidth = frame.width;
            frame.sourceRoiHeight = frame.height;
            applyTiffImageDescriptionMetadata(imageDescriptionBytes, frame);
            frame.bytes = std::move(bytes);
            return frame.isValid() ? frame : ImageFrame{};
        }

        if (m_manifest.plan.format != RecordingFormat::Binary || fileManifest.frameInfoPath.isEmpty())
        {
            return {};
        }

        QFile frameInfoFile(fileManifest.frameInfoPath);
        QFile rawFile(fileManifest.rawPath);
        if (!frameInfoFile.open(QIODevice::ReadOnly | QIODevice::Text)
            || !rawFile.open(QIODevice::ReadOnly))
        {
            return {};
        }

        (void)frameInfoFile.readLine();
        int currentIndex = 0;
        qint64 rawOffset = 0;
        while (!frameInfoFile.atEnd())
        {
            const QByteArray line = frameInfoFile.readLine().trimmed();
            if (line.isEmpty())
            {
                continue;
            }
            const QList<QByteArray> fields = parseFrameInfoCsvLine(line);
            if (fields.size() < 10)
            {
                return {};
            }

            qint64 payloadBytes = 0;
            if (!readInt64Field(fields, 9, payloadBytes) || payloadBytes <= 0)
            {
                return {};
            }

            if (currentIndex == index)
            {
                if (payloadBytes > (std::numeric_limits<qsizetype>::max)()
                    || !rawFile.seek(rawOffset))
                {
                    return {};
                }

                QByteArray bytes = rawFile.read(payloadBytes);
                if (static_cast<qint64>(bytes.size()) != payloadBytes)
                {
                    return {};
                }

                ImageFrame frame;
                frame.cameraId = QString::fromUtf8(fields.at(0)).trimmed();
                if (frame.cameraId.isEmpty())
                {
                    frame.cameraId = cameraId;
                }
                if (!readUInt64Field(fields, 1, frame.frameIndex)
                    || !readUInt64Field(fields, 2, frame.timestampNs)
                    || !readIntField(fields, 3, frame.width)
                    || !readIntField(fields, 4, frame.height))
                {
                    return {};
                }

                int bitsPerSample = 0;
                int pixelFormatId = 0;
                if (!readIntField(fields, 5, bitsPerSample)
                    || !readIntField(fields, 6, frame.stride)
                    || !readIntField(fields, 8, pixelFormatId))
                {
                    return {};
                }
                frame.pixelFormat = pixelFormatFromFrameInfo(fields.at(7), pixelFormatId);
                frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(frame.pixelFormat, bitsPerSample);

                if (fields.size() >= 14)
                {
                    if (!readIntField(fields, 10, frame.sourceRoiX)
                        || !readIntField(fields, 11, frame.sourceRoiY)
                        || !readIntField(fields, 12, frame.sourceRoiWidth)
                        || !readIntField(fields, 13, frame.sourceRoiHeight))
                    {
                        return {};
                    }
                }
                else
                {
                    frame.sourceRoiX = 0;
                    frame.sourceRoiY = 0;
                    frame.sourceRoiWidth = frame.width;
                    frame.sourceRoiHeight = frame.height;
                }

                frame.bytes = std::move(bytes);
                return frame.isValid() ? frame : ImageFrame{};
            }

            if (payloadBytes > (std::numeric_limits<qint64>::max)() - rawOffset)
            {
                return {};
            }
            rawOffset += payloadBytes;
            ++currentIndex;
        }
        return {};
    }

    struct ScopeOneCore::Managers
    {
        MMCoreManager* mmcoreManager{nullptr};
        MultiProcessCameraManager* mpcm{nullptr};
        RecordingManager* recordingManager{nullptr};
        ImageProcessingManager* imageProcessingManager{nullptr};
    };

    // Return the compiled core version string
    QString ScopeOneCore::getVersion()
    {
        return QStringLiteral(SCOPEONE_CORE_VERSION_STRING);
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
        qRegisterMetaType<scopeone::core::ImageFrame>("scopeone::core::ImageFrame");
        m_managers->mmcoreManager = new MMCoreManager(this);
        m_managers->mpcm = new MultiProcessCameraManager(this);
        m_managers->recordingManager = new RecordingManager(this);
        m_managers->imageProcessingManager = new ImageProcessingManager(this);
        m_managers->recordingManager->setMultiProcessCameraManager(m_managers->mpcm);
        m_managers->recordingManager->setMMCore(m_managers->mmcoreManager->getCore());
        m_managers->recordingManager->setLatestFrameFetcher(
            [this](const QString& cameraId, ImageFrame& frame)
            {
                return getLatestRawFrame(cameraId, frame);
            });

        connect(m_managers->mpcm, &MultiProcessCameraManager::newRawFrameReady,
                this, &ScopeOneCore::handleIncomingRawFrame);
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
                    emit processedFrameReady(frame);
                    scheduleHistogramStats(frame.cameraId, true, frame);
                    updateLineProfile(frame.cameraId, true, frame);
                });
        connect(m_managers->imageProcessingManager, &ImageProcessingManager::processingError,
                this, &ScopeOneCore::processingError);
    }

    // Release loaded devices before the facade is destroyed
    ScopeOneCore::~ScopeOneCore()
    {
        unloadConfiguration();
    }

    // Expose the native MMCore handle for low level callers
    std::shared_ptr<CMMCore> ScopeOneCore::core() const
    {
        return m_managers->mmcoreManager->getCore();
    }

    // Check whether a device is owned by the agent camera path
    bool ScopeOneCore::isAgentCamera(const QString& deviceLabel) const
    {
        return m_cameraIds.contains(deviceLabel);
    }

    // Check whether a device is a native MMCore camera
    bool ScopeOneCore::isNativeCamera(const QString& deviceLabel) const
    {
        const QString device = deviceLabel.trimmed();
        if (device.isEmpty() || isAgentCamera(device))
        {
            return false;
        }

        auto handle = core();
        try
        {
            return handle->getDeviceType(device.toStdString().c_str()) == MM::CameraDevice;
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    // Collect camera ids that currently have active previews
    QStringList ScopeOneCore::runningPreviewCameraIds() const
    {
        QStringList running;
        for (const QString& cameraId : m_cameraIds)
        {
            if (m_managers->mpcm->isPreviewRunning(cameraId))
            {
                running.append(cameraId);
            }
        }
        return running;
    }

    // Load devices and agent cameras from a Micro Manager config
    bool ScopeOneCore::loadConfigurationInternal(const QString& configPath,
                                                 LoadConfigResult* result,
                                                 QString* errorMessage)
    {
        MMCoreManager::LoadConfigResult mmResult;
        if (!m_managers->mmcoreManager->loadConfigurationAndStartCameras(
            configPath, m_managers->mpcm, &mmResult, errorMessage))
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

    // Replace the active configuration with a new device setup
    bool ScopeOneCore::loadConfiguration(const QString& configPath,
                                         LoadConfigResult* result,
                                         QString* errorMessage)
    {
        if (configPath.trimmed().isEmpty())
        {
            return loadConfigurationInternal(configPath, result, errorMessage);
        }
        if (!loadedDevices().isEmpty())
        {
            unloadConfiguration();
        }
        return loadConfigurationInternal(configPath, result, errorMessage);
    }

    // Stop cameras and clear all cached runtime state
    void ScopeOneCore::unloadConfiguration()
    {
        m_managers->mpcm->stopPreview();
        m_managers->mpcm->stopAgents();
        auto handle = core();
        try
        {
            handle->unloadAllDevices();
        }
        catch (const CMMError&)
        {
        }
        m_cameraIds.clear();
        m_latestRawFrames.clear();
        m_latestProcessedFrames.clear();
        m_histogramJobStates.clear();
        m_latestHistogramStats.clear();
        clearLineProfile();
    }

    // Start preview for one camera or the full camera set
    void ScopeOneCore::startPreview(const QString& cameraIdOrAll)
    {
        // Route preview to one camera or all cameras
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

    // Stop preview for one camera or the full camera set
    void ScopeOneCore::stopPreview(const QString& cameraIdOrAll)
    {
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

    // Submit exposure changes through the active camera manager
    bool ScopeOneCore::setExposure(const QString& cameraIdOrAll, double exposureMs)
    {
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->setExposure(target, exposureMs);
    }

    // Apply an ROI rectangle to a camera
    bool ScopeOneCore::setROI(const QString& cameraId, int x, int y, int width, int height)
    {
        const QString target = cameraId.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->setROI(target, x, y, width, height);
    }

    bool ScopeOneCore::clearROI(const QString& cameraId)
    {
        const QString target = cameraId.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->clearROI(target);
    }

    // Read the active hardware ROI rectangle from a camera
    bool ScopeOneCore::getROI(const QString& cameraId, int& x, int& y, int& width, int& height)
    {
        const QString target = cameraId.trimmed();
        if (target.isEmpty())
        {
            return false;
        }
        return m_managers->mpcm->getROI(target, x, y, width, height);
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
        m_latestRawFrames.insert(cameraId, normalizedFrame);
        emit newRawFrameReady(normalizedFrame);
        scheduleHistogramStats(cameraId, false, normalizedFrame);
        updateLineProfile(cameraId, false, normalizedFrame);

        if (isRealTimeProcessingEnabled())
        {
            processFrameAsync(normalizedFrame);
        }
    }

    // Return the newest raw frame as an ImageFrame
    bool ScopeOneCore::getLatestRawFrame(const QString& cameraId, ImageFrame& frame) const
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty())
        {
            frame = ImageFrame{};
            return false;
        }

        const auto cachedFrame = m_latestRawFrames.constFind(trimmedCameraId);
        if (cachedFrame != m_latestRawFrames.constEnd() && cachedFrame.value().isValid())
        {
            frame = cachedFrame.value();
            return true;
        }

        return m_managers->mpcm->getLatestRaw(trimmedCameraId, frame);
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

        ImageFrame frame;
        if (!getLatestRawFrame(cameraId, frame))
        {
            return false;
        }
        return computeHistogramStats(frame, stats);
    }

    // Schedule throttled histogram work for a layer
    void ScopeOneCore::scheduleHistogramStats(const QString& cameraId,
                                              bool processed,
                                              const ImageFrame& frame)
    {
        // Throttle histogram work per layer
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty() || !frame.isValid())
        {
            return;
        }

        const QString cacheKey = histogramLayerKey(trimmedCameraId, processed);
        HistogramJobState& state = m_histogramJobStates[cacheKey];
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (state.inFlight)
        {
            state.queuedFrame = frame;
            return;
        }
        if (state.lastScheduledMs > 0 && (nowMs - state.lastScheduledMs) < kHistogramRefreshIntervalMs)
        {
            return;
        }

        state.inFlight = true;
        state.lastScheduledMs = nowMs;
        const quint64 sequence = ++m_nextHistogramSequence;
        state.activeSequence = sequence;

        auto* watcher = new QFutureWatcher<HistogramStats>(this);
        connect(watcher, &QFutureWatcher<HistogramStats>::finished, this,
                [this, watcher, trimmedCameraId, processed, cacheKey, sequence]()
                {
                    HistogramStats stats = watcher->result();
                    ImageFrame queuedFrame;

                    auto it = m_histogramJobStates.find(cacheKey);
                    if (it != m_histogramJobStates.end() && it->activeSequence == sequence)
                    {
                        it->inFlight = false;
                        if (stats.hasData())
                        {
                            m_latestHistogramStats.insert(cacheKey, stats);
                            emit imageHistogramReady(trimmedCameraId, processed, stats);
                        }
                        if (it->queuedFrame.isValid())
                        {
                            queuedFrame = it->queuedFrame;
                            it->queuedFrame = ImageFrame{};
                            it->lastScheduledMs = 0;
                        }
                    }
                    watcher->deleteLater();

                    if (queuedFrame.isValid())
                    {
                        scheduleHistogramStats(trimmedCameraId, processed, queuedFrame);
                    }
                });
        watcher->setFuture(QtConcurrent::run([frame]()
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
        try
        {
            return QString::fromStdString(handle->getFocusDevice());
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    // Read the current XY stage position from MMCore
    bool ScopeOneCore::readXYPosition(const QString& xyStageLabel, double& x, double& y) const
    {
        x = 0.0;
        y = 0.0;
        const QString label = xyStageLabel.trimmed();
        auto handle = core();
        if (label.isEmpty())
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

    // Read the current Z stage position from MMCore
    bool ScopeOneCore::readZPosition(const QString& zStageLabel, double& z) const
    {
        z = 0.0;
        const QString label = zStageLabel.trimmed();
        auto handle = core();
        if (label.isEmpty())
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

    // Move an XY stage by a relative offset and wait for completion
    bool ScopeOneCore::moveXYRelative(const QString& xyStageLabel, double dx, double dy)
    {
        auto handle = core();
        return runWithTrimmedLabel(xyStageLabel, [&](const std::string& label)
        {
            handle->setRelativeXYPosition(label.c_str(), dx, dy);
            handle->waitForDevice(label.c_str());
        });
    }

    // Move a Z stage by a relative offset and wait for completion
    bool ScopeOneCore::moveZRelative(const QString& zStageLabel, double dz)
    {
        auto handle = core();
        return runWithTrimmedLabel(zStageLabel, [&](const std::string& label)
        {
            handle->setRelativePosition(label.c_str(), dz);
            handle->waitForDevice(label.c_str());
        });
    }

    // Move an XY stage to an absolute position and wait for completion
    bool ScopeOneCore::moveXYTo(const QString& xyStageLabel, double x, double y)
    {
        auto handle = core();
        return runWithTrimmedLabel(xyStageLabel, [&](const std::string& label)
        {
            handle->setXYPosition(label.c_str(), x, y);
            handle->waitForDevice(label.c_str());
        });
    }

    // Move a Z stage to an absolute position and wait for completion
    bool ScopeOneCore::moveZTo(const QString& zStageLabel, double z)
    {
        auto handle = core();
        return runWithTrimmedLabel(zStageLabel, [&](const std::string& label)
        {
            handle->setPosition(label.c_str(), z);
            handle->waitForDevice(label.c_str());
        });
    }

    // List available Micro Manager configuration groups
    QStringList ScopeOneCore::availableConfigGroups() const
    {
        auto handle = core();
        try
        {
            const auto groups = handle->getAvailableConfigGroups();
            QStringList result;
            for (const auto& g : groups)
            {
                result.append(QString::fromStdString(g));
            }
            return result;
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    // List presets in a Micro Manager configuration group
    QStringList ScopeOneCore::availableConfigs(const QString& configGroup) const
    {
        auto handle = core();
        if (configGroup.isEmpty())
        {
            return {};
        }
        try
        {
            const auto configs = handle->getAvailableConfigs(configGroup.toStdString().c_str());
            QStringList result;
            for (const auto& c : configs)
            {
                result.append(QString::fromStdString(c));
            }
            return result;
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    // Read the current preset for a configuration group
    QString ScopeOneCore::currentConfig(const QString& groupName) const
    {
        auto handle = core();
        if (groupName.isEmpty())
        {
            return {};
        }
        try
        {
            return QString::fromStdString(handle->getCurrentConfig(groupName.toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    // Apply a configuration preset while camera previews are paused
    bool ScopeOneCore::setConfig(const QString& groupName, const QString& configName)
    {
        auto handle = core();
        if (groupName.isEmpty() || configName.isEmpty())
        {
            return false;
        }
        const QStringList runningPreviewIds = runningPreviewCameraIds();
        return withSuspendedPreviews(this, runningPreviewIds, [&]()
        {
            try
            {
                handle->setConfig(groupName.toStdString().c_str(), configName.toStdString().c_str());
                handle->waitForSystem();
                handle->updateSystemStateCache();
                return true;
            }
            catch (const CMMError&)
            {
                return false;
            }
        });
    }

    // Read exposure from the active camera path
    bool ScopeOneCore::readExposure(const QString& cameraIdOrAll, double& exposureMs) const
    {
        exposureMs = 0.0;
        const QString target = cameraIdOrAll.trimmed();
        if (target.isEmpty())
        {
            return false;
        }

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

        auto handle = core();
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

    // Merge native MMCore devices with agent camera labels
    QStringList ScopeOneCore::loadedDevices() const
    {
        auto handle = core();
        QStringList devices;
        try
        {
            devices = toQStringList(handle->getLoadedDevices());
        }
        catch (const CMMError&)
        {
            devices.clear();
        }

        // Agent cameras are not always loaded in the UI side MMCore instance
        for (const QString& cameraId : m_cameraIds)
        {
            if (!devices.contains(cameraId))
            {
                devices.append(cameraId);
            }
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
        if (device.isEmpty())
        {
            return {};
        }
        if (isAgentCamera(device))
        {
            return m_managers->mpcm->listProperties(device);
        }
        auto handle = core();
        try
        {
            return toQStringList(handle->getDevicePropertyNames(device.toStdString().c_str()));
        }
        catch (const CMMError&)
        {
            return {};
        }
    }

    // Read a property value from hardware or cache
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
            return m_managers->mpcm->getProperty(device, property);
        }
        auto handle = core();
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

    // Convert backend property types into UI strings
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
            return m_managers->mpcm->getPropertyType(device, property);
        }
        auto handle = core();
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

    // Check whether a property can be edited
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
            return m_managers->mpcm->isPropertyReadOnly(device, property);
        }
        auto handle = core();
        try
        {
            return handle->isPropertyReadOnly(device.toStdString().c_str(), property.toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return true;
        }
    }

    // Check whether a native property must be set before initialization
    bool ScopeOneCore::isPropertyPreInit(const QString& deviceLabel, const QString& name) const
    {
        const QString device = deviceLabel.trimmed();
        const QString property = name.trimmed();
        if (device.isEmpty() || property.isEmpty() || isAgentCamera(device))
        {
            return false;
        }
        auto handle = core();
        try
        {
            return handle->isPropertyPreInit(device.toStdString().c_str(), property.toStdString().c_str());
        }
        catch (const CMMError&)
        {
            return false;
        }
    }

    // Return allowed values for enumerated properties
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
            return m_managers->mpcm->getAllowedPropertyValues(device, property);
        }
        auto handle = core();
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
        if (device.isEmpty() || property.isEmpty())
        {
            return false;
        }
        if (isAgentCamera(device))
        {
            if (!m_managers->mpcm->hasPropertyLimits(device, property))
            {
                return false;
            }
            lower = m_managers->mpcm->getPropertyLowerLimit(device, property);
            upper = m_managers->mpcm->getPropertyUpperLimit(device, property);
            return true;
        }

        auto handle = core();
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

    // Set a property and refresh backend state after the device accepts it
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
            QString agentError;
            if (!m_managers->mpcm->setProperty(device, property, value, &agentError))
            {
                if (errorMessage)
                {
                    *errorMessage = agentError.isEmpty()
                                        ? QStringLiteral("Agent setProperty failed")
                                        : agentError;
                }
                return false;
            }
            return true;
        }

        auto handle = core();
        const bool isCamera = isNativeCamera(device);
        const QStringList runningPreviewIds = isCamera ? runningPreviewCameraIds() : QStringList{};
        const auto applyProperty = [&]() -> bool
        {
            try
            {
                handle->setProperty(device.toStdString().c_str(),
                                    property.toStdString().c_str(),
                                    value.toStdString().c_str());
                handle->waitForDevice(device.toStdString().c_str());
                handle->updateSystemStateCache();
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
        };
        return isCamera
                   ? withSuspendedPreviews(this, runningPreviewIds, applyProperty)
                   : applyProperty();
    }

    bool ScopeOneCore::isRealTimeProcessingEnabled() const
    {
        return m_managers->imageProcessingManager->isRealTimeProcessingEnabled();
    }

    // Toggle live processing without changing the module list
    void ScopeOneCore::setRealTimeProcessingEnabled(bool enabled)
    {
        m_managers->imageProcessingManager->enableRealTimeProcessing(enabled);
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
        const int nextBitDepth = bitDepth == ProcessingBitDepth::Bit16 ? 16 : 8;
        if (m_managers->imageProcessingManager->processingBitDepth() == nextBitDepth)
        {
            return true;
        }

        m_managers->imageProcessingManager->setProcessingBitDepth(nextBitDepth);
        emit processingSettingsChanged();
        return true;
    }

    // Queue one frame for asynchronous processing
    void ScopeOneCore::processFrameAsync(const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return;
        }
        m_managers->imageProcessingManager->processFrameAsync(frame);
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

    // Export processing module descriptions for the UI
    QList<scopeone::core::ScopeOneCore::ProcessingModuleInfo> ScopeOneCore::processingModules() const
    {
        QList<ProcessingModuleInfo> out;
        ProcessingPipeline* pipeline = m_managers->imageProcessingManager->pipeline();

        out.reserve(pipeline->getModuleCount());
        pipeline->forEachModule([&out](const ProcessingModule* module)
        {
            ProcessingModuleInfo info;
            info.setKind(processingModuleKind(module));
            info.setName(module->getModuleName());
            info.setParameters(module->getParameters());
            out.append(std::move(info));
        });
        return out;
    }

    // Add a processing module to the editable pipeline
    bool ScopeOneCore::addProcessingModule(ProcessingModuleKind kind)
    {
        ProcessingPipeline* pipeline = m_managers->imageProcessingManager->pipeline();

        std::unique_ptr<ProcessingModule> module;
        switch (kind)
        {
        case ProcessingModuleKind::FFT:
            module = std::make_unique<FFTModule>(pipeline);
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
        case ProcessingModuleKind::DifferentialRolling:
            module = std::make_unique<DifferentialRollingModule>(pipeline);
            break;
        case ProcessingModuleKind::Unknown:
            return false;
        }

        pipeline->addModule(std::move(module));
        m_managers->imageProcessingManager->clearRuntimePipelines();
        emit processingModulesChanged();
        return true;
    }

    // Remove a processing module and drop stale runtime clones
    bool ScopeOneCore::removeProcessingModule(int index)
    {
        ProcessingPipeline* pipeline = m_managers->imageProcessingManager->pipeline();
        if (!pipeline->removeModule(index))
        {
            return false;
        }
        m_managers->imageProcessingManager->clearRuntimePipelines();
        emit processingModulesChanged();
        return true;
    }

    // Update module parameters and rebuild per camera runtime modules
    bool ScopeOneCore::setProcessingModuleParameters(int index, const QVariantMap& parameters)
    {
        ProcessingPipeline* pipeline = m_managers->imageProcessingManager->pipeline();
        const bool updated = pipeline->withModule(index, [&parameters](ProcessingModule* module)
        {
            module->setParameters(parameters);
            return true;
        });
        if (!updated)
        {
            return false;
        }
        m_managers->imageProcessingManager->clearRuntimePipelines();
        emit processingModulesChanged();
        return true;
    }

    // Reset stateful module buffers through the public facade
    bool ScopeOneCore::resetProcessingModuleState(int index)
    {
        ProcessingPipeline* pipeline = m_managers->imageProcessingManager->pipeline();
        const bool reset = pipeline->withModule(index, [](ProcessingModule* module)
        {
            if (auto* background = qobject_cast<BackgroundCalibrationModule*>(module))
            {
                background->resetCalibration();
                return true;
            }
            if (auto* differentialRolling = qobject_cast<DifferentialRollingModule*>(module))
            {
                differentialRolling->resetBuffer();
                return true;
            }
            return false;
        });
        if (reset)
        {
            m_managers->imageProcessingManager->clearRuntimePipelines();
            emit processingModulesChanged();
            return true;
        }
        return false;
    }

    void ScopeOneCore::setRecordingMaxPendingWriteBytes(qint64 bytes)
    {
        m_managers->recordingManager->setRecordedMaxBytes(bytes);
    }

    qint64 ScopeOneCore::recordingMaxPendingWriteBytes() const
    {
        return m_managers->recordingManager->recordedMaxBytes();
    }

    // Start recording and suspend preview during MDA motion
    bool ScopeOneCore::startRecording(const RecordingSettings& settings, const QStringList& activeCameraIds)
    {
        RecordingSettings settingsSnapshot = settings;
        const bool useMda = !settingsSnapshot.positions.empty() || !settingsSnapshot.zPositions.empty();
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

        if (settingsSnapshot.metadataFileName.trimmed().isEmpty())
        {
            settingsSnapshot.metadataFileName = recordingMetadataFileName(settingsSnapshot.baseName);
        }
        if (settingsSnapshot.sessionMetadataJson.isEmpty())
        {
            settingsSnapshot.sessionMetadataJson = buildDevicePropertyMetadataJson(*this);
        }

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

        const bool started = m_managers->recordingManager->start(
            toRecordingManagerSettings(settingsSnapshot),
            activeCameraIds);
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

    // Save a completed session with device metadata attached
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

    // Save a completed session on a worker thread
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
