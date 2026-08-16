#include "internal/RecordingManager.h"

#include "scopeone/CameraProvider.h"
#include "MMCore.h"
#include <scopewriter/ScopeWriter.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QSysInfo>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>

namespace scopeone::core::internal
{
    using scopeone::core::RecordingFormat;
    using scopeone::core::ImageFrame;
    using scopeone::core::ImagePixelFormat;
    using scopeone::core::kRecordingPhaseIdle;
    using scopeone::core::kRecordingPhaseRecording;
    using scopeone::core::kRecordingPhaseRecordingBurst;
    using scopeone::core::kRecordingPhaseRecordingMda;
    using scopeone::core::kRecordingPhaseWaitingNextBurst;
    using scopeone::core::kRecordingPhaseStopped;

    namespace
    {
        QString recordingFormatName(scopeone::core::RecordingFormat format)
        {
            switch (format)
            {
            case scopeone::core::RecordingFormat::OmeTiff:
                return QStringLiteral("OME-TIFF");
            case scopeone::core::RecordingFormat::OmeZarr:
                return QStringLiteral("OME-Zarr");
            case scopeone::core::RecordingFormat::Tiff:
                return QStringLiteral("TIFF");
            case scopeone::core::RecordingFormat::Binary:
                return QStringLiteral("Binary");
            }
            return QStringLiteral("Unknown");
        }

        QString recordingExtension(scopeone::core::RecordingFormat format)
        {
            switch (format)
            {
            case scopeone::core::RecordingFormat::OmeTiff:
                return QStringLiteral(".ome.tiff");
            case scopeone::core::RecordingFormat::OmeZarr:
                return QStringLiteral(".ome.zarr");
            case scopeone::core::RecordingFormat::Tiff:
                return QStringLiteral(".tif");
            case scopeone::core::RecordingFormat::Binary:
                return QStringLiteral(".bin");
            }
            return QStringLiteral(".dat");
        }

        bool readFiniteNumber(const QJsonValue& value, double& number)
        {
            bool ok = false;
            number = value.isDouble() ? value.toDouble() : value.toString().toDouble(&ok);
            return (value.isDouble() || ok) && std::isfinite(number);
        }

        double uniformTimeIncrementMs(const ExperimentPlan& plan)
        {
            if (plan.framesPerBurst < 2 || (plan.burstMode && plan.targetBursts > 1))
            {
                return 0.0;
            }
            return plan.mdaIntervalMs > 0.0 ? plan.mdaIntervalMs : 0.0;
        }

        bool requiresFrameInfo(scopeone::core::RecordingFormat format)
        {
            return format == scopeone::core::RecordingFormat::Binary;
        }

        QString ensureExtensionName(const QString& baseName, const QString& extension)
        {
            if (baseName.endsWith(extension, Qt::CaseInsensitive))
            {
                return baseName;
            }
            return baseName + extension;
        }

        QString sessionOutputDir(const QString& saveDir, const QString& baseName)
        {
            return QDir(saveDir).filePath(baseName.trimmed());
        }

        quint64 currentTimestampNs()
        {
            return static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }

        QString displayOutputPath(const QString& path)
        {
            const QString trimmedPath = path.trimmed();
            return trimmedPath.isEmpty()
                       ? QString()
                       : QDir::toNativeSeparators(QDir::cleanPath(trimmedPath));
        }

        QString saveSuccessMessage(const QString& message, const QString& outputPath)
        {
            const QString displayPath = displayOutputPath(outputPath);
            return displayPath.isEmpty()
                       ? message
                       : QStringLiteral("%1 in %2").arg(message, displayPath);
        }

        struct SessionOutputInfo
        {
            QString outputDir;
            QString metadataFileName;
        };

        struct CameraOutputPaths
        {
            QString rawPath;
            QString frameInfoPath;
        };

        QString buildSessionFilePath(const QString& dir,
                                     const QString& baseName,
                                     const QString& cameraId,
                                     const QString& extension,
                                     const QString& suffix = QString())
        {
            QString name = baseName;
            if (!cameraId.isEmpty())
            {
                name += "_" + cameraId;
            }
            if (!suffix.isEmpty())
            {
                name += suffix;
            }
            name = ensureExtensionName(name, extension);
            return QDir(dir).filePath(name);
        }

        QString metadataFileNameForPlan(const QString& baseName, const QString& metadataFileName)
        {
            const QString trimmedMetadataFileName = metadataFileName.trimmed();
            if (!trimmedMetadataFileName.isEmpty())
            {
                return ensureExtensionName(trimmedMetadataFileName, QStringLiteral(".json"));
            }
            return ensureExtensionName(baseName + QStringLiteral("_metadata"), QStringLiteral(".json"));
        }

        // Reserves a unique recording directory without overwriting existing output
        bool reserveUniqueSessionOutput(ExperimentPlan& plan,
                                        SessionOutputInfo& output,
                                        QString& errorMessage)
        {
            static std::mutex allocationMutex;
            std::lock_guard<std::mutex> lock(allocationMutex);

            const QString requestedBaseName = plan.baseName.trimmed();
            QDir saveRoot(plan.saveDir.trimmed());
            if (!saveRoot.exists() && !QDir().mkpath(saveRoot.path()))
            {
                errorMessage = QStringLiteral("Failed to create save directory: %1").arg(saveRoot.path());
                return false;
            }

            QString resolvedBaseName = requestedBaseName;
            int suffix = 1;
            while (true)
            {
                output.outputDir = saveRoot.filePath(resolvedBaseName);
                if (!QFileInfo::exists(output.outputDir) && saveRoot.mkdir(resolvedBaseName))
                {
                    break;
                }
                if (!QFileInfo::exists(output.outputDir))
                {
                    errorMessage = QStringLiteral("Failed to reserve recording directory: %1")
                                       .arg(output.outputDir);
                    return false;
                }
                resolvedBaseName = QStringLiteral("%1_%2")
                                       .arg(requestedBaseName)
                                       .arg(suffix++, 3, 10, QChar('0'));
            }

            const QString requestedMetadataFileName = plan.metadataFileName.trimmed();
            const QString defaultMetadataFileName = metadataFileNameForPlan(requestedBaseName, QString());
            const QString normalizedMetadataFileName = metadataFileNameForPlan(requestedBaseName,
                                                                                requestedMetadataFileName);
            const bool usesDefaultMetadataFileName = requestedMetadataFileName.isEmpty()
                                                     || normalizedMetadataFileName.compare(
                                                            defaultMetadataFileName,
                                                            Qt::CaseInsensitive) == 0;
            plan.baseName = resolvedBaseName;
            plan.metadataFileName = usesDefaultMetadataFileName
                                        ? metadataFileNameForPlan(resolvedBaseName, QString())
                                        : metadataFileNameForPlan(resolvedBaseName,
                                                                  requestedMetadataFileName);
            output.metadataFileName = plan.metadataFileName;
            return true;
        }

        QString sessionDocumentPath(const QString& saveDir,
                                    const QString& baseName,
                                    const QString& metadataFileName)
        {
            return QDir(sessionOutputDir(saveDir, baseName))
                .filePath(metadataFileNameForPlan(baseName, metadataFileName));
        }

        CameraOutputPaths buildCameraOutputPaths(const QString& outputDir,
                                                 const QString& baseName,
                                                 const QString& cameraId,
                                                 const ExperimentPlan& plan)
        {
            CameraOutputPaths paths;
            paths.rawPath = buildSessionFilePath(outputDir,
                                                baseName,
                                                cameraId,
                                                recordingExtension(plan.format));
            if (plan.format == scopeone::core::RecordingFormat::OmeTiff
                && plan.positions.size() > 1)
            {
                paths.rawPath.chop(QStringLiteral(".ome.tiff").size());
            }
            if (requiresFrameInfo(plan.format))
            {
                paths.frameInfoPath = buildSessionFilePath(outputDir,
                                                           baseName,
                                                           cameraId,
                                                           QStringLiteral(".csv"),
                                                           QStringLiteral("_frameinfo"));
            }
            return paths;
        }

        struct FramePayloadView
        {
            const uchar* externalData{nullptr};
            qint64 byteCount{0};
            QByteArray owned;

            const uchar* data() const
            {
                return owned.isEmpty() ? externalData : reinterpret_cast<const uchar*>(owned.constData());
            }
        };

        qint64 packedFrameByteCount(const ImageFrame& frame)
        {
            const int bytesPerPixel = frame.bytesPerPixel();
            if (frame.width <= 0 || frame.height <= 0 || bytesPerPixel <= 0)
            {
                return 0;
            }
            return static_cast<qint64>(frame.width) * static_cast<qint64>(frame.height) * bytesPerPixel;
        }

        double physicalPixelSizeUm(int imageExtent,
                                   int sourceExtent,
                                   double cameraPixelSizeUm)
        {
            if (cameraPixelSizeUm <= 0.0 || imageExtent <= 0 || sourceExtent <= 0)
            {
                return cameraPixelSizeUm;
            }
            return cameraPixelSizeUm * static_cast<double>(sourceExtent)
                / static_cast<double>(imageExtent);
        }

        FramePayloadView framePayloadForWrite(const ImageFrame& frame, RecordingFormat format)
        {
            FramePayloadView payload;
            const qint64 payloadBytes = frame.payloadByteCount();
            if (!frame.isValid() || payloadBytes <= 0)
            {
                return payload;
            }

            payload.externalData = reinterpret_cast<const uchar*>(frame.bytes.constData());
            payload.byteCount = payloadBytes;
            if (format == RecordingFormat::Binary)
            {
                return payload;
            }

            const int bytesPerPixel = frame.bytesPerPixel();
            const qint64 rowBytes = static_cast<qint64>(frame.width) * bytesPerPixel;
            const qint64 packedBytes = packedFrameByteCount(frame);
            if (rowBytes <= 0 || packedBytes <= 0)
            {
                return FramePayloadView{};
            }
            if (frame.stride == rowBytes)
            {
                payload.byteCount = packedBytes;
                return payload;
            }

            payload.owned.resize(static_cast<qsizetype>(packedBytes));
            const char* src = frame.bytes.constData();
            char* dst = payload.owned.data();
            for (int y = 0; y < frame.height; ++y)
            {
                std::memcpy(dst + y * rowBytes,
                            src + static_cast<qint64>(y) * frame.stride,
                            static_cast<size_t>(rowBytes));
            }
            payload.byteCount = packedBytes;
            return payload;
        }

        void discardIncompleteOutput(const QString& path)
        {
            const QFileInfo info(path);
            if (info.isDir())
            {
                QDir(info.absoluteFilePath()).removeRecursively();
            }
            else
            {
                QFile::remove(info.absoluteFilePath());
            }
        }

        bool prepareSessionOutput(const QString& saveDir,
                                  const QString& baseName,
                                  const QString& metadataFileName,
                                  SessionOutputInfo& output,
                                  QString& errorMessage)
        {
            output = SessionOutputInfo{};
            output.outputDir = sessionOutputDir(saveDir, baseName);
            if (!QDir().mkpath(output.outputDir))
            {
                errorMessage = QStringLiteral("Failed to create save directory: %1").arg(output.outputDir);
                return false;
            }

            output.metadataFileName = metadataFileNameForPlan(baseName, metadataFileName);
            return true;
        }

        // Resolves the output directory reported for a completed session
        QString savedSessionOutputDir(const ScopeOneCore::RecordingSessionData& session)
        {
            for (auto it = session.outputFiles().constBegin(); it != session.outputFiles().constEnd(); ++it)
            {
                const QString path = !it.value().rawPath.isEmpty()
                                         ? it.value().rawPath
                                         : it.value().frameInfoPath;
                if (!path.isEmpty())
                {
                    return QFileInfo(path).absolutePath();
                }
            }

            const auto& plan = session.capturePlan();
            if (!plan.saveDir.trimmed().isEmpty() && !plan.baseName.trimmed().isEmpty())
            {
                return sessionOutputDir(plan.saveDir, plan.baseName);
            }
            return {};
        }

        int tiffStorageBitsForFormat(ImagePixelFormat pixelFormat)
        {
            if (pixelFormat == ImagePixelFormat::Mono8) return 8;
            if (pixelFormat == ImagePixelFormat::Mono16) return 16;
            return 0;
        }

        class SaveBackend
        {
        public:
            struct TiffOptions
            {
                bool useDeflate{true};
                int zipQuality{6};
            };

            // Closes any active writer before releasing backend resources
            ~SaveBackend() { (void)stopStack(); }

            // Opens a streaming writer from the capture plan and camera metadata
            // Keeps writer options explicit for GCC compatibility
            bool startStackRaw(const QString& filePath,
                               const QString& frameInfoPath,
                               const QString& metadataFileName,
                               scopeone::core::RecordingFormat recordingFormat,
                               int width,
                               int height,
                               ImagePixelFormat pixelFormat,
                               int bitsPerSample,
                               const ExperimentPlan& plan,
                               double physicalSizeXUm,
                               double physicalSizeYUm,
                               quint64 acquisitionStartTimestampNs,
                               const QString& imageName,
                               const QJsonObject& cameraProperties,
                               const TiffOptions& tiff)
            {
                (void)stopStack();
                m_lastError.clear();
                m_omePlan = plan;

                if (recordingFormat == scopeone::core::RecordingFormat::OmeTiff
                    || recordingFormat == scopeone::core::RecordingFormat::OmeZarr
                    || recordingFormat == scopeone::core::RecordingFormat::Tiff
                    || recordingFormat == scopeone::core::RecordingFormat::Binary)
                {
                    const int storageBits = tiffStorageBitsForFormat(pixelFormat);
                    if (storageBits == 0)
                    {
                        m_lastError = QStringLiteral("Unsupported bit depth");
                        return false;
                    }

                    scopewriter::WriterSettings settings;
                    switch (recordingFormat)
                    {
                    case scopeone::core::RecordingFormat::OmeTiff:
                        settings.format = scopewriter::Format::OmeTiff;
                        break;
                    case scopeone::core::RecordingFormat::OmeZarr:
                        settings.format = scopewriter::Format::OmeZarr;
                        break;
                    case scopeone::core::RecordingFormat::Tiff:
                        settings.format = scopewriter::Format::Tiff;
                        break;
                    case scopeone::core::RecordingFormat::Binary:
                        settings.format = scopewriter::Format::Binary;
                        break;
                    }
#if defined(_WIN32)
                    settings.outputPath = std::filesystem::path(filePath.toStdWString());
                    settings.frameMetadataPath = std::filesystem::path(frameInfoPath.toStdWString());
#else
                    settings.outputPath = std::filesystem::path(filePath.toStdString());
                    settings.frameMetadataPath = std::filesystem::path(frameInfoPath.toStdString());
#endif
                    settings.linkedMetadataFile = metadataFileName.toStdString();
                    settings.width = width;
                    settings.height = height;
                    settings.pixelType = pixelFormat == ImagePixelFormat::Mono8
                                             ? scopewriter::PixelType::UInt8
                                             : scopewriter::PixelType::UInt16;
                    settings.significantBits = bitsPerSample;
                    settings.positionCount = (std::max)(1, static_cast<int>(plan.positions.size()));
                    settings.timeCount = (std::max)(1, plan.framesPerBurst)
                        * (plan.burstMode ? (std::max)(1, plan.targetBursts) : 1);
                    settings.channelCount = 1;
                    settings.zCount = (std::max)(1, static_cast<int>(plan.zPositions.size()));
                    const auto timeAxis = std::find(plan.order.begin(),
                                                    plan.order.end(),
                                                    RecordingAxis::Time);
                    const auto zAxis = std::find(plan.order.begin(),
                                                 plan.order.end(),
                                                 RecordingAxis::Z);
                    settings.acquisitionOrder = zAxis < timeAxis ? "ZTC" : "TZC";
                    settings.physicalSizeXUm = physicalSizeXUm;
                    settings.physicalSizeYUm = physicalSizeYUm;
                    settings.timeIncrementMs = uniformTimeIncrementMs(plan);
                    if (plan.zPositions.size() > 1)
                    {
                        const double zStepUm = std::abs(plan.zPositions[1] - plan.zPositions[0]);
                        bool uniform = zStepUm > 0.0;
                        for (size_t index = 2; uniform && index < plan.zPositions.size(); ++index)
                        {
                            const double step = std::abs(plan.zPositions[index]
                                                         - plan.zPositions[index - 1]);
                            uniform = std::abs(step - zStepUm)
                                <= (std::max)(1e-9, zStepUm * 1e-9);
                        }
                        if (uniform)
                        {
                            settings.physicalSizeZUm = zStepUm;
                        }
                    }
                    settings.acquisitionStartTimestampNs = acquisitionStartTimestampNs;
                    settings.imageName = imageName.toStdString();
                    settings.creator = "ScopeOne";
                    settings.channels.push_back(scopewriter::ChannelMetadata{
                        .name = settings.imageName
                    });
                    settings.positions.reserve(static_cast<std::size_t>(settings.positionCount));
                    if (plan.positions.empty())
                    {
                        settings.positions.push_back(scopewriter::PositionMetadata{
                            .name = "Position 1"
                        });
                    }
                    else
                    {
                        for (std::size_t index = 0; index < plan.positions.size(); ++index)
                        {
                            const auto& position = plan.positions[index];
                            settings.positions.push_back(scopewriter::PositionMetadata{
                                .name = "Position " + std::to_string(index + 1),
                                .xUm = position.x(),
                                .yUm = position.y()
                            });
                        }
                    }
                    double value = 0.0;
                    if (readFiniteNumber(cameraProperties.value(QStringLiteral("Exposure")), value)
                        && value > 0.0)
                    {
                        settings.defaultExposureMs = value;
                    }
                    if (readFiniteNumber(cameraProperties.value(QStringLiteral("Offset")), value))
                    {
                        settings.detector.offset = value;
                    }
                    settings.enableCompression = tiff.useDeflate;
                    settings.compressionLevel = tiff.zipQuality;

                    auto writer = std::make_unique<scopewriter::Writer>();
                    if (!writer->open(settings))
                    {
                        m_lastError = QString::fromStdString(writer->lastError());
                        return false;
                    }
                    m_writer = std::move(writer);
                    return true;
                }

                m_lastError = QStringLiteral("Unsupported recording format");
                return false;
            }

            // Appends one frame with its acquisition coordinates and timing
            bool appendRaw(const uchar* data,
                           qint64 rawBytes,
                           const ImageFrame& frame,
                           const AcquisitionEvent* event = nullptr)
            {
                if (!data || !m_writer)
                {
                    return false;
                }
                if (rawBytes <= 0)
                {
                    m_lastError = QStringLiteral("Invalid frame size");
                    return false;
                }
                scopewriter::FrameMetadata metadata;
                metadata.cameraId = frame.cameraId.toStdString();
                metadata.frameIndex = frame.frameIndex;
                metadata.timestampNs = frame.timestampNs;
                metadata.stride = m_omePlan.format == RecordingFormat::Binary
                    ? static_cast<std::size_t>(frame.stride)
                    : static_cast<std::size_t>(frame.width * frame.bytesPerPixel());
                metadata.sourceRoiX = frame.sourceRoiX;
                metadata.sourceRoiY = frame.sourceRoiY;
                metadata.sourceRoiWidth = frame.sourceRoiWidth;
                metadata.sourceRoiHeight = frame.sourceRoiHeight;
                if (event)
                {
                    metadata.positionIndex = event->positionIndex;
                    metadata.t = static_cast<std::int64_t>(event->burstIndex)
                            * (std::max)(1, m_omePlan.framesPerBurst)
                        + event->timeIndex;
                    metadata.z = event->zIndex;
                    metadata.exposureMs = event->exposureMs;
                    if (event->hasXY)
                    {
                        metadata.positionXUm = event->x;
                        metadata.positionYUm = event->y;
                    }
                    if (event->hasZ)
                    {
                        metadata.positionZUm = event->z;
                    }
                }
                if (!m_writer->append(data,
                                      static_cast<std::size_t>(rawBytes),
                                      metadata))
                {
                    m_lastError = QString::fromStdString(m_writer->lastError());
                    return false;
                }
                return true;
            }

            // Flushes and closes the active writer
            bool stopStack()
            {
                if (!m_writer)
                {
                    return true;
                }
                const bool success = m_writer->close();
                if (!success)
                {
                    m_lastError = QString::fromStdString(m_writer->lastError());
                }
                m_writer.reset();
                return success;
            }

            // Returns the latest writer failure
            QString lastError() const { return m_lastError; }

        private:
            QString m_lastError;
            ExperimentPlan m_omePlan;
            std::unique_ptr<scopewriter::Writer> m_writer;
        };
    } // namespace

    struct RecordingManager::WriteTask
    {
        ImageFrame frame;
        AcquisitionEvent event;
        bool hasEvent{false};
    };

    struct RecordingManager::CameraOutput
    {
        QString cameraId;
        QString rawPath;
        QString frameInfoPath;
        QString metadataFileName;
        QJsonObject cameraProperties;
        double pixelSizeUm{0.0};
        std::unique_ptr<SaveBackend> backend;
        quint64 acquisitionStartTimestampNs{0};
        int width{0};
        int height{0};
        int bits{0};
        ImagePixelFormat pixelFormat{ImagePixelFormat::Invalid};
        std::deque<WriteTask> writeQueue;
        std::mutex queueMutex;
        std::condition_variable writeCondition;
        std::thread writerThread;
        std::atomic_bool writerFinished{false};
        bool stopRequested{false};
        qint64 framesWritten{0};
    };

    // Initializes serialized finalization and rate limited writer telemetry
    RecordingManager::RecordingManager(QObject* parent)
        : QObject(parent)
    {
        m_finalizationThreadPool.setMaxThreadCount(1);
        m_writerStatusTimer.setInterval(100);
        connect(&m_writerStatusTimer, &QTimer::timeout, this, [this]()
        {
            if (m_writerStatusDirty.load(std::memory_order_acquire))
            {
                emitWriterStatus();
            }
        });
    }

    // Stops recording and writer threads during teardown
    RecordingManager::~RecordingManager()
    {
        shutdown();
    }

    // Stops recording and joins acquisition and writer workers
    void RecordingManager::shutdown()
    {
        stop();
        if (m_mdaState.manager)
        {
            m_mdaState.manager->cancelAndWait();
        }
        stopStreamingOutputs();
        m_finalizationThreadPool.waitForDone();
    }

    // Sets the maximum queued write buffer size
    void RecordingManager::setRecordedMaxBytes(qint64 bytes)
    {
        if (bytes <= 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        m_writerState.recordedMaxBytes = static_cast<size_t>(bytes);
        m_writerState.status.setMaxPendingWriteBytes(bytes);
    }

    // Returns the maximum queued write buffer size
    qint64 RecordingManager::recordedMaxBytes() const
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        return static_cast<qint64>(m_writerState.recordedMaxBytes);
    }

    // Marks writer telemetry for the next bounded UI update
    void RecordingManager::markWriterStatusDirty()
    {
        m_writerStatusDirty.store(true, std::memory_order_release);
    }

    // Emits a snapshot of writer status
    void RecordingManager::emitWriterStatus()
    {
        m_writerStatusDirty.store(false, std::memory_order_release);
        RecordingWriterStatus status;
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            status = m_writerState.status;
            status.setPendingWriteBytes(static_cast<qint64>(m_writerState.pendingWriteBytes));
            status.setMaxPendingWriteBytes(static_cast<qint64>(m_writerState.recordedMaxBytes));
        }
        qint64 capturedFrames = 0;
        for (auto it = m_captureState.framesCapturedTotal.constBegin();
             it != m_captureState.framesCapturedTotal.constEnd();
             ++it)
        {
            capturedFrames += it.value();
        }
        status.setFramesCaptured(capturedFrames);
        if (m_activeSession)
        {
            m_activeSession->setWriterStatusSnapshot(status);
        }
        emit writerStatusChanged(status);
    }

    // Updates writer phase and optional error state
    void RecordingManager::setWriterStatus(RecordingWriterPhase phase, const QString& errorMessage)
    {
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.status.setPhase(phase, errorMessage);
            m_writerState.status.setPendingWriteBytes(static_cast<qint64>(m_writerState.pendingWriteBytes));
            m_writerState.status.setMaxPendingWriteBytes(static_cast<qint64>(m_writerState.recordedMaxBytes));
        }
        emitWriterStatus();
    }

    // Returns a thread safe snapshot of the current writer error
    QString RecordingManager::writerErrorSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        return m_writerState.writerError;
    }

    // Replaces the writer error under the shared writer lock
    void RecordingManager::setWriterError(const QString& errorMessage)
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        m_writerState.writerError = errorMessage;
    }

    // Update the persisted save result on a recording session
    QString RecordingManager::updateSessionResult(
        RecordingSessionData& session,
        const QString& result,
        bool saved)
    {
        session.setSaveResult(saved, result);
        return result;
    }

    // Builds the capture plan before recording starts
    bool RecordingManager::buildCapturePlan(const ExperimentPlan& requestedPlan,
                                            const QStringList& activeCameraIds,
                                            ExperimentPlan& plan,
                                            QString& errorMessage) const
    {
        plan = requestedPlan;
        plan.cameraIds.clear();
        const QStringList requestedCameraIds = activeCameraIds.isEmpty()
                                                   ? requestedPlan.cameraIds
                                                   : activeCameraIds;
        for (const QString& cameraId : requestedCameraIds)
        {
            const QString trimmedCameraId = cameraId.trimmed();
            if (!trimmedCameraId.isEmpty() && !plan.cameraIds.contains(trimmedCameraId))
            {
                plan.cameraIds.append(trimmedCameraId);
            }
        }
        if (plan.cameraIds.isEmpty() && m_mmcore)
        {
            plan.cameraIds << QStringLiteral("Camera");
        }
        if (plan.cameraIds.isEmpty())
        {
            errorMessage = QStringLiteral("No cameras available for recording");
            return false;
        }
        if (requestedPlan.streamToDisk && requestedPlan.saveDir.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Save directory is empty");
            return false;
        }
        if (requestedPlan.streamToDisk && requestedPlan.baseName.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Base name is empty");
            return false;
        }
        if (plan.experimentId.trimmed().isEmpty())
        {
            plan.experimentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        plan.targetBursts = requestedPlan.burstMode ? requestedPlan.targetBursts : 1;
        plan.saveDir = requestedPlan.saveDir.trimmed();
        plan.baseName = requestedPlan.baseName.trimmed();
        plan.metadataFileName = requestedPlan.metadataFileName.trimmed();
        if (!validateExperimentPlan(plan, &errorMessage))
        {
            return false;
        }
        if (!planUsesMda(plan) && !planStreamsMda(plan) && !m_cameraProvider && !m_latestFrameFetcher)
        {
            errorMessage = QStringLiteral("Frame source is not available for recording");
            return false;
        }
        return true;
    }

    // Returns whether spatial axes require explicit MDA events
    bool RecordingManager::planUsesMda(const ExperimentPlan& plan) const
    {
        return !plan.positions.empty() || !plan.zPositions.empty();
    }

    // Returns whether native preview recording uses the requested frame interval
    bool RecordingManager::planStreamsMda(const ExperimentPlan& plan) const
    {
        return m_mmcore && plan.cameraIds.size() == 1 && !planUsesMda(plan);
    }

    // Resets counters and MDA state for a new capture plan
    void RecordingManager::resetCaptureState(const ExperimentPlan& plan)
    {
        m_captureState.activeCameraIds = plan.cameraIds;
        m_captureState.format = plan.format;
        m_captureState.enableCompression = plan.enableCompression;
        m_captureState.compressionLevel = plan.compressionLevel;
        m_captureState.framesPerBurst = plan.framesPerBurst;
        m_captureState.targetBursts = plan.targetBursts;
        m_captureState.burstMode = plan.burstMode;
        m_captureState.burstIntervalMs = plan.burstIntervalMs;
        m_captureState.streamToDisk = plan.streamToDisk;
        m_captureState.lastFrameIndex.clear();
        m_captureState.framesCapturedThisBurst.clear();
        m_captureState.framesCapturedTotal.clear();
        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            m_captureState.framesCapturedThisBurst[cameraId] = 0;
            m_captureState.framesCapturedTotal[cameraId] = 0;
        }
        m_captureState.currentBurst = 0;
        m_captureState.waitingBetweenBursts = false;
        m_captureState.lastBurstEndMs = 0;
        m_captureState.phase = kRecordingPhaseIdle;

        m_mdaState.plan = plan;
        m_mdaState.hasLastEvent = false;
    }

    // Prepares a fresh session object for the next recording
    void RecordingManager::resetSessionState(const ExperimentPlan& plan,
                                             const QJsonObject& deviceProperties,
                                             const QHash<QString, double>& cameraPixelSizesUm)
    {
        m_activeSession = std::make_shared<RecordingSessionData>();
        m_activeSession->setCapturePlan(plan);
        SoftwareSnapshot software;
        software.applicationVersion = QCoreApplication::applicationVersion();
        software.coreVersion = ScopeOneCore::getVersion();
        software.mmCoreVersion = ScopeOneCore::getMMCoreVersion();
        software.libTiffVersion = ScopeOneCore::getLibTiffVersion();
        software.zlibVersion = ScopeOneCore::getZlibVersion();
        software.operatingSystem = QSysInfo::prettyProductName();
        m_activeSession->setSoftwareSnapshot(software);
        m_activeSession->setDeviceProperties(deviceProperties);
        m_activeSession->setCameraPixelSizesUm(cameraPixelSizesUm);
        m_activeSession->prepareForSave(plan.streamToDisk, recordedMaxBytes());
        m_activeSession->setStartedTimestampNs(currentTimestampNs());
        m_activeSession->setRunState(ExperimentRunState::Running);
    }

    // Writes final save result information into the active session
    void RecordingManager::finalizeActiveSession(ExperimentRunState state, const QString& errorMessage)
    {
        QString finalError = errorMessage;
        const QString writerError = writerErrorSnapshot();
        if (finalError.isEmpty() && !writerError.isEmpty())
        {
            finalError = writerError;
        }
        if (state == ExperimentRunState::Completed
            && m_captureState.streamToDisk
            && finalError.isEmpty()
            && !m_activeSession->hasRecordedOutput())
        {
            finalError = QStringLiteral("No frames captured");
        }
        if (!finalError.isEmpty())
        {
            state = ExperimentRunState::Failed;
        }
        m_activeSession->setRunState(state, currentTimestampNs(), finalError);

        if (!m_captureState.streamToDisk)
        {
            return;
        }
        if (state == ExperimentRunState::Canceled)
        {
            const bool hasOutput = m_activeSession->hasRecordedOutput();
            const QString result = hasOutput
                                       ? saveSuccessMessage(
                                           QStringLiteral("Success: Saved partial %1 recording")
                                               .arg(recordingFormatName(m_captureState.format)),
                                           savedSessionOutputDir(*m_activeSession))
                                       : QStringLiteral("Canceled: No frames captured");
            updateSessionResult(*m_activeSession, result, hasOutput);
            return;
        }
        const QString result = finalError.isEmpty()
                                   ? saveSuccessMessage(
                                       QStringLiteral("Success: Saved %1 recording during acquisition").arg(
                                           recordingFormatName(m_captureState.format)),
                                       savedSessionOutputDir(*m_activeSession))
                                   : QStringLiteral("Error: %1").arg(finalError);
        updateSessionResult(*m_activeSession, result, finalError.isEmpty());
    }

    // Writes the complete experiment document beside recorded payload files
    bool RecordingManager::writeSessionDocument(const RecordingSessionData& session,
                                                 QString& errorMessage)
    {
        const ExperimentPlan& plan = session.capturePlan();
        return saveExperimentDocument(sessionDocumentPath(plan.saveDir,
                                                          plan.baseName,
                                                          plan.metadataFileName),
                                      session.experimentDocument(),
                                      &errorMessage);
    }

    // Starts one writer thread per active camera output
    bool RecordingManager::startStreamingOutputs(const ExperimentPlan& plan)
    {
        stopStreamingOutputs();

        setWriterError(QString());
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.status = RecordingWriterStatus{};
            m_writerState.status.setMaxPendingWriteBytes(static_cast<qint64>(m_writerState.recordedMaxBytes));
        }
        const quint64 acquisitionStartTimestampNs =
            m_activeSession->experimentDocument().startedTimestampNs;
        setWriterStatus(RecordingWriterPhase::Starting);

        SessionOutputInfo outputInfo;
        QString outputError;
        if (!prepareSessionOutput(plan.saveDir,
                                  plan.baseName,
                                  plan.metadataFileName,
                                  outputInfo,
                                  outputError))
        {
            setWriterError(outputError);
            setWriterStatus(RecordingWriterPhase::Failed, outputError);
            return false;
        }
        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            auto output = std::make_shared<CameraOutput>();
            output->cameraId = cameraId;
            const CameraOutputPaths paths = buildCameraOutputPaths(outputInfo.outputDir,
                                                                   plan.baseName,
                                                                   cameraId,
                                                                   plan);
            output->rawPath = paths.rawPath;
            output->metadataFileName = outputInfo.metadataFileName;
            output->acquisitionStartTimestampNs = acquisitionStartTimestampNs;
            output->cameraProperties = m_activeSession->experimentDocument()
                                           .deviceProperties.value(cameraId).toObject();
            output->pixelSizeUm = m_activeSession->cameraPixelSizeUm(cameraId);
            if (requiresFrameInfo(plan.format))
            {
                output->frameInfoPath = paths.frameInfoPath;
            }

            m_writerState.cameraOutputs.insert(cameraId, output);
        }

        const quint64 generation = m_captureState.generation;
        for (auto it = m_writerState.cameraOutputs.begin(); it != m_writerState.cameraOutputs.end(); ++it)
        {
            const auto& output = it.value();
            output->writerThread = std::thread([this, output, generation]()
            {
                writerLoop(output, generation);
            });
        }
        m_writerStatusTimer.start();
        setWriterStatus(RecordingWriterPhase::Writing);
        return true;
    }

    // Takes a stable snapshot of active camera outputs
    QList<std::shared_ptr<RecordingManager::CameraOutput>>
    RecordingManager::writerOutputsSnapshot() const
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        return m_writerState.cameraOutputs.values();
    }

    // Requests all writer threads to stop after queued work
    void RecordingManager::requestWriterStop()
    {
        for (const auto& output : writerOutputsSnapshot())
        {
            {
                std::lock_guard<std::mutex> lock(output->queueMutex);
                output->stopRequested = true;
            }
            output->writeCondition.notify_all();
        }
    }

    // Completes finalization after every writer has drained and closed
    void RecordingManager::checkWriterFinalization()
    {
        if (!m_writerFinalizationCompletion)
        {
            return;
        }

        for (const auto& output : writerOutputsSnapshot())
        {
            if (!output->writerFinished.load(std::memory_order_acquire))
            {
                return;
            }
        }

        stopStreamingOutputs(true);
    }

    // Stops writer threads and closes all output files
    void RecordingManager::stopStreamingOutputs(bool applyOutputManifest)
    {
        auto completion = std::move(m_writerFinalizationCompletion);
        const auto outputs = writerOutputsSnapshot();
        requestWriterStop();

        for (const auto& output : outputs)
        {
            if (output->writerThread.joinable())
            {
                output->writerThread.join();
            }
        }
        const bool writerFailed = !writerErrorSnapshot().isEmpty();
        if (applyOutputManifest && !writerFailed)
        {
            m_activeSession->clearOutputFiles();
            for (const auto& output : outputs)
            {
                if (output->framesWritten <= 0)
                {
                    continue;
                }
                m_activeSession->setOutputFilePaths(output->cameraId,
                                                     output->rawPath,
                                                     output->frameInfoPath);
                m_activeSession->setOutputFramesWritten(output->cameraId,
                                                        output->framesWritten);
            }
        }
        if (writerFailed)
        {
            for (const auto& output : outputs)
            {
                if (!output->rawPath.isEmpty())
                {
                    m_incompleteOutputPaths.append(output->rawPath);
                }
                if (!output->frameInfoPath.isEmpty())
                {
                    m_incompleteOutputPaths.append(output->frameInfoPath);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.cameraOutputs.clear();
            m_writerState.pendingWriteBytes = 0;
        }
        m_writerStatusTimer.stop();
        emitWriterStatus();
        if (completion)
        {
            completion();
        }
    }

    // Drains queued frames for one camera writer thread
    void RecordingManager::writerLoop(const std::shared_ptr<CameraOutput>& output, quint64 generation)
    {
        while (true)
        {
            WriteTask task;
            {
                std::unique_lock<std::mutex> lock(output->queueMutex);
                output->writeCondition.wait(lock, [&output]()
                {
                    return output->stopRequested || !output->writeQueue.empty();
                });
                if (output->writeQueue.empty())
                {
                    if (output->stopRequested)
                    {
                        break;
                    }
                    continue;
                }
                task = std::move(output->writeQueue.front());
                output->writeQueue.pop_front();
            }

            QString errorMessage;
            if (!writeTask(*output, task, errorMessage))
            {
                requestWriterStop();
                qint64 droppedFrames = 1;
                {
                    std::lock_guard<std::mutex> lock(output->queueMutex);
                    droppedFrames += static_cast<qint64>(output->writeQueue.size());
                }
                QString writerFailure;
                {
                    std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
                    if (m_writerState.writerError.isEmpty())
                    {
                        m_writerState.writerError = errorMessage.isEmpty()
                                                        ? QStringLiteral("Unknown recording writer error")
                                                        : errorMessage;
                    }
                    m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
                    m_writerState.status.addDroppedFrames(droppedFrames);
                    writerFailure = m_writerState.writerError;
                }
                markWriterStatusDirty();
                QMetaObject::invokeMethod(this, [this, writerFailure, generation]()
                {
                    if (generation == m_captureState.generation && m_captureState.isRecording)
                    {
                        finishRecording(ExperimentRunState::Failed, writerFailure);
                    }
                }, Qt::QueuedConnection);
                break;
            }

            {
                std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
                const qint64 frameBytes = task.frame.payloadByteCount();
                m_writerState.pendingWriteBytes -= static_cast<size_t>(frameBytes);
                m_writerState.status.addWrittenFrames(1);
                m_writerState.status.addWrittenBytes(frameBytes);
            }
            output->framesWritten += 1;
            markWriterStatusDirty();
        }

        if (output->backend)
        {
            if (!output->backend->stopStack())
            {
                std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
                if (m_writerState.writerError.isEmpty())
                {
                    m_writerState.writerError =
                        QStringLiteral("Failed to finalize output for %1: %2")
                            .arg(output->cameraId)
                            .arg(output->backend->lastError());
                }
            }
            output->backend.reset();
        }
        output->writerFinished.store(true, std::memory_order_release);
        QMetaObject::invokeMethod(this,
                                  [this]() { checkWriterFinalization(); },
                                  Qt::QueuedConnection);
    }

    // Writes one queued frame to its camera output
    bool RecordingManager::writeTask(CameraOutput& output, const WriteTask& task, QString& errorMessage)
    {
        SaveBackend* backend = output.backend.get();
        if (!backend)
        {
            SaveBackend::TiffOptions tiffOpts;
            tiffOpts.useDeflate = m_captureState.enableCompression;
            tiffOpts.zipQuality = m_captureState.compressionLevel;

            auto newBackend = std::make_unique<SaveBackend>();
            if (!newBackend->startStackRaw(output.rawPath,
                                           output.frameInfoPath,
                                           output.metadataFileName,
                                           m_captureState.format,
                                           task.frame.width,
                                           task.frame.height,
                                           task.frame.pixelFormat,
                                           task.frame.bitsPerSample,
                                           m_mdaState.plan,
                                           physicalPixelSizeUm(task.frame.width,
                                                               task.frame.sourceRoiWidth,
                                                               output.pixelSizeUm),
                                           physicalPixelSizeUm(task.frame.height,
                                                               task.frame.sourceRoiHeight,
                                                               output.pixelSizeUm),
                                           output.acquisitionStartTimestampNs,
                                           output.cameraId,
                                           output.cameraProperties,
                                           tiffOpts))
            {
                errorMessage = QStringLiteral("Failed to open raw output for %1: %2")
                               .arg(output.cameraId)
                               .arg(newBackend->lastError());
                return false;
            }
            output.backend = std::move(newBackend);
            output.width = task.frame.width;
            output.height = task.frame.height;
            output.bits = task.frame.bitsPerSample;
            output.pixelFormat = task.frame.pixelFormat;
            backend = output.backend.get();
        }
        else if (task.frame.width != output.width
            || task.frame.height != output.height
            || task.frame.bitsPerSample != output.bits
            || task.frame.pixelFormat != output.pixelFormat)
        {
            errorMessage = QStringLiteral("Frame format changed during recording for %1").arg(output.cameraId);
            return false;
        }

        const FramePayloadView payload = framePayloadForWrite(task.frame, m_captureState.format);
        if (!payload.data() || payload.byteCount <= 0)
        {
            errorMessage = QStringLiteral("Invalid frame payload for %1").arg(output.cameraId);
            return false;
        }

        if (!backend->appendRaw(payload.data(),
                                payload.byteCount,
                                task.frame,
                                task.hasEvent ? &task.event : nullptr))
        {
            errorMessage = QStringLiteral("Failed writing raw frame for %1: %2")
                           .arg(output.cameraId)
                           .arg(backend->lastError());
            return false;
        }

        return true;
    }

    // Starts a recording session using the requested plan
    bool RecordingManager::start(const ExperimentPlan& requestedPlan,
                                 const QStringList& activeCameraIds,
                                 const QJsonObject& deviceProperties,
                                 const QHash<QString, double>& cameraPixelSizesUm)
    {
        if (m_captureState.isRecording || isFinalizing())
        {
            qWarning().noquote() << (m_captureState.isRecording
                                         ? QStringLiteral("Recording already running")
                                         : QStringLiteral("Previous recording is still being finalized"));
            return false;
        }

        ExperimentPlan plan;
        QString errorMessage;
        if (!buildCapturePlan(requestedPlan, activeCameraIds, plan, errorMessage))
        {
            qWarning().noquote() << errorMessage;
            return false;
        }

        const bool usesMda = planUsesMda(plan);
        const bool streamsMda = planStreamsMda(plan);
        if (m_mdaState.manager && m_mdaState.manager->isRunning())
        {
            qWarning().noquote() << "Previous MDA acquisition is still running";
            return false;
        }
        if (plan.streamToDisk)
        {
            SessionOutputInfo outputInfo;
            if (!reserveUniqueSessionOutput(plan, outputInfo, errorMessage))
            {
                qWarning().noquote() << errorMessage;
                return false;
            }
        }
        ++m_captureState.generation;
        resetCaptureState(plan);
        if (!usesMda)
        {
            primeLastFrameIndices();
            if (m_cameraRuntimeControl
                && !m_cameraRuntimeControl->setRecordingFrameDeliveryEnabled(true))
            {
                if (plan.streamToDisk)
                {
                    QDir(sessionOutputDir(plan.saveDir, plan.baseName)).removeRecursively();
                }
                qWarning().noquote() << "Failed to enable all-frame camera delivery";
                return false;
            }
        }
        resetSessionState(plan, deviceProperties, cameraPixelSizesUm);

        if (plan.streamToDisk)
        {
            if (!startStreamingOutputs(plan))
            {
                if (m_cameraRuntimeControl)
                {
                    m_cameraRuntimeControl->setRecordingFrameDeliveryEnabled(false);
                }
                const QString writerError = writerErrorSnapshot();
                qWarning().noquote() << (writerError.isEmpty()
                                             ? QStringLiteral("Failed to start streaming outputs")
                                             : writerError);
                finalizeActiveSession(ExperimentRunState::Failed, writerError);
                m_activeSession.reset();
                return false;
            }
        }

        m_captureState.elapsedTimer.start();
        m_captureState.isRecording = true;
        m_mdaState.usingMda = usesMda;
        m_mdaState.streamMda = streamsMda;
        m_mdaState.streamIntervalMs = streamsMda ? plan.mdaIntervalMs : 0.0;
        m_mdaState.lastStreamCaptureMs = streamsMda ? static_cast<qint64>(-m_mdaState.streamIntervalMs) : 0;
        m_captureState.phase = usesMda
                                   ? kRecordingPhaseRecordingMda
                                   : (m_captureState.burstMode
                                          ? kRecordingPhaseRecordingBurst
                                          : kRecordingPhaseRecording);
        emit recordingStateChanged(true);
        emitProgress(true);

        qInfo().noquote() << QString("Recording started (%1 camera(s))").arg(m_captureState.activeCameraIds.size());

        if (usesMda)
        {
            QString mdaError;
            if (!startMdaCapture(&mdaError))
            {
                finishRecording(ExperimentRunState::Failed,
                                mdaError.isEmpty()
                                    ? QStringLiteral("Failed to start MDA capture")
                                    : mdaError);
                return false;
            }
        }
        return true;
    }

    // Stops the active recording session and emits the finished session
    void RecordingManager::stop()
    {
        finishRecording(ExperimentRunState::Canceled);
    }

    // Finalizes one active recording with an explicit terminal state
    void RecordingManager::finishRecording(ExperimentRunState state, const QString& errorMessage)
    {
        if (!m_captureState.isRecording) return;

        const bool streamToDisk = m_captureState.streamToDisk;
        auto session = m_activeSession;
        m_completionPending = true;
        if (streamToDisk)
        {
            setWriterStatus(RecordingWriterPhase::Stopping);
        }
        m_captureState.isRecording = false;
        m_captureState.phase = kRecordingPhaseStopped;
        emit recordingStateChanged(false);
        emitProgress(true);

        if (m_cameraRuntimeControl)
        {
            m_cameraRuntimeControl->setRecordingFrameDeliveryEnabled(false);
        }
        if (m_mdaState.usingMda && m_mdaState.manager && m_mdaState.manager->isRunning())
        {
            m_mdaState.manager->requestCancel();
        }
        if (m_cameraRuntimeControl)
        {
            m_cameraRuntimeControl->setFrameDeliveryPaused(false);
        }

        qInfo().noquote() << "Recording stopped";

        if (streamToDisk)
        {
            m_writerFinalizationCompletion = [this, state, errorMessage, session]()
                {
                    completeRecording(state, errorMessage, session);
                };
            requestWriterStop();
            checkWriterFinalization();
            return;
        }
        completeRecording(state, errorMessage, session);
    }

    // Finalizes metadata and failed output cleanup without blocking the manager thread
    void RecordingManager::completeRecording(
        ExperimentRunState state,
        const QString& errorMessage,
        const std::shared_ptr<RecordingSessionData>& session)
    {
        finalizeActiveSession(state, errorMessage);
        if (m_sessionPreparationCallback)
        {
            m_sessionPreparationCallback(*session);
        }
        if (!m_captureState.streamToDisk)
        {
            publishRecordingStopped(session);
            return;
        }

        const QString payloadError = writerErrorSnapshot();
        const QStringList incompleteOutputPaths = std::exchange(m_incompleteOutputPaths, {});
        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this,
                [this, watcher, session]()
                {
                    const QString finalizationError = watcher->result();
                    if (finalizationError.isEmpty())
                    {
                        setWriterStatus(RecordingWriterPhase::Completed);
                    }
                    else
                    {
                        setWriterError(finalizationError);
                        setWriterStatus(RecordingWriterPhase::Failed, finalizationError);
                        updateSessionResult(*session,
                                            QStringLiteral("Error: %1").arg(finalizationError),
                                            false);
                    }
                    publishRecordingStopped(session);
                    watcher->deleteLater();
                });
        watcher->setFuture(QtConcurrent::run(
            &m_finalizationThreadPool,
            [session, payloadError, incompleteOutputPaths]()
            {
                QString finalizationError = payloadError;
                if (finalizationError.isEmpty())
                {
                    writeSessionDocument(*session, finalizationError);
                }

                for (const QString& path : incompleteOutputPaths)
                {
                    discardIncompleteOutput(path);
                    QDir().rmdir(QFileInfo(path).absolutePath());
                }
                return finalizationError;
            }));
    }

    // Resets recording state and publishes one completed session
    void RecordingManager::publishRecordingStopped(
        const std::shared_ptr<RecordingSessionData>& session)
    {
        m_activeSession.reset();
        MDAManager* const manager = m_mdaState.manager;
        m_mdaState = MdaState{};
        m_mdaState.manager = manager;
        m_completionPending = false;
        emit recordingStopped(session);
    }

    // Receives one complete preview frame batch for recording ingestion
    void RecordingManager::onRawFramesReady(const QList<ImageFrame>& frames)
    {
        for (const ImageFrame& frame : frames)
        {
            if (!m_captureState.isRecording)
            {
                break;
            }
            ingestFrame(FramePacket{frame, FramePacket::Source::PreviewStream});
        }
    }

    // Fails an active preview recording when frame delivery is incomplete
    void RecordingManager::onFrameDeliveryFailed(const QString& errorMessage, quint64 droppedFrames)
    {
        if (!m_captureState.isRecording || m_mdaState.usingMda)
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.status.addDroppedFrames(static_cast<qint64>(droppedFrames));
        }
        finishRecording(ExperimentRunState::Failed, errorMessage);
    }

    // Seeds last frame indices to skip stale preview frames
    void RecordingManager::primeLastFrameIndices()
    {
        m_captureState.lastFrameIndex.clear();
        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            ImageFrame frame;
            const bool ok = m_latestFrameFetcher && m_latestFrameFetcher(cameraId, frame);
            if (ok)
            {
                m_captureState.lastFrameIndex[cameraId] = frame.frameIndex;
            }
            else
            {
                m_captureState.lastFrameIndex[cameraId] = 0;
            }
        }
    }

    // Emits recording progress for UI and API listeners
    void RecordingManager::emitProgress(bool force)
    {
        constexpr qint64 kProgressPublishIntervalMs = 100;
        if (!force
            && m_progressPublishTimer.isValid()
            && m_progressPublishTimer.elapsed() < kProgressPublishIntervalMs)
        {
            return;
        }
        m_progressPublishTimer.restart();

        qint64 frameCurrent = 0;
        const int burstCurrent = m_captureState.burstMode ? m_captureState.currentBurst : 0;
        const int burstTarget = m_captureState.burstMode ? m_captureState.targetBursts : 0;

        if (!m_captureState.activeCameraIds.isEmpty())
        {
            qint64 minFrames = (std::numeric_limits<qint64>::max)();
            for (const QString& cameraId : m_captureState.activeCameraIds)
            {
                const qint64 count = m_captureState.framesCapturedThisBurst.value(cameraId, 0);
                minFrames = (std::min)(minFrames, count);
            }
            frameCurrent = minFrames;
        }

        const int tCount = (m_mdaState.plan.framesPerBurst > 0) ? m_mdaState.plan.framesPerBurst : 1;
        const int zCount = m_mdaState.plan.zPositions.empty()
                               ? 1
                               : static_cast<int>(m_mdaState.plan.zPositions.size());
        const int xyCount = m_mdaState.plan.positions.empty()
                                ? 1
                                : static_cast<int>(m_mdaState.plan.positions.size());
        const qint64 frameTarget = m_mdaState.usingMda
                                       ? static_cast<qint64>(tCount) * zCount * xyCount
                                       : m_captureState.framesPerBurst;

        int mdaTimeIndex = 0;
        int mdaZIndex = 0;
        int mdaPositionIndex = 0;
        bool hasXY = false;
        double x = 0.0;
        double y = 0.0;
        bool hasZ = false;
        double z = 0.0;
        if (m_mdaState.usingMda && m_mdaState.hasLastEvent)
        {
            mdaTimeIndex = m_mdaState.lastEvent.timeIndex + 1;
            mdaZIndex = m_mdaState.lastEvent.zIndex + 1;
            mdaPositionIndex = m_mdaState.lastEvent.positionIndex + 1;
            hasXY = m_mdaState.lastEvent.hasXY;
            x = m_mdaState.lastEvent.x;
            y = m_mdaState.lastEvent.y;
            hasZ = m_mdaState.lastEvent.hasZ;
            z = m_mdaState.lastEvent.z;
        }

        qint64 waitRemainingMs = 0;
        if (m_captureState.waitingBetweenBursts)
        {
            const qint64 waitedMs = m_captureState.elapsedTimer.elapsed() - m_captureState.lastBurstEndMs;
            const qint64 burstIntervalMs = static_cast<qint64>(m_captureState.burstIntervalMs);
            waitRemainingMs = (std::max)(0ll, burstIntervalMs - waitedMs);
        }

        emit progressChanged(m_captureState.phase,
                             frameCurrent,
                             frameTarget,
                             burstCurrent,
                             burstTarget,
                             waitRemainingMs,
                             mdaTimeIndex,
                             tCount,
                             mdaZIndex,
                             zCount,
                             mdaPositionIndex,
                             xyCount,
                             hasXY,
                             x,
                             y,
                             hasZ,
                             z);
    }

    // Adds one captured frame to the asynchronous writer queue
    bool RecordingManager::enqueueFrame(const ImageFrame& frame,
                                        const AcquisitionEvent* event)
    {
        const QString cameraId = frame.cameraId.trimmed();
        const size_t frameBytes = static_cast<size_t>(frame.payloadByteCount());
        std::shared_ptr<CameraOutput> output;
        QString failureError;
        bool emitStatus = false;
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            const auto it = m_writerState.cameraOutputs.constFind(cameraId);
            if (it == m_writerState.cameraOutputs.constEnd() || !it.value())
            {
                if (m_writerState.writerError.isEmpty())
                {
                    m_writerState.writerError = QStringLiteral("Missing output for %1").arg(cameraId);
                }
                m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
                m_writerState.status.addDroppedFrames(1);
                failureError = m_writerState.writerError;
                emitStatus = true;
            }
            else if (m_writerState.pendingWriteBytes + frameBytes > m_writerState.recordedMaxBytes)
            {
                if (m_writerState.writerError.isEmpty())
                {
                    m_writerState.writerError = QStringLiteral("Recording write queue exceeded limit");
                }
                m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
                m_writerState.status.addDroppedFrames(1);
                failureError = m_writerState.writerError;
                emitStatus = true;
            }
            else
            {
                output = it.value();
                m_writerState.pendingWriteBytes += frameBytes;
            }
        }
        if (!output)
        {
            if (emitStatus)
            {
                qWarning().noquote() << "Recording write queue full or output missing, stopping capture";
                const quint64 generation = m_captureState.generation;
                QMetaObject::invokeMethod(this, [this, failureError, generation]()
                {
                    if (generation == m_captureState.generation && m_captureState.isRecording)
                    {
                        finishRecording(ExperimentRunState::Failed, failureError);
                    }
                }, Qt::QueuedConnection);
                emitWriterStatus();
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(output->queueMutex);
            if (output->stopRequested)
            {
                {
                    std::lock_guard<std::mutex> stateLock(m_writerState.writeMutex);
                    m_writerState.pendingWriteBytes -= frameBytes;
                }
                markWriterStatusDirty();
                return false;
            }
            WriteTask task;
            task.frame = frame;
            if (event)
            {
                task.event = *event;
                task.hasEvent = true;
            }
            output->writeQueue.push_back(std::move(task));
        }
        output->writeCondition.notify_one();
        markWriterStatusDirty();
        return true;
    }

    // Decides whether a frame belongs to the active recording window
    bool RecordingManager::shouldAcceptFrame(const FramePacket& packet) const
    {
        if (!m_captureState.isRecording) return false;
        if (!packet.frame.isValid()) return false;
        const QString cameraId = packet.frame.cameraId.trimmed();
        if (cameraId.isEmpty()) return false;
        if (!m_captureState.activeCameraIds.contains(cameraId)) return false;
        if (m_mdaState.usingMda && packet.source != FramePacket::Source::Mda) return false;
        return packet.frame.frameIndex > m_captureState.lastFrameIndex.value(cameraId, 0);
    }

    // Applies burst timing and records one accepted frame
    void RecordingManager::ingestFrame(const FramePacket& packet)
    {
        if (!shouldAcceptFrame(packet)) return;
        ImageFrame frame = packet.frame;
        frame.cameraId = frame.cameraId.trimmed();
        if (frame.timestampNs == 0)
        {
            frame.timestampNs = currentTimestampNs();
        }
        const QString& cameraId = frame.cameraId;

        if (m_captureState.waitingBetweenBursts)
        {
            const qint64 waited = m_captureState.elapsedTimer.elapsed() - m_captureState.lastBurstEndMs;
            if (waited < static_cast<qint64>(m_captureState.burstIntervalMs))
            {
                emitProgress();
                return;
            }
            m_captureState.waitingBetweenBursts = false;
            m_captureState.phase = m_mdaState.usingMda
                                       ? kRecordingPhaseRecordingMda
                                       : (m_captureState.burstMode
                                              ? kRecordingPhaseRecordingBurst
                                              : kRecordingPhaseRecording);
            if (m_mdaState.streamMda && m_mdaState.streamIntervalMs > 0.0)
            {
                m_mdaState.lastStreamCaptureMs = m_captureState.elapsedTimer.elapsed() - static_cast<qint64>(m_mdaState.
                    streamIntervalMs);
            }
        }

        if (m_mdaState.streamMda && m_mdaState.streamIntervalMs > 0.0)
        {
            const qint64 now = m_captureState.elapsedTimer.elapsed();
            const qint64 intervalMs = static_cast<qint64>(m_mdaState.streamIntervalMs);
            if (intervalMs > 0 && (now - m_mdaState.lastStreamCaptureMs) < intervalMs)
            {
                return;
            }
            m_mdaState.lastStreamCaptureMs = now;
        }

        if (!m_mdaState.usingMda
            && m_captureState.framesCapturedThisBurst.value(cameraId, 0)
                >= m_captureState.framesPerBurst)
        {
            return;
        }

        const QString writerError = m_captureState.streamToDisk
                                        ? writerErrorSnapshot()
                                        : QString();
        if (!writerError.isEmpty())
        {
            finishRecording(ExperimentRunState::Failed, writerError);
            return;
        }

        AcquisitionEvent previewEvent;
        if (!m_mdaState.usingMda)
        {
            previewEvent.sequenceIndex = static_cast<quint64>(
                m_activeSession->experimentDocument().events.size());
            previewEvent.burstIndex = m_captureState.currentBurst;
            previewEvent.timeIndex = static_cast<int>(
                m_captureState.framesCapturedThisBurst.value(frame.cameraId, 0));
            previewEvent.exposureMs = m_mdaState.plan.exposureMs;
            if (previewEvent.exposureMs <= 0.0)
            {
                double exposureMs = 0.0;
                const QJsonObject cameraProperties = m_activeSession->experimentDocument()
                                                         .deviceProperties.value(frame.cameraId).toObject();
                if (readFiniteNumber(cameraProperties.value(QStringLiteral("Exposure")), exposureMs)
                    && exposureMs > 0.0)
                {
                    previewEvent.exposureMs = exposureMs;
                }
            }
            previewEvent.cameraIds = QStringList{frame.cameraId};
        }
        const AcquisitionEvent* writeEvent = packet.hasEvent
                                                 ? &packet.event
                                                 : (!m_mdaState.usingMda ? &previewEvent : nullptr);

        if (m_captureState.streamToDisk)
        {
            if (!enqueueFrame(frame, writeEvent))
            {
                return;
            }
        }
        else
        {
            m_activeSession->appendImageFrame(frame);
        }
        m_captureState.lastFrameIndex[cameraId] = frame.frameIndex;

        if (!m_mdaState.usingMda)
        {
            appendPreviewEventRecord(frame, previewEvent);
        }

        m_captureState.framesCapturedThisBurst[cameraId] += 1;
        m_captureState.framesCapturedTotal[cameraId] += 1;
        emitProgress();

        advanceBurstStateIfNeeded();
    }

    // Records one accepted preview frame as an actual acquisition event
    void RecordingManager::appendPreviewEventRecord(const ImageFrame& frame,
                                                     const AcquisitionEvent& event)
    {
        AcquisitionEventRecord record;
        record.event = event;
        record.startedTimestampNs = frame.timestampNs;
        record.completedTimestampNs = record.startedTimestampNs;
        record.succeeded = true;
        record.frames.insert(frame.cameraId, frameRecordFromImageFrame(frame));
        m_activeSession->appendEventRecord(record);
    }

    // Converts one completed MDA event into metadata and recording frame packets
    void RecordingManager::handleMdaOutput(const MDAOutput& output)
    {
        if (!m_captureState.isRecording || !m_mdaState.usingMda) return;

        m_mdaState.lastEvent = output.event;
        m_mdaState.hasLastEvent = true;

        AcquisitionEventRecord record;
        record.event = output.event;
        record.startedTimestampNs = output.startedTimestampNs;
        record.completedTimestampNs = output.completedTimestampNs;
        record.succeeded = output.succeeded;
        record.errorMessage = output.errorMessage;
        if (!output.succeeded)
        {
            for (auto it = output.frames.constBegin(); it != output.frames.constEnd(); ++it)
            {
                ImageFrame frame = it.value();
                QString cameraId = it.key().trimmed();
                if (cameraId.isEmpty()) cameraId = frame.cameraId.trimmed();
                if (cameraId.isEmpty() || !record.event.cameraIds.contains(cameraId) || !frame.isValid())
                {
                    continue;
                }
                frame.cameraId = cameraId;
                if (frame.timestampNs == 0)
                {
                    frame.timestampNs = output.completedTimestampNs != 0
                                            ? output.completedTimestampNs
                                            : currentTimestampNs();
                }
                record.frames.insert(cameraId, frameRecordFromImageFrame(frame));
            }
            m_activeSession->appendEventRecord(record);
            emitProgress(true);
            return;
        }

        const auto dispatchMdaFrame = [this, &output](ImageFrame rawFrame)
        {
            rawFrame.cameraId = rawFrame.cameraId.trimmed();
            if (rawFrame.cameraId.isEmpty())
            {
                rawFrame.cameraId = m_mdaState.cameraId;
            }
            if (rawFrame.timestampNs == 0)
            {
                rawFrame.timestampNs = output.completedTimestampNs != 0
                                           ? output.completedTimestampNs
                                           : currentTimestampNs();
            }
            if (rawFrame.frameIndex <= m_captureState.lastFrameIndex.value(rawFrame.cameraId, 0))
            {
                rawFrame.frameIndex = m_captureState.lastFrameIndex.value(rawFrame.cameraId, 0) + 1;
            }
            ingestFrame(FramePacket{rawFrame, FramePacket::Source::Mda, output.event, true});
            if (rawFrame.isValid())
            {
                emit mdaRawFrameReady(rawFrame);
            }
            return rawFrame;
        };

        if (output.frames.isEmpty())
        {
            m_activeSession->appendEventRecord(record);
            return;
        }

        for (auto it = output.frames.constBegin(); it != output.frames.constEnd(); ++it)
        {
            QString cameraId = it.key().trimmed();
            ImageFrame rawFrame = it.value();
            if (cameraId.isEmpty())
            {
                cameraId = rawFrame.cameraId.trimmed();
            }
            if (cameraId.isEmpty())
            {
                cameraId = m_mdaState.cameraId;
            }
            if (!m_captureState.activeCameraIds.contains(cameraId))
            {
                continue;
            }
            rawFrame.cameraId = cameraId;
            if (m_captureState.activeCameraIds.size() == 1)
            {
                rawFrame.frameIndex = ++m_mdaState.frameIndex;
            }
            const ImageFrame normalizedFrame = dispatchMdaFrame(rawFrame);
            record.frames.insert(cameraId, frameRecordFromImageFrame(normalizedFrame));
        }
        m_activeSession->appendEventRecord(record);
    }

    // Starts MDA driven recording capture
    bool RecordingManager::startMdaCapture(QString* errorMessage)
    {
        if (!m_mmcore)
        {
            const QString message = QStringLiteral("MMCore not available for MDA");
            if (errorMessage) *errorMessage = message;
            qWarning().noquote() << message;
            return false;
        }
        if (m_captureState.activeCameraIds.isEmpty())
        {
            const QString message = QStringLiteral("MDA requires at least one camera");
            if (errorMessage) *errorMessage = message;
            qWarning().noquote() << message;
            return false;
        }
        if (m_captureState.activeCameraIds.size() > 1 && !m_cameraProvider)
        {
            const QString message = QStringLiteral("Multi-camera MDA requires a camera provider");
            if (errorMessage) *errorMessage = message;
            qWarning().noquote() << message;
            return false;
        }

        if (!m_mdaState.manager)
        {
            m_mdaState.manager = new MDAManager(m_mmcore, this);
        }
        if (m_mdaState.manager->isRunning())
        {
            const QString message = QStringLiteral("Previous MDA acquisition is still running");
            if (errorMessage) *errorMessage = message;
            qWarning().noquote() << message;
            return false;
        }
        m_mdaState.manager->setCameraProvider(m_cameraProvider);

        if (m_captureState.activeCameraIds.size() > 1 && m_cameraRuntimeControl)
        {
            m_cameraRuntimeControl->setFrameDeliveryPaused(true);
        }

        m_mdaState.cameraId = m_captureState.activeCameraIds.first();
        m_mdaState.frameIndex = 0;
        m_mdaState.usingMda = true;
        m_captureState.lastFrameIndex.clear();
        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            m_captureState.lastFrameIndex[cameraId] = 0;
        }

        m_mdaState.burstsRemaining = m_captureState.burstMode ? m_captureState.targetBursts : 1;
        const quint64 generation = m_captureState.generation;
        disconnect(m_mdaState.manager, nullptr, this, nullptr);
        connect(m_mdaState.manager, &MDAManager::eventFinished, this,
                [this, generation](const MDAOutput& output)
        {
            if (generation != m_captureState.generation
                || !m_mdaState.usingMda
                || !m_captureState.isRecording)
            {
                return;
            }
            handleMdaOutput(output);
        });
        connect(m_mdaState.manager, &MDAManager::sequenceFinished, this, [this, generation]()
        {
            if (generation != m_captureState.generation
                || !m_mdaState.usingMda
                || !m_captureState.isRecording)
            {
                return;
            }
            m_mdaState.burstsRemaining -= 1;
            if (m_mdaState.burstsRemaining > 0)
            {
                m_captureState.waitingBetweenBursts = true;
                m_captureState.lastBurstEndMs = m_captureState.elapsedTimer.elapsed();
                m_captureState.phase = kRecordingPhaseWaitingNextBurst;
                emitProgress(true);
                const int waitMs = static_cast<int>(m_captureState.burstIntervalMs);
                QTimer::singleShot(waitMs, this, [this, generation]()
                {
                    if (generation != m_captureState.generation
                        || !m_mdaState.usingMda
                        || !m_captureState.isRecording)
                    {
                        return;
                    }
                    QString startError;
                    if (!startMdaRun(&startError))
                    {
                        finishRecording(ExperimentRunState::Failed,
                                        startError.isEmpty()
                                            ? QStringLiteral("Failed to start MDA capture")
                                            : startError);
                    }
                });
            }
            else
            {
                finishRecording(ExperimentRunState::Completed);
            }
        });
        connect(m_mdaState.manager, &MDAManager::sequenceCanceled, this, [this, generation]()
        {
            if (generation != m_captureState.generation
                || !m_mdaState.usingMda
                || !m_captureState.isRecording)
            {
                return;
            }
            finishRecording(ExperimentRunState::Canceled);
        });
        connect(m_mdaState.manager, &MDAManager::sequenceError, this,
                [this, generation](const QString& message)
        {
            if (generation != m_captureState.generation
                || !m_mdaState.usingMda
                || !m_captureState.isRecording)
            {
                return;
            }
            qWarning().noquote() << QString("MDA error: %1").arg(message);
            finishRecording(ExperimentRunState::Failed, message);
        });

        return startMdaRun(errorMessage);
    }

    // Starts the next MDA burst run
    bool RecordingManager::startMdaRun(QString* errorMessage)
    {
        if (!m_captureState.isRecording || !m_mdaState.usingMda)
        {
            if (errorMessage) *errorMessage = QStringLiteral("Recording is not active");
            return false;
        }
        if (m_mdaState.burstsRemaining <= 0)
        {
            finishRecording(ExperimentRunState::Completed);
            return true;
        }
        if (!m_mdaState.manager)
        {
            if (errorMessage) *errorMessage = QStringLiteral("MDA manager is not available");
            return false;
        }
        if (m_mdaState.manager->isRunning())
        {
            if (errorMessage) *errorMessage = QStringLiteral("MDA acquisition is already running");
            return false;
        }

        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            m_captureState.framesCapturedThisBurst[cameraId] = 0;
        }

        m_captureState.waitingBetweenBursts = false;
        const int burstIndex = m_captureState.currentBurst;
        m_captureState.currentBurst += 1;
        m_captureState.phase = kRecordingPhaseRecordingMda;
        emitProgress(true);

        QString buildError;
        const QList<AcquisitionEvent> events = buildAcquisitionEvents(m_mdaState.plan,
                                                                      burstIndex,
                                                                      &buildError);
        if (events.isEmpty())
        {
            if (errorMessage)
            {
                *errorMessage = buildError.isEmpty()
                                    ? QStringLiteral("MDA event list is empty")
                                    : buildError;
            }
            return false;
        }
        if (!m_mdaState.manager->start(events, false))
        {
            if (errorMessage && errorMessage->isEmpty())
            {
                *errorMessage = QStringLiteral("Failed to start MDA acquisition");
            }
            return false;
        }
        return true;
    }

    // Checks whether all cameras reached the current frame target
    bool RecordingManager::allCamerasReachedTarget() const
    {
        return std::all_of(m_captureState.activeCameraIds.cbegin(),
                           m_captureState.activeCameraIds.cend(),
                           [this](const QString& cameraId)
                           {
                               return m_captureState.framesCapturedThisBurst.value(cameraId, 0)
                                   >= m_captureState.framesPerBurst;
                           });
    }

    // Advances burst state after an accepted frame
    void RecordingManager::advanceBurstStateIfNeeded()
    {
        if (m_mdaState.usingMda)
        {
            return;
        }
        if (!m_captureState.burstMode)
        {
            if (allCamerasReachedTarget())
            {
                finishRecording(ExperimentRunState::Completed);
            }
            return;
        }

        if (!allCamerasReachedTarget())
        {
            return;
        }

        m_captureState.currentBurst += 1;
        m_captureState.phase = kRecordingPhaseWaitingNextBurst;
        emitProgress(true);

        if (m_captureState.currentBurst >= m_captureState.targetBursts)
        {
            finishRecording(ExperimentRunState::Completed);
            return;
        }

        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            m_captureState.framesCapturedThisBurst[cameraId] = 0;
        }

        m_captureState.waitingBetweenBursts = true;
        m_captureState.lastBurstEndMs = m_captureState.elapsedTimer.elapsed();
    }

    // Saves buffered sessions and preserves direct writer outputs
    QString RecordingManager::saveSessionToDisk(const std::shared_ptr<RecordingSessionData>& session)
    {
        if (!session)
        {
            return QStringLiteral("Error: Missing recording session");
        }
        ExperimentPlan capturePlan = session->capturePlan();
        if (capturePlan.cameraIds.isEmpty())
        {
            return updateSessionResult(*session, QStringLiteral("Error: No cameras to save"), false);
        }
        if (capturePlan.saveDir.trimmed().isEmpty())
        {
            return updateSessionResult(*session, QStringLiteral("Error: Save directory is empty"), false);
        }
        if (capturePlan.baseName.trimmed().isEmpty())
        {
            return updateSessionResult(*session, QStringLiteral("Error: Base name is empty"), false);
        }
        QString planError;
        if (!validateExperimentPlan(capturePlan, &planError))
        {
            return updateSessionResult(*session, QStringLiteral("Error: %1").arg(planError), false);
        }

        if (!session->hasAnyFrames())
        {
            if (!session->saveMessage().isEmpty())
            {
                return session->saveMessage();
            }
            if (session->streamedToDisk() && session->hasRecordedOutput())
            {
                return updateSessionResult(
                    *session,
                    saveSuccessMessage(
                        QStringLiteral("Success: Recording was already saved during acquisition"),
                        savedSessionOutputDir(*session)),
                    true);
            }
            return updateSessionResult(*session, QStringLiteral("Error: No frames captured"), false);
        }

        if (session->runState() == ExperimentRunState::Draft
            || session->runState() == ExperimentRunState::Running)
        {
            return updateSessionResult(*session,
                                       QStringLiteral("Error: Recording session is not complete"),
                                       false);
        }

        SessionOutputInfo outputInfo;
        QString outputErrorMessage;
        if (!reserveUniqueSessionOutput(capturePlan, outputInfo, outputErrorMessage))
        {
            return updateSessionResult(*session, QStringLiteral("Error: %1").arg(outputErrorMessage), false);
        }
        session->setCapturePlan(capturePlan);

        session->prepareForSave(session->streamedToDisk());
        session->setWriterPhase(RecordingWriterPhase::Starting);
        const auto failSave = [&session, &outputInfo](const QString& errorMessage)
        {
            QDir(outputInfo.outputDir).removeRecursively();
            session->clearOutputFiles();
            session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
            return updateSessionResult(*session, QStringLiteral("Error: %1").arg(errorMessage), false);
        };
        QHash<QString, RecordingFileManifest> completedOutputs;
        for (const QString& cameraId : session->cameraIds())
        {
            const int frameCount = session->frameCount(cameraId);
            if (frameCount <= 0)
            {
                continue;
            }
            session->setWriterPhase(RecordingWriterPhase::Writing);
            SaveBackend rawSaver;
            SaveBackend::TiffOptions tiffOpts;
            tiffOpts.useDeflate = capturePlan.enableCompression;
            tiffOpts.zipQuality = capturePlan.compressionLevel;

            const ImageFrame firstImageFrame = session->imageFrameAt(cameraId, 0);
            if (!firstImageFrame.isValid())
            {
                continue;
            }
            QList<AcquisitionEvent> cameraEvents;
            if (capturePlan.format == RecordingFormat::OmeTiff
                || capturePlan.format == RecordingFormat::OmeZarr)
            {
                for (const AcquisitionEventRecord& record : session->experimentDocument().events)
                {
                    if (record.succeeded && record.frames.contains(cameraId))
                    {
                        cameraEvents.append(record.event);
                    }
                }
            }
            const CameraOutputPaths paths = buildCameraOutputPaths(outputInfo.outputDir,
                                                                   capturePlan.baseName,
                                                                   cameraId,
                                                                   capturePlan);
            if (!rawSaver.startStackRaw(paths.rawPath,
                                        paths.frameInfoPath,
                                        outputInfo.metadataFileName,
                                        capturePlan.format,
                                        firstImageFrame.width,
                                        firstImageFrame.height,
                                        firstImageFrame.pixelFormat,
                                        firstImageFrame.bitsPerSample,
                                        capturePlan,
                                        physicalPixelSizeUm(firstImageFrame.width,
                                                            firstImageFrame.sourceRoiWidth,
                                                            session->cameraPixelSizeUm(cameraId)),
                                        physicalPixelSizeUm(firstImageFrame.height,
                                                            firstImageFrame.sourceRoiHeight,
                                                            session->cameraPixelSizeUm(cameraId)),
                                        session->experimentDocument().startedTimestampNs,
                                        cameraId,
                                        session->experimentDocument().deviceProperties
                                            .value(cameraId).toObject(),
                                        tiffOpts))
            {
                const QString errorMessage = QString("Failed to start raw output for %1").arg(cameraId);
                return failSave(errorMessage);
            }
            int saved = 0;
            for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex)
            {
                const ImageFrame imageFrame = session->imageFrameAt(cameraId, frameIndex);
                if (!imageFrame.isValid())
                {
                    continue;
                }
                if (imageFrame.width != firstImageFrame.width
                    || imageFrame.height != firstImageFrame.height
                    || imageFrame.bitsPerSample != firstImageFrame.bitsPerSample
                    || imageFrame.pixelFormat != firstImageFrame.pixelFormat)
                {
                    rawSaver.stopStack();
                    const QString errorMessage = QString("Frame format changed during save for %1").arg(cameraId);
                    return failSave(errorMessage);
                }
                const FramePayloadView payload = framePayloadForWrite(imageFrame, capturePlan.format);
                if (!payload.data() || payload.byteCount <= 0)
                {
                    rawSaver.stopStack();
                    const QString errorMessage = QString("Invalid frame payload for %1").arg(cameraId);
                    return failSave(errorMessage);
                }

                const AcquisitionEvent* event = saved < cameraEvents.size()
                                                    ? &cameraEvents.at(saved)
                                                    : nullptr;
                if (!rawSaver.appendRaw(payload.data(),
                                        payload.byteCount,
                                        imageFrame,
                                        event))
                {
                    rawSaver.stopStack();
                    const QString errorMessage = QString("Failed to append raw frame %1 for %2").arg(saved).arg(
                        cameraId);
                    return failSave(errorMessage);
                }
                saved += 1;
            }
            if (!rawSaver.stopStack())
            {
                return failSave(QStringLiteral("Failed to finalize output for %1: %2")
                                    .arg(cameraId)
                                    .arg(rawSaver.lastError()));
            }
            completedOutputs.insert(cameraId,
                                    RecordingFileManifest{paths.rawPath, paths.frameInfoPath, saved});
            session->addWrittenFrames(saved);
        }
        if (completedOutputs.isEmpty())
        {
            return failSave(QStringLiteral("No valid frames to save"));
        }
        session->clearOutputFiles();
        for (auto it = completedOutputs.constBegin(); it != completedOutputs.constEnd(); ++it)
        {
            session->setOutputFilePaths(it.key(), it.value().rawPath, it.value().frameInfoPath);
            session->setOutputFramesWritten(it.key(), it.value().framesWritten);
        }
        session->setWriterPhase(RecordingWriterPhase::Completed);
        if (!writeSessionDocument(*session, outputErrorMessage))
        {
            return failSave(outputErrorMessage);
        }
        return updateSessionResult(
            *session,
            saveSuccessMessage(
                QStringLiteral("Success: Saved %1 recording").arg(recordingFormatName(capturePlan.format)),
                outputInfo.outputDir),
            true);
    }
} // namespace scopeone::core::internal
