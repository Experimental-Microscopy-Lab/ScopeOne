#include "internal/RecordingManager.h"

#include "internal/MultiProcessCameraManager.h"
#include "MMCore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <tiffio.h>

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
            case scopeone::core::RecordingFormat::Tiff:
                return QStringLiteral("TIFF");
            case scopeone::core::RecordingFormat::Binary:
                return QStringLiteral("Binary");
            }
            return QStringLiteral("Unknown");
        }

        QString pixelFormatName(ImagePixelFormat pixelFormat)
        {
            switch (pixelFormat)
            {
            case ImagePixelFormat::Mono8:
                return QStringLiteral("Mono8");
            case ImagePixelFormat::Mono16:
                return QStringLiteral("Mono16");
            default:
                return QStringLiteral("Unknown");
            }
        }

        quint32 pixelFormatId(ImagePixelFormat pixelFormat)
        {
            switch (pixelFormat)
            {
            case ImagePixelFormat::Mono8:
                return 0;
            case ImagePixelFormat::Mono16:
                return 1;
            default:
                return 0;
            }
        }

        QString updateSessionResult(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
                                    const QString& result,
                                    bool saved)
        {
            if (session)
            {
                session->setSaveResult(saved, result);
            }
            return result;
        }

        quint64 timestampNsForStorage(const ImageFrame& frame)
        {
            if (frame.timestampNs != 0)
            {
                return frame.timestampNs;
            }
            return static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()) * 1000000ull;
        }

        QByteArray buildImageDescriptionJson(const QString& metadataFileName, const ImageFrame& frame)
        {
            QJsonObject imageDescription;
            imageDescription["metadata_file"] = metadataFileName;
            imageDescription["camera_id"] = frame.cameraId;
            imageDescription["frame_index"] = QString::number(frame.frameIndex);
            imageDescription["timestamp_ns"] = QString::number(timestampNsForStorage(frame));
            imageDescription["bits_per_sample"] = frame.bitsPerSample;
            imageDescription["pixel_format"] = pixelFormatName(frame.pixelFormat);
            imageDescription["pixel_format_id"] = static_cast<int>(pixelFormatId(frame.pixelFormat));
            imageDescription["source_roi_x"] = frame.sourceRoiX;
            imageDescription["source_roi_y"] = frame.sourceRoiY;
            imageDescription["source_roi_width"] = frame.sourceRoiWidth;
            imageDescription["source_roi_height"] = frame.sourceRoiHeight;
            return QJsonDocument(imageDescription).toJson(QJsonDocument::Compact);
        }

        QString recordingExtension(scopeone::core::RecordingFormat format)
        {
            switch (format)
            {
            case scopeone::core::RecordingFormat::Tiff:
                return QStringLiteral(".tif");
            case scopeone::core::RecordingFormat::Binary:
                return QStringLiteral(".bin");
            }
            return QStringLiteral(".dat");
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

        CameraOutputPaths buildCameraOutputPaths(const QString& outputDir,
                                                 const QString& baseName,
                                                 const QString& cameraId,
                                                 scopeone::core::RecordingFormat format)
        {
            CameraOutputPaths paths;
            paths.rawPath = buildSessionFilePath(outputDir,
                                                baseName,
                                                cameraId,
                                                recordingExtension(format));
            if (requiresFrameInfo(format))
            {
                paths.frameInfoPath = buildSessionFilePath(outputDir,
                                                           baseName,
                                                           cameraId,
                                                           QStringLiteral(".csv"),
                                                           QStringLiteral("_frameinfo"));
            }
            return paths;
        }

        bool writeSessionMetadataFile(const QString& filePath, const QByteArray& json, QString& errorMessage)
        {
            QFile file(filePath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            {
                errorMessage = QStringLiteral("Failed to open metadata output: %1").arg(filePath);
                return false;
            }
            if (file.write(json) != json.size())
            {
                errorMessage = QStringLiteral("Failed to write metadata output: %1").arg(filePath);
                return false;
            }
            return true;
        }

        QByteArray frameInfoHeaderLine()
        {
            return QByteArray(
                "camera_id,frame_index,timestamp_ns,width,height,bits_per_sample,stride,pixel_format,pixel_format_id,payload_bytes,source_roi_x,source_roi_y,source_roi_width,source_roi_height\n");
        }

        // Escapes one frame info field for CSV storage
        QString csvField(QString value)
        {
            if (!value.contains(QLatin1Char(','))
                && !value.contains(QLatin1Char('"'))
                && !value.contains(QLatin1Char('\n'))
                && !value.contains(QLatin1Char('\r')))
            {
                return value;
            }

            value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(value);
        }

        QByteArray frameInfoLine(const ImageFrame& frame)
        {
            const quint64 timestampNs = timestampNsForStorage(frame);
            QStringList fields;
            fields.reserve(14);
            fields << csvField(frame.cameraId)
                   << QString::number(frame.frameIndex)
                   << QString::number(timestampNs)
                   << QString::number(frame.width)
                   << QString::number(frame.height)
                   << QString::number(frame.bitsPerSample)
                   << QString::number(frame.stride)
                   << csvField(pixelFormatName(frame.pixelFormat))
                   << QString::number(pixelFormatId(frame.pixelFormat))
                   << QString::number(frame.payloadByteCount())
                   << QString::number(frame.sourceRoiX)
                   << QString::number(frame.sourceRoiY)
                   << QString::number(frame.sourceRoiWidth)
                   << QString::number(frame.sourceRoiHeight);
            return (fields.join(QLatin1Char(',')) + QLatin1Char('\n')).toUtf8();
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

        void discardIncompleteFile(QFile& file)
        {
            const QString filePath = file.fileName();
            if (file.isOpen())
            {
                file.close();
            }
            if (!filePath.isEmpty())
            {
                QFile::remove(filePath);
            }
        }

        bool prepareSessionOutput(const QString& saveDir,
                                  const QString& baseName,
                                  const QString& metadataFileName,
                                  const QByteArray& sessionMetadataJson,
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
            const QString metadataFilePath = QDir(output.outputDir).filePath(output.metadataFileName);
            return writeSessionMetadataFile(metadataFilePath, sessionMetadataJson, errorMessage);
        }

        int tiffStorageBitsForFormat(ImagePixelFormat pixelFormat)
        {
            if (pixelFormat == ImagePixelFormat::Mono8) return 8;
            if (pixelFormat == ImagePixelFormat::Mono16) return 16;
            return 0;
        }

        std::vector<scopeone::core::ScopeOneCore::RecordingAxis> toManifestOrder(const MDAOrder& order)
        {
            std::vector<scopeone::core::ScopeOneCore::RecordingAxis> axes;
            axes.reserve(order.size());
            for (MDAAxis axis : order)
            {
                switch (axis)
                {
                case MDAAxis::Time:
                    axes.push_back(scopeone::core::ScopeOneCore::RecordingAxis::Time);
                    break;
                case MDAAxis::Z:
                    axes.push_back(scopeone::core::ScopeOneCore::RecordingAxis::Z);
                    break;
                case MDAAxis::XY:
                    axes.push_back(scopeone::core::ScopeOneCore::RecordingAxis::XY);
                    break;
                }
            }
            return axes;
        }

        class SaveBackend
        {
        public:
            struct TiffOptions
            {
                bool useDeflate;
                int zipQuality;
                TiffOptions() : useDeflate(true), zipQuality(6) {}
            };

            enum class Format { None, TiffStack, BinaryStream };

            SaveBackend() = default;
            ~SaveBackend() { stopStack(); }

            bool startStackRaw(const QString& filePath,
                               scopeone::core::RecordingFormat recordingFormat,
                               int width,
                               int height,
                               ImagePixelFormat pixelFormat,
                               int bitsPerSample,
                               const TiffOptions& tiff = TiffOptions{})
            {
                stopStack();
                if (!ensureDir(filePath)) return false;
                m_lastError.clear();
                m_format = Format::None;
                m_filePath = filePath;

                if (recordingFormat == scopeone::core::RecordingFormat::Binary)
                {
                    auto file = std::make_unique<QFile>(filePath);
                    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
                    {
                        m_lastError = QStringLiteral("Failed to open binary output");
                        return false;
                    }
                    m_binaryFile = std::move(file);
                    m_format = Format::BinaryStream;
                    m_width = width;
                    m_height = height;
                    m_bits = bitsPerSample;
                    return true;
                }

                const int storageBits = tiffStorageBitsForFormat(pixelFormat);
                if (storageBits == 0)
                {
                    m_lastError = QStringLiteral("Unsupported bit depth");
                    return false;
                }
                m_width = width;
                m_height = height;
                m_bits = storageBits;
                m_useDeflate = tiff.useDeflate;
                m_zipQuality = tiff.zipQuality;
                TIFF* t = reinterpret_cast<TIFF*>(openTiffForWrite(filePath));
                if (!t)
                {
                    m_lastError = QStringLiteral("Failed to open TIFF output");
                    return false;
                }
                m_tiff = t;
                m_format = Format::TiffStack;
                return true;
            }

            bool appendRaw(const uchar* data, qint64 rawBytes, const QByteArray& imageDescription = QByteArray())
            {
                if (!data) return false;
                if (m_format == Format::BinaryStream)
                {
                    if (!m_binaryFile) return false;
                    if (rawBytes <= 0)
                    {
                        m_lastError = QStringLiteral("Invalid binary frame size");
                        return false;
                    }
                    if (m_binaryFile->write(reinterpret_cast<const char*>(data), rawBytes) != rawBytes)
                    {
                        m_lastError = QStringLiteral("Failed to append binary frame");
                        return false;
                    }
                    return true;
                }
                if (m_format != Format::TiffStack || !m_tiff) return false;
                TIFF* t = reinterpret_cast<TIFF*>(m_tiff);
                TIFFCreateDirectory(t);
                setCommonTags(t, m_width, m_height, m_bits);
                if (m_useDeflate)
                {
                    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
                    TIFFSetField(t, TIFFTAG_ZIPQUALITY, m_zipQuality);
                    TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
                }
                else
                {
                    TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
                }
                if (!imageDescription.isEmpty())
                {
                    TIFFSetField(t, TIFFTAG_IMAGEDESCRIPTION, imageDescription.constData());
                }
                if (!writeStrip(t, data, m_width, m_height, m_bits)) return false;
                TIFFWriteDirectory(t);
                return true;
            }

            void stopStack()
            {
                if (m_tiff)
                {
                    TIFFClose(reinterpret_cast<TIFF*>(m_tiff));
                    m_tiff = nullptr;
                }
                if (m_binaryFile)
                {
                    m_binaryFile->close();
                    m_binaryFile.reset();
                }
                m_format = Format::None;
            }

            QString lastError() const { return m_lastError; }
            bool isRecording() const { return m_format != Format::None; }

        private:
            static bool ensureDir(const QString& filePath)
            {
                QFileInfo fi(filePath);
                QDir dir = fi.dir();
                if (dir.exists()) return true;
                return dir.mkpath(".");
            }

            static void* openTiffForWrite(const QString& path)
            {
                const char* mode = "w8";
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

            static void setCommonTags(void* tiff, int width, int height, int bits)
            {
                TIFF* t = reinterpret_cast<TIFF*>(tiff);
                TIFFSetField(t, TIFFTAG_IMAGEWIDTH, width);
                TIFFSetField(t, TIFFTAG_IMAGELENGTH, height);
                TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, bits);
                TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 1);
                TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
                TIFFSetField(t, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
                TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
                TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, height);
            }

            static bool writeStrip(void* tiff, const uchar* data, int width, int height, int bits)
            {
                TIFF* t = reinterpret_cast<TIFF*>(tiff);
                const tmsize_t bytesPerSample = bits / 8;
                const tmsize_t stride = static_cast<tmsize_t>(width) * bytesPerSample;
                const tmsize_t total = stride * height;
                return TIFFWriteEncodedStrip(t, 0, (void*)data, total) != -1;
            }

            void* m_tiff{nullptr};
            int m_width{0};
            int m_height{0};
            int m_bits{0};
            bool m_useDeflate{true};
            int m_zipQuality{6};
            Format m_format{Format::None};
            QString m_lastError;
            QString m_filePath;
            std::unique_ptr<QFile> m_binaryFile;
        };
    } // namespace

    RecordingManager::RecordingManager(QObject* parent)
        : QObject(parent)
    {
    }

    // Stops recording and writer threads during teardown
    RecordingManager::~RecordingManager()
    {
        stop();
        stopStreamingOutputs();
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

    // Emits the current write buffer usage
    void RecordingManager::emitBufferUsageChanged(qint64 pendingWriteBytes)
    {
        emit bufferUsageChanged(pendingWriteBytes);
    }

    // Emits a snapshot of writer status
    void RecordingManager::emitWriterStatus()
    {
        RecordingWriterStatus status;
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            status = m_writerState.status;
            status.setPendingWriteBytes(static_cast<qint64>(m_writerState.pendingWriteBytes));
            status.setMaxPendingWriteBytes(static_cast<qint64>(m_writerState.recordedMaxBytes));
            if (m_sessionState.activeSession)
            {
                m_sessionState.activeSession->setWriterStatusSnapshot(status);
            }
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

    // Returns the total frame count written by all writer threads
    qint64 RecordingManager::totalFramesWritten() const
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        return m_writerState.status.framesWritten();
    }

    // Builds the capture plan before recording starts
    bool RecordingManager::buildCapturePlan(const Settings& settings,
                                            const QStringList& activeCameraIds,
                                            CapturePlan& plan,
                                            QString& errorMessage) const
    {
        plan = CapturePlan{};
        for (const QString& cameraId : activeCameraIds)
        {
            const QString trimmedCameraId = cameraId.trimmed();
            if (!trimmedCameraId.isEmpty() && !plan.activeCameraIds.contains(trimmedCameraId))
            {
                plan.activeCameraIds.append(trimmedCameraId);
            }
        }
        if (plan.activeCameraIds.isEmpty() && m_mmcore)
        {
            plan.activeCameraIds << QStringLiteral("Camera");
        }
        if (plan.activeCameraIds.isEmpty())
        {
            errorMessage = QStringLiteral("No cameras available for recording");
            return false;
        }
        if (settings.streamToDisk && settings.saveDir.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Save directory is empty");
            return false;
        }
        if (settings.streamToDisk && settings.baseName.trimmed().isEmpty())
        {
            errorMessage = QStringLiteral("Base name is empty");
            return false;
        }

        plan.format = settings.format;
        plan.streamToDisk = settings.streamToDisk;
        plan.enableCompression = settings.enableCompression;
        plan.compressionLevel = settings.compressionLevel;
        plan.framesPerBurst = settings.framesPerBurst;
        plan.burstMode = settings.burstMode;
        plan.targetBursts = settings.burstMode ? settings.targetBursts : 1;
        plan.burstIntervalMs = settings.burstIntervalMs;
        plan.mdaIntervalMs = settings.mdaIntervalMs;
        plan.positions = settings.positions;
        plan.zPositions = settings.zPositions;
        plan.order = settings.order;
        plan.saveDir = settings.saveDir.trimmed();
        plan.baseName = settings.baseName.trimmed();
        plan.captureAll = settings.captureAll;
        plan.metadataFileName = settings.metadataFileName.trimmed();
        plan.sessionMetadataJson = settings.sessionMetadataJson;

        const bool hasSpatial = !plan.positions.empty() || !plan.zPositions.empty();
        plan.useMda = (m_mmcore != nullptr) && hasSpatial;
        plan.streamMda = (m_mmcore != nullptr) && (plan.activeCameraIds.size() == 1) && !plan.useMda;
        if (!plan.useMda && !plan.streamMda && !m_mpcm && !m_latestFrameFetcher)
        {
            errorMessage = QStringLiteral("Frame source is not available for recording");
            return false;
        }
        return true;
    }

    // Resets counters and MDA state for a new capture plan
    void RecordingManager::resetCaptureState(const CapturePlan& plan)
    {
        m_captureState.activeCameraIds = plan.activeCameraIds;
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

        m_mdaState.positions = plan.positions;
        m_mdaState.zPositions = plan.zPositions;
        m_mdaState.timePoints = plan.framesPerBurst;
        m_mdaState.intervalMs = plan.mdaIntervalMs;
        m_mdaState.order = plan.order;
        m_mdaState.hasLastEvent = false;
    }

    // Prepares a fresh session object for the next recording
    void RecordingManager::resetSessionState(const CapturePlan& plan)
    {
        m_sessionState.activeSession = std::make_shared<RecordingSessionData>();
        scopeone::core::ScopeOneCore::RecordingCapturePlanData manifestPlan;
        manifestPlan.cameraIds = plan.activeCameraIds;
        manifestPlan.format = plan.format;
        manifestPlan.streamToDisk = plan.streamToDisk;
        manifestPlan.captureAll = plan.captureAll;
        manifestPlan.enableCompression = plan.enableCompression;
        manifestPlan.compressionLevel = plan.compressionLevel;
        manifestPlan.framesPerBurst = plan.framesPerBurst;
        manifestPlan.burstMode = plan.burstMode;
        manifestPlan.targetBursts = plan.targetBursts;
        manifestPlan.burstIntervalMs = plan.burstIntervalMs;
        manifestPlan.mdaIntervalMs = plan.mdaIntervalMs;
        manifestPlan.order = toManifestOrder(plan.order);
        manifestPlan.positions = plan.positions;
        manifestPlan.zPositions = plan.zPositions;
        manifestPlan.saveDir = plan.saveDir;
        manifestPlan.baseName = plan.baseName;
        manifestPlan.metadataFileName = plan.metadataFileName;
        manifestPlan.sessionMetadataJson = plan.sessionMetadataJson;
        m_sessionState.activeSession->setCapturePlan(manifestPlan);
        m_sessionState.activeSession->prepareForSave(plan.streamToDisk, recordedMaxBytes());
        m_sessionState.activeSession->clearFrames();
    }

    // Writes final save result information into the active session
    void RecordingManager::finalizeActiveSession()
    {
        if (!m_sessionState.activeSession)
        {
            return;
        }
        if (!m_captureState.streamToDisk)
        {
            return;
        }
        if (m_writerState.writerError.isEmpty() && !m_sessionState.activeSession->hasRecordedOutput())
        {
            m_writerState.writerError = QStringLiteral("No frames captured");
        }
        const QString result = m_writerState.writerError.isEmpty()
                                   ? QStringLiteral("Success: Saved %1 recording during acquisition").arg(
                                       formatName(m_captureState.format))
                                   : QStringLiteral("Error: %1").arg(m_writerState.writerError);
        updateSessionResult(m_sessionState.activeSession, result, m_writerState.writerError.isEmpty());
    }

    // Starts one writer thread per active camera output
    bool RecordingManager::startStreamingOutputs(const CapturePlan& plan)
    {
        stopStreamingOutputs();

        m_writerState.writerError.clear();
        m_writerState.cameraOutputs.clear();
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.status = RecordingWriterStatus{};
            m_writerState.status.setMaxPendingWriteBytes(static_cast<qint64>(m_writerState.recordedMaxBytes));
        }
        if (m_sessionState.activeSession)
        {
            m_sessionState.activeSession->clearOutputFiles();
            m_sessionState.activeSession->resetSaveResult();
            m_sessionState.activeSession->resetWriterStatus(recordedMaxBytes());
        }
        setWriterStatus(RecordingWriterPhase::Starting);

        SessionOutputInfo outputInfo;
        if (!prepareSessionOutput(plan.saveDir,
                                  plan.baseName,
                                  plan.metadataFileName,
                                  plan.sessionMetadataJson,
                                  outputInfo,
                                  m_writerState.writerError))
        {
            setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
            return false;
        }

        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            auto output = std::make_shared<CameraOutput>();
            output->cameraId = cameraId;
            const CameraOutputPaths paths = buildCameraOutputPaths(outputInfo.outputDir,
                                                                   plan.baseName,
                                                                   cameraId,
                                                                   plan.format);
            output->rawPath = paths.rawPath;
            output->metadataFileName = outputInfo.metadataFileName;
            if (requiresFrameInfo(plan.format))
            {
                output->frameInfoPath = paths.frameInfoPath;
                output->frameInfoFile.setFileName(output->frameInfoPath);
                if (!output->frameInfoFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
                {
                    m_writerState.writerError = QStringLiteral("Failed to open frame info output for %1").arg(cameraId);
                    stopStreamingOutputs();
                    setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
                    return false;
                }

                output->frameInfoFile.write(frameInfoHeaderLine());
                output->frameInfoFile.flush();
            }

            m_writerState.cameraOutputs.insert(cameraId, output);
            if (m_sessionState.activeSession)
            {
                m_sessionState.activeSession->setOutputFilePaths(cameraId, output->rawPath, output->frameInfoPath);
                m_sessionState.activeSession->setOutputFramesWritten(cameraId, 0);
            }
        }

        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.pendingWriteBytes = 0;
        }
        emitBufferUsageChanged(0);
        for (auto it = m_writerState.cameraOutputs.begin(); it != m_writerState.cameraOutputs.end(); ++it)
        {
            const auto& output = it.value();
            if (!output)
            {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(output->queueMutex);
                output->writeQueue.clear();
                output->stopRequested = false;
            }
            output->writerThread = std::thread([this, output]()
            {
                writerLoop(output);
            });
        }
        setWriterStatus(RecordingWriterPhase::Writing);
        return true;
    }

    // Requests all writer threads to stop after queued work
    void RecordingManager::requestWriterStop()
    {
        QList<std::shared_ptr<CameraOutput>> outputs;
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            outputs = m_writerState.cameraOutputs.values();
        }
        for (const auto& output : outputs)
        {
            if (!output)
            {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(output->queueMutex);
                output->stopRequested = true;
            }
            output->writeCondition.notify_all();
        }
    }

    // Stops writer threads and closes all output files
    void RecordingManager::stopStreamingOutputs()
    {
        QList<std::shared_ptr<CameraOutput>> outputs;
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            outputs = m_writerState.cameraOutputs.values();
        }
        requestWriterStop();

        for (const auto& output : outputs)
        {
            if (!output)
            {
                continue;
            }
            if (output->writerThread.joinable())
            {
                output->writerThread.join();
            }
            if (output->backend)
            {
                auto* backend = reinterpret_cast<SaveBackend*>(output->backend);
                delete backend;
                output->backend = nullptr;
            }
            if (output->frameInfoFile.isOpen())
            {
                output->frameInfoFile.flush();
                output->frameInfoFile.close();
            }
            {
                std::lock_guard<std::mutex> lock(output->queueMutex);
                output->writeQueue.clear();
                output->stopRequested = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.cameraOutputs.clear();
            m_writerState.pendingWriteBytes = 0;
        }
        emitBufferUsageChanged(0);
        emitWriterStatus();
    }

    // Drains queued frames for one camera writer thread
    void RecordingManager::writerLoop(const std::shared_ptr<CameraOutput>& output)
    {
        const QString cameraId = output ? output->cameraId : QString{};
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
                {
                    std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
                    if (m_writerState.writerError.isEmpty())
                    {
                        m_writerState.writerError = errorMessage.isEmpty()
                                                        ? QStringLiteral("Unknown recording writer error")
                                                        : errorMessage;
                    }
                    m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
                }
                requestWriterStop();
                emitWriterStatus();
                QMetaObject::invokeMethod(this, [this]()
                {
                    if (m_captureState.isRecording)
                    {
                        stop();
                    }
                }, Qt::QueuedConnection);
                break;
            }

            qint64 pendingWriteBytes = 0;
            {
                std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
                m_writerState.pendingWriteBytes -= static_cast<size_t>(task.frame.payloadByteCount());
                m_writerState.status.addWrittenFrames(1);
                pendingWriteBytes = static_cast<qint64>(m_writerState.pendingWriteBytes);
                if (m_sessionState.activeSession)
                {
                    const qint64 framesWritten =
                        m_sessionState.activeSession->ensureFileManifest(cameraId).framesWritten + 1;
                    m_sessionState.activeSession->setOutputFramesWritten(cameraId, framesWritten);
                }
            }
            emitBufferUsageChanged(pendingWriteBytes);
            emitWriterStatus();
        }
    }

    // Writes one queued frame to its camera output
    bool RecordingManager::writeTask(CameraOutput& output, const WriteTask& task, QString& errorMessage)
    {
        auto* backend = reinterpret_cast<SaveBackend*>(output.backend);
        if (!backend)
        {
            SaveBackend::TiffOptions tiffOpts;
            tiffOpts.useDeflate = m_captureState.enableCompression;
            tiffOpts.zipQuality = m_captureState.compressionLevel;

            auto newBackend = std::make_unique<SaveBackend>();
            if (!newBackend->startStackRaw(output.rawPath,
                                           m_captureState.format,
                                           task.frame.width,
                                           task.frame.height,
                                           task.frame.pixelFormat,
                                           task.frame.bitsPerSample,
                                           tiffOpts))
            {
                errorMessage = QStringLiteral("Failed to open raw output for %1: %2")
                               .arg(output.cameraId)
                               .arg(newBackend->lastError());
                return false;
            }
            output.backend = newBackend.release();
            output.width = task.frame.width;
            output.height = task.frame.height;
            output.bits = task.frame.bitsPerSample;
            output.pixelFormat = task.frame.pixelFormat;
            backend = reinterpret_cast<SaveBackend*>(output.backend);
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

        const QByteArray imageDescription = m_captureState.format == RecordingFormat::Tiff
                                                ? buildImageDescriptionJson(output.metadataFileName, task.frame)
                                                : QByteArray();
        if (!backend->appendRaw(payload.data(),
                                payload.byteCount,
                                imageDescription))
        {
            errorMessage = QStringLiteral("Failed writing raw frame for %1: %2")
                           .arg(output.cameraId)
                           .arg(backend->lastError());
            return false;
        }

        if (output.frameInfoFile.isOpen())
        {
            const QByteArray infoLine = frameInfoLine(task.frame);
            if (output.frameInfoFile.write(infoLine) != infoLine.size())
            {
                errorMessage = QStringLiteral("Failed writing frame info for %1").arg(output.cameraId);
                return false;
            }
        }
        return true;
    }

    // Converts a recording format enum into its display name
    QString RecordingManager::formatName(RecordingFormat format) const
    {
        return recordingFormatName(format);
    }

    // Starts a recording session using the requested settings
    bool RecordingManager::start(const Settings& settings, const QStringList& activeCameraIds)
    {
        if (m_captureState.isRecording)
        {
            qWarning().noquote() << "Recording already running";
            return false;
        }

        CapturePlan plan;
        QString errorMessage;
        if (!buildCapturePlan(settings, activeCameraIds, plan, errorMessage))
        {
            qWarning().noquote() << errorMessage;
            return false;
        }

        resetCaptureState(plan);
        if (!plan.useMda)
        {
            primeLastFrameIndices();
        }
        resetSessionState(plan);

        if (plan.streamToDisk)
        {
            if (!startStreamingOutputs(plan))
            {
                qWarning().noquote() << (m_writerState.writerError.isEmpty()
                                             ? QStringLiteral("Failed to start streaming outputs")
                                             : m_writerState.writerError);
                m_sessionState.activeSession.reset();
                return false;
            }
        }

        m_captureState.elapsedTimer.start();
        m_captureState.isRecording = true;
        m_mdaState.usingMda = plan.useMda;
        m_mdaState.streamMda = plan.streamMda;
        m_mdaState.streamIntervalMs = plan.streamMda ? plan.mdaIntervalMs : 0.0;
        m_mdaState.lastStreamCaptureMs = plan.streamMda ? static_cast<qint64>(-m_mdaState.streamIntervalMs) : 0;
        m_captureState.phase = plan.useMda
                                   ? kRecordingPhaseRecordingMda
                                   : (m_captureState.burstMode
                                          ? kRecordingPhaseRecordingBurst
                                          : kRecordingPhaseRecording);
        emit recordingStateChanged(true);
        emitProgress();

        qInfo().noquote() << QString("Recording started (%1 camera(s))").arg(m_captureState.activeCameraIds.size());

        if (plan.useMda)
        {
            if (!startMdaCapture())
            {
                stop();
                return false;
            }
        }
        return true;
    }

    // Stops the active recording session and emits the finished session
    void RecordingManager::stop()
    {
        if (!m_captureState.isRecording) return;
        if (m_mdaState.usingMda && m_mdaState.manager && m_mdaState.manager->isRunning())
        {
            m_mdaState.manager->requestCancel();
        }
        if (m_mpcm)
        {
            m_mpcm->setPollingPaused(false);
        }

        if (m_captureState.streamToDisk)
        {
            setWriterStatus(RecordingWriterPhase::Stopping);
            stopStreamingOutputs();
        }

        m_captureState.isRecording = false;
        m_captureState.phase = kRecordingPhaseStopped;
        emit recordingStateChanged(false);
        emitProgress();

        qInfo().noquote() << "Recording stopped";

        auto session = m_sessionState.activeSession;
        finalizeActiveSession();
        if (m_captureState.streamToDisk)
        {
            if (m_writerState.writerError.isEmpty())
            {
                setWriterStatus(RecordingWriterPhase::Completed);
            }
            else
            {
                setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
            }
        }
        m_sessionState.activeSession.reset();
        m_mdaState.usingMda = false;
        m_mdaState.streamMda = false;
        m_mdaState.streamIntervalMs = 0.0;
        m_mdaState.lastStreamCaptureMs = 0;
        m_mdaState.cameraId.clear();
        m_mdaState.frameIndex = 0;
        m_mdaState.positions.clear();
        m_mdaState.zPositions.clear();
        m_mdaState.timePoints = 0;
        m_mdaState.intervalMs = 0.0;
        m_mdaState.burstsRemaining = 0;
        m_mdaState.hasLastEvent = false;
        emit recordingStopped(session);
    }

    // Receives one raw preview frame for recording ingestion
    void RecordingManager::onNewRawFrameReady(const ImageFrame& frame)
    {
        ingestFrame(FramePacket{frame, FramePacket::Source::PreviewStream});
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
    void RecordingManager::emitProgress()
    {
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

        const int tCount = (m_mdaState.timePoints > 0) ? m_mdaState.timePoints : 1;
        const int zCount = m_mdaState.zPositions.empty() ? 1 : static_cast<int>(m_mdaState.zPositions.size());
        const int xyCount = m_mdaState.positions.empty() ? 1 : static_cast<int>(m_mdaState.positions.size());
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
            mdaTimeIndex = m_mdaState.lastEvent.tIndex + 1;
            mdaZIndex = m_mdaState.lastEvent.zIndex + 1;
            mdaPositionIndex = m_mdaState.lastEvent.positionIndex + 1;
            hasXY = !m_mdaState.positions.empty();
            x = m_mdaState.lastEvent.x;
            y = m_mdaState.lastEvent.y;
            hasZ = !m_mdaState.zPositions.empty();
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
    bool RecordingManager::enqueueFrame(const ImageFrame& frame)
    {
        const QString cameraId = frame.cameraId.trimmed();
        const size_t frameBytes = static_cast<size_t>(frame.payloadByteCount());
        qint64 pendingWriteBytes = 0;
        std::shared_ptr<CameraOutput> output;
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
                emitStatus = true;
            }
            else if (m_writerState.pendingWriteBytes + frameBytes > m_writerState.recordedMaxBytes)
            {
                if (m_writerState.writerError.isEmpty())
                {
                    m_writerState.writerError = QStringLiteral("Recording write queue exceeded limit");
                }
                m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
                emitStatus = true;
            }
            else
            {
                output = it.value();
                m_writerState.pendingWriteBytes += frameBytes;
                pendingWriteBytes = static_cast<qint64>(m_writerState.pendingWriteBytes);
            }
        }
        if (!output)
        {
            if (emitStatus)
            {
                qWarning().noquote() << "Recording write queue full or output missing, stopping capture";
                QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
                emitWriterStatus();
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(output->queueMutex);
            if (output->stopRequested)
            {
                qint64 revertedPendingBytes = 0;
                {
                    std::lock_guard<std::mutex> stateLock(m_writerState.writeMutex);
                    m_writerState.pendingWriteBytes -= frameBytes;
                    revertedPendingBytes = static_cast<qint64>(m_writerState.pendingWriteBytes);
                }
                emitBufferUsageChanged(revertedPendingBytes);
                return false;
            }
            output->writeQueue.push_back(WriteTask{frame});
        }
        emitBufferUsageChanged(pendingWriteBytes);
        output->writeCondition.notify_one();
        emitWriterStatus();
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
        if (!m_sessionState.activeSession) return false;
        return packet.frame.frameIndex > m_captureState.lastFrameIndex.value(cameraId, 0);
    }

    // Applies burst timing and records one accepted frame
    void RecordingManager::ingestFrame(const FramePacket& packet)
    {
        if (!shouldAcceptFrame(packet)) return;
        ImageFrame frame = packet.frame;
        frame.cameraId = frame.cameraId.trimmed();
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

        if (!m_mdaState.usingMda && !shouldCaptureCamera(cameraId))
        {
            return;
        }

        if (m_captureState.streamToDisk && !m_writerState.writerError.isEmpty())
        {
            stop();
            return;
        }

        if (m_captureState.streamToDisk)
        {
            if (!enqueueFrame(frame))
            {
                return;
            }
        }
        else if (m_sessionState.activeSession)
        {
            m_sessionState.activeSession->appendImageFrame(frame);
        }
        m_captureState.lastFrameIndex[cameraId] = frame.frameIndex;

        m_captureState.framesCapturedThisBurst[cameraId] += 1;
        m_captureState.framesCapturedTotal[cameraId] += 1;
        emitProgress();

        (void)advanceBurstStateIfNeeded();
    }

    // Converts MDA output frames into recording frame packets
    void RecordingManager::handleMdaFrame(const MDAOutput& frame)
    {
        if (!m_captureState.isRecording || !m_mdaState.usingMda) return;

        m_mdaState.lastEvent = frame.event;
        m_mdaState.hasLastEvent = true;

        const auto dispatchMdaFrame = [this, &frame](ImageFrame rawFrame)
        {
            rawFrame.cameraId = rawFrame.cameraId.trimmed();
            if (rawFrame.cameraId.isEmpty())
            {
                rawFrame.cameraId = m_mdaState.cameraId;
            }
            if (rawFrame.timestampNs == 0 && frame.timestampMs > 0)
            {
                rawFrame.timestampNs = static_cast<quint64>(frame.timestampMs) * 1000000ull;
            }
            if (rawFrame.frameIndex <= m_captureState.lastFrameIndex.value(rawFrame.cameraId, 0))
            {
                rawFrame.frameIndex = m_captureState.lastFrameIndex.value(rawFrame.cameraId, 0) + 1;
            }
            ingestFrame(FramePacket{rawFrame, FramePacket::Source::Mda});
            if (rawFrame.isValid())
            {
                emit mdaRawFrameReady(rawFrame);
            }
        };

        if (frame.frames.isEmpty())
        {
            return;
        }

        for (auto it = frame.frames.constBegin(); it != frame.frames.constEnd(); ++it)
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
            dispatchMdaFrame(rawFrame);
        }
    }

    // Starts MDA driven recording capture
    bool RecordingManager::startMdaCapture()
    {
        if (!m_mmcore)
        {
            qWarning().noquote() << "MMCore not available for MDA";
            return false;
        }
        if (m_captureState.activeCameraIds.isEmpty())
        {
            qWarning().noquote() << "MDA requires at least one camera";
            return false;
        }
        if (m_captureState.activeCameraIds.size() > 1 && !m_mpcm)
        {
            qWarning().noquote() << "Multi-camera MDA requires MultiProcessCameraManager";
            return false;
        }
        if (m_captureState.activeCameraIds.size() > 1 && m_mpcm)
        {
            m_mpcm->setPollingPaused(true);
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

        if (!m_mdaState.manager)
        {
            m_mdaState.manager = new MDAManager(m_mmcore, this);
        }
        m_mdaState.manager->setCameras(m_captureState.activeCameraIds);
        m_mdaState.manager->setMultiProcessCameraManager(m_mpcm);
        if (!m_mdaState.signalsConnected)
        {
            connect(m_mdaState.manager, &MDAManager::frameReady, this, [this](const MDAOutput& frame)
            {
                handleMdaFrame(frame);
            });
            connect(m_mdaState.manager, &MDAManager::sequenceFinished, this, [this]()
            {
                if (m_mdaState.usingMda && m_captureState.isRecording)
                {
                    m_mdaState.burstsRemaining -= 1;
                    if (m_mdaState.burstsRemaining > 0)
                    {
                        m_captureState.waitingBetweenBursts = true;
                        m_captureState.lastBurstEndMs = m_captureState.elapsedTimer.elapsed();
                        m_captureState.phase = kRecordingPhaseWaitingNextBurst;
                        emitProgress();
                        const int waitMs = static_cast<int>(m_captureState.burstIntervalMs);
                        QTimer::singleShot(waitMs, this, [this]() { startMdaRun(); });
                    }
                    else
                    {
                        stop();
                    }
                }
            });
            connect(m_mdaState.manager, &MDAManager::sequenceCanceled, this, [this]()
            {
                if (m_mdaState.usingMda && m_captureState.isRecording)
                {
                    stop();
                }
            });
            connect(m_mdaState.manager, &MDAManager::sequenceError, this, [this](const QString& message)
            {
                qWarning().noquote() << QString("MDA error: %1").arg(message);
                if (m_mdaState.usingMda && m_captureState.isRecording)
                {
                    stop();
                }
            });
            m_mdaState.signalsConnected = true;
        }

        startMdaRun();
        return true;
    }

    // Starts the next MDA burst run
    void RecordingManager::startMdaRun()
    {
        if (!m_captureState.isRecording || !m_mdaState.usingMda)
        {
            return;
        }
        if (m_mdaState.burstsRemaining <= 0)
        {
            stop();
            return;
        }

        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            m_captureState.framesCapturedThisBurst[cameraId] = 0;
        }

        m_captureState.waitingBetweenBursts = false;
        m_captureState.currentBurst += 1;
        m_captureState.phase = kRecordingPhaseRecordingMda;
        emitProgress();

        m_mdaState.manager->start(m_mdaState.timePoints, m_mdaState.intervalMs, 0.0, m_mdaState.positions,
                                  m_mdaState.zPositions, m_mdaState.order, false);
    }

    // Checks whether one camera still needs frames in the current burst
    bool RecordingManager::shouldCaptureCamera(const QString& cameraId) const
    {
        return m_captureState.framesCapturedThisBurst.value(cameraId, 0) < m_captureState.framesPerBurst;
    }

    // Checks whether all cameras reached the current frame target
    bool RecordingManager::allCamerasReachedTarget() const
    {
        if (m_captureState.activeCameraIds.isEmpty()) return true;
        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            if (m_captureState.framesCapturedThisBurst.value(cameraId, 0) < m_captureState.framesPerBurst)
            {
                return false;
            }
        }
        return true;
    }

    // Advances burst state after an accepted frame
    bool RecordingManager::advanceBurstStateIfNeeded()
    {
        if (m_mdaState.usingMda)
        {
            return false;
        }
        if (!m_captureState.burstMode)
        {
            if (allCamerasReachedTarget())
            {
                stop();
                return true;
            }
            return false;
        }

        if (!allCamerasReachedTarget())
        {
            return false;
        }

        m_captureState.currentBurst += 1;
        m_captureState.phase = kRecordingPhaseWaitingNextBurst;
        emitProgress();

        if (m_captureState.currentBurst >= m_captureState.targetBursts)
        {
            stop();
            return true;
        }

        for (const QString& cameraId : m_captureState.activeCameraIds)
        {
            m_captureState.framesCapturedThisBurst[cameraId] = 0;
        }

        m_captureState.waitingBetweenBursts = true;
        m_captureState.lastBurstEndMs = m_captureState.elapsedTimer.elapsed();
        return false;
    }

    // Saves buffered sessions and preserves direct writer outputs
    QString RecordingManager::saveSessionToDisk(const std::shared_ptr<RecordingSessionData>& session)
    {
        if (!session)
        {
            return QStringLiteral("Error: Missing recording session");
        }
        const auto& capturePlan = session->capturePlan();
        if (capturePlan.cameraIds.isEmpty())
        {
            return updateSessionResult(session, QStringLiteral("Error: No cameras to save"), false);
        }

        if (!session->hasAnyFrames())
        {
            if (!session->saveMessage().isEmpty())
            {
                return session->saveMessage();
            }
            if (session->isSaved())
            {
                return QStringLiteral("Success: Recording was already saved during acquisition");
            }
            if (session->streamedToDisk() && session->hasRecordedOutput())
            {
                return updateSessionResult(
                    session,
                    QStringLiteral("Success: Recording was already saved during acquisition"),
                    true);
            }
            return updateSessionResult(session, QStringLiteral("Error: No frames captured"), false);
        }

        SessionOutputInfo outputInfo;
        QString outputErrorMessage;
        if (!prepareSessionOutput(capturePlan.saveDir,
                                  capturePlan.baseName,
                                  capturePlan.metadataFileName,
                                  capturePlan.sessionMetadataJson,
                                  outputInfo,
                                  outputErrorMessage))
        {
            return updateSessionResult(session, QStringLiteral("Error: %1").arg(outputErrorMessage), false);
        }

        session->prepareForSave(session->streamedToDisk());
        session->setWriterPhase(RecordingWriterPhase::Starting);
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
            const CameraOutputPaths paths = buildCameraOutputPaths(outputInfo.outputDir,
                                                                   capturePlan.baseName,
                                                                   cameraId,
                                                                   capturePlan.format);
            QFile frameInfoFile;
            if (requiresFrameInfo(capturePlan.format))
            {
                frameInfoFile.setFileName(paths.frameInfoPath);
                if (!frameInfoFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
                {
                    const QString errorMessage = QString("Failed to open frame info output for %1").arg(cameraId);
                    session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                    return updateSessionResult(
                        session,
                        QStringLiteral("Error: %1").arg(errorMessage),
                        false);
                }
                const QByteArray header = frameInfoHeaderLine();
                if (frameInfoFile.write(header) != header.size())
                {
                    discardIncompleteFile(frameInfoFile);
                    const QString errorMessage = QString("Failed to write frame info header for %1").arg(cameraId);
                    session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                    return updateSessionResult(
                        session,
                        QStringLiteral("Error: %1").arg(errorMessage),
                        false);
                }
            }
            if (!rawSaver.startStackRaw(paths.rawPath,
                                        capturePlan.format,
                                        firstImageFrame.width,
                                        firstImageFrame.height,
                                        firstImageFrame.pixelFormat,
                                        firstImageFrame.bitsPerSample,
                                        tiffOpts))
            {
                discardIncompleteFile(frameInfoFile);
                const QString errorMessage = QString("Failed to start raw output for %1").arg(cameraId);
                session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                return updateSessionResult(
                    session,
                    QStringLiteral("Error: %1").arg(errorMessage),
                    false);
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
                    discardIncompleteFile(frameInfoFile);
                    rawSaver.stopStack();
                    const QString errorMessage = QString("Frame format changed during save for %1").arg(cameraId);
                    session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                    return updateSessionResult(
                        session,
                        QStringLiteral("Error: %1").arg(errorMessage),
                        false);
                }
                const FramePayloadView payload = framePayloadForWrite(imageFrame, capturePlan.format);
                if (!payload.data() || payload.byteCount <= 0)
                {
                    discardIncompleteFile(frameInfoFile);
                    rawSaver.stopStack();
                    const QString errorMessage = QString("Invalid frame payload for %1").arg(cameraId);
                    session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                    return updateSessionResult(
                        session,
                        QStringLiteral("Error: %1").arg(errorMessage),
                        false);
                }

                const QByteArray imageDescription = capturePlan.format == RecordingFormat::Tiff
                                                        ? buildImageDescriptionJson(outputInfo.metadataFileName,
                                                                                    imageFrame)
                                                        : QByteArray();
                if (!rawSaver.appendRaw(payload.data(),
                                        payload.byteCount,
                                        imageDescription))
                {
                    discardIncompleteFile(frameInfoFile);
                    rawSaver.stopStack();
                    const QString errorMessage = QString("Failed to append raw frame %1 for %2").arg(saved).arg(
                        cameraId);
                    session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                    return updateSessionResult(
                        session,
                        QStringLiteral("Error: %1").arg(errorMessage),
                        false);
                }
                if (frameInfoFile.isOpen())
                {
                    const QByteArray infoLine = frameInfoLine(imageFrame);
                    if (frameInfoFile.write(infoLine) != infoLine.size())
                    {
                        discardIncompleteFile(frameInfoFile);
                        rawSaver.stopStack();
                        const QString errorMessage = QString("Failed to write frame info for %1").arg(cameraId);
                        session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                        return updateSessionResult(
                            session,
                            QStringLiteral("Error: %1").arg(errorMessage),
                            false);
                    }
                }
                saved += 1;
            }
            if (frameInfoFile.isOpen())
            {
                frameInfoFile.close();
            }
            rawSaver.stopStack();
            session->setOutputFilePaths(cameraId, paths.rawPath, paths.frameInfoPath);
            session->setOutputFramesWritten(cameraId, saved);
            session->addWrittenFrames(saved);
        }
        session->setWriterPhase(RecordingWriterPhase::Completed);
        return updateSessionResult(
            session,
            QStringLiteral("Success: Saved %1 recording").arg(recordingFormatName(capturePlan.format)),
            true);
    }
} // namespace scopeone::core::internal
