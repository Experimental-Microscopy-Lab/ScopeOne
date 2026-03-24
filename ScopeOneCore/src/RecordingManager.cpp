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
#include <QTimer>
#include <algorithm>
#include <limits>
#include <tiffio.h>

namespace scopeone::core::internal {

using scopeone::core::RecordingFormat;
using scopeone::core::SharedFrameHeader;
using scopeone::core::SharedPixelFormat;
using scopeone::core::kRecordingPhaseIdle;
using scopeone::core::kRecordingPhaseRecording;
using scopeone::core::kRecordingPhaseRecordingBurst;
using scopeone::core::kRecordingPhaseRecordingMda;
using scopeone::core::kRecordingPhaseWaitingNextBurst;
using scopeone::core::kRecordingPhaseStopped;

namespace {
QString recordingFormatName(scopeone::core::RecordingFormat format)
{
    switch (format) {
    case scopeone::core::RecordingFormat::Tiff:
        return QStringLiteral("TIFF");
    case scopeone::core::RecordingFormat::Binary:
        return QStringLiteral("Binary");
    }
    return QStringLiteral("Unknown");
}

QString pixelFormatName(quint32 pixelFormat)
{
    switch (static_cast<scopeone::core::SharedPixelFormat>(pixelFormat)) {
    case scopeone::core::SharedPixelFormat::Mono8:
        return QStringLiteral("Mono8");
    case scopeone::core::SharedPixelFormat::Mono16:
        return QStringLiteral("Mono16");
    default:
        return QStringLiteral("Unknown");
    }
}

QString updateSessionResult(const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
                            const QString& result,
                            bool saved)
{
    if (session) {
        session->setSaveResult(saved, result);
    }
    return result;
}

int resolveFrameBits(const SharedFrameHeader& header)
{
    const int bits = static_cast<int>(header.bitsPerSample);
    if (bits > 0) {
        return bits;
    }
    switch (static_cast<SharedPixelFormat>(header.pixelFormat)) {
    case SharedPixelFormat::Mono8:
        return 8;
    case SharedPixelFormat::Mono16:
        return 16;
    default:
        return 0;
    }
}

QByteArray buildImageDescriptionJson(const QString& metadataFileName)
{
    QJsonObject imageDescription;
    imageDescription["metadata_file"] = metadataFileName;
    return QJsonDocument(imageDescription).toJson(QJsonDocument::Compact);
}

QString recordingExtension(scopeone::core::RecordingFormat format)
{
    switch (format) {
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
    if (baseName.endsWith(extension, Qt::CaseInsensitive)) {
        return baseName;
    }
    return baseName + extension;
}

QString buildSessionFilePath(const QString& dir,
                             const QString& baseName,
                             const QString& cameraId,
                             const QString& extension,
                             const QString& suffix = QString())
{
    QString name = baseName;
    if (!cameraId.isEmpty()) {
        name += "_" + cameraId;
    }
    if (!suffix.isEmpty()) {
        name += suffix;
    }
    name = ensureExtensionName(name, extension);
    return QDir(dir).filePath(name);
}

QString metadataFileNameForPlan(const QString& baseName, const QString& metadataFileName)
{
    const QString trimmedMetadataFileName = metadataFileName.trimmed();
    if (!trimmedMetadataFileName.isEmpty()) {
        return ensureExtensionName(trimmedMetadataFileName, QStringLiteral(".json"));
    }
    return ensureExtensionName(baseName + QStringLiteral("_metadata"), QStringLiteral(".json"));
}

bool writeSessionMetadataFile(const QString& filePath, const QByteArray& json, QString& errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorMessage = QStringLiteral("Failed to open metadata output: %1").arg(filePath);
        return false;
    }
    if (file.write(json) != json.size()) {
        errorMessage = QStringLiteral("Failed to write metadata output: %1").arg(filePath);
        return false;
    }
    return true;
}

int normalizeBitsForStorage(int bitsPerSample)
{
    if (bitsPerSample <= 0) return 0;
    if (bitsPerSample <= 8) return 8;
    if (bitsPerSample <= 16) return 16;
    return 0;
}

std::vector<scopeone::core::ScopeOneCore::RecordingAxis> toManifestOrder(const MDAOrder& order)
{
    std::vector<scopeone::core::ScopeOneCore::RecordingAxis> axes;
    axes.reserve(order.size());
    for (MDAAxis axis : order) {
        switch (axis) {
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
    struct TiffOptions { bool useDeflate{true}; int zipQuality{6}; };
    enum class Format { None, TiffStack, BinaryStream };

    SaveBackend() = default;
    ~SaveBackend() { stopStack(); }

    bool startStackRaw(const QString& filePath,
                       scopeone::core::RecordingFormat recordingFormat,
                       int width,
                       int height,
                       int bitsPerSample,
                       const TiffOptions& tiff = {})
    {
        stopStack();
        if (!ensureDir(filePath)) return false;
        m_lastError.clear();
        m_format = Format::None;
        m_filePath = filePath;

        if (recordingFormat == scopeone::core::RecordingFormat::Binary) {
            auto file = std::make_unique<QFile>(filePath);
            if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
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

        const int storageBits = normalizeBitsForStorage(bitsPerSample);
        if (storageBits == 0) {
            m_lastError = QStringLiteral("Unsupported bit depth");
            return false;
        }
        m_width = width;
        m_height = height;
        m_bits = storageBits;
        m_useDeflate = tiff.useDeflate;
        m_zipQuality = tiff.zipQuality;
        TIFF* t = reinterpret_cast<TIFF*>(openTiffForWrite(filePath));
        if (!t) {
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
        if (m_format == Format::BinaryStream) {
            if (!m_binaryFile) return false;
            if (rawBytes <= 0) {
                m_lastError = QStringLiteral("Invalid binary frame size");
                return false;
            }
            if (m_binaryFile->write(reinterpret_cast<const char*>(data), rawBytes) != rawBytes) {
                m_lastError = QStringLiteral("Failed to append binary frame");
                return false;
            }
            return true;
        }
        if (m_format != Format::TiffStack || !m_tiff) return false;
        TIFF* t = reinterpret_cast<TIFF*>(m_tiff);
        TIFFCreateDirectory(t);
        setCommonTags(t, m_width, m_height, m_bits);
        if (m_useDeflate) {
            TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
            TIFFSetField(t, TIFFTAG_ZIPQUALITY, m_zipQuality);
            TIFFSetField(t, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
        } else {
            TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
        }
        if (!imageDescription.isEmpty()) {
            TIFFSetField(t, TIFFTAG_IMAGEDESCRIPTION, imageDescription.constData());
        }
        if (!writeStrip(t, data, m_width, m_height, m_bits)) return false;
        TIFFWriteDirectory(t);
        return true;
    }

    void stopStack()
    {
        if (m_tiff) {
            TIFFClose(reinterpret_cast<TIFF*>(m_tiff));
            m_tiff = nullptr;
        }
        if (m_binaryFile) {
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

RecordingManager::~RecordingManager()
{
    stop();
    stopStreamingOutputs();
}

void RecordingManager::setRecordedMaxBytes(qint64 bytes)
{
    if (bytes <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
    m_writerState.recordedMaxBytes = static_cast<size_t>(bytes);
    m_writerState.status.setMaxPendingWriteBytes(bytes);
}

qint64 RecordingManager::recordedMaxBytes() const
{
    std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
    return static_cast<qint64>(m_writerState.recordedMaxBytes);
}

void RecordingManager::emitBufferUsageChanged(qint64 pendingWriteBytes)
{
    emit bufferUsageChanged(pendingWriteBytes);
}

void RecordingManager::emitWriterStatus()
{
    RecordingWriterStatus status;
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        status = m_writerState.status;
        status.setPendingWriteBytes(static_cast<qint64>(m_writerState.pendingWriteBytes));
        status.setMaxPendingWriteBytes(static_cast<qint64>(m_writerState.recordedMaxBytes));
    }
    if (m_sessionState.activeSession) {
        m_sessionState.activeSession->setWriterStatusSnapshot(status);
    }
    emit writerStatusChanged(status);
}

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

qint64 RecordingManager::totalFramesWritten() const
{
    std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
    return m_writerState.status.framesWritten();
}

bool RecordingManager::buildCapturePlan(const Settings& settings,
                                        const QStringList& activeCameraIds,
                                        CapturePlan& plan,
                                        QString& errorMessage) const
{
    // Pick the capture path before recording starts
    plan = CapturePlan{};
    plan.activeCameraIds = activeCameraIds;
    if (plan.activeCameraIds.isEmpty() && m_mmcore) {
        plan.activeCameraIds << QStringLiteral("Camera");
    }
    if (plan.activeCameraIds.isEmpty()) {
        errorMessage = QStringLiteral("No cameras available for recording");
        return false;
    }
    if (settings.saveDir.trimmed().isEmpty()) {
        errorMessage = QStringLiteral("Save directory is empty");
        return false;
    }
    if (settings.baseName.trimmed().isEmpty()) {
        errorMessage = QStringLiteral("Base name is empty");
        return false;
    }

    plan.format = settings.format;
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
    plan.saveDir = settings.saveDir;
    plan.baseName = settings.baseName;
    plan.captureAll = settings.captureAll;
    plan.metadataFileName = settings.metadataFileName;
    plan.sessionMetadataJson = settings.sessionMetadataJson;

    const bool hasSpatial = !plan.positions.empty() || !plan.zPositions.empty();
    const bool multiCamera = plan.activeCameraIds.size() > 1;
    plan.useMda = (m_mmcore != nullptr) && (hasSpatial || multiCamera);
    plan.streamMda = (m_mmcore != nullptr) && (plan.activeCameraIds.size() == 1) && !plan.useMda;
    if (!plan.useMda && !plan.streamMda && !m_mpcm && !m_latestFrameFetcher) {
        errorMessage = QStringLiteral("Frame source is not available for recording");
        return false;
    }
    return true;
}

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
    m_captureState.lastFrameIndex.clear();
    m_captureState.framesCapturedThisBurst.clear();
    m_captureState.framesCapturedTotal.clear();
    for (const QString& cameraId : m_captureState.activeCameraIds) {
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

void RecordingManager::resetSessionState(const CapturePlan& plan)
{
    // Prepare a fresh session for this run
    m_sessionState.activeSession = std::make_shared<RecordingSessionData>();
    scopeone::core::ScopeOneCore::RecordingCapturePlanData manifestPlan;
    manifestPlan.cameraIds = plan.activeCameraIds;
    manifestPlan.format = plan.format;
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
    m_sessionState.activeSession->prepareForSave(true, recordedMaxBytes());
    m_sessionState.activeSession->clearFrames();
}

void RecordingManager::finalizeActiveSession()
{
    if (!m_sessionState.activeSession) {
        return;
    }
    const QString result = m_writerState.writerError.isEmpty()
        ? QStringLiteral("Success: Saved %1 recording during acquisition").arg(formatName(m_captureState.format))
        : QStringLiteral("Error: %1").arg(m_writerState.writerError);
    updateSessionResult(m_sessionState.activeSession, result, m_writerState.writerError.isEmpty());
}

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
    if (m_sessionState.activeSession) {
        m_sessionState.activeSession->clearOutputFiles();
        m_sessionState.activeSession->resetSaveResult();
        m_sessionState.activeSession->resetWriterStatus(recordedMaxBytes());
    }
    setWriterStatus(RecordingWriterPhase::Starting);

    if (!QDir().mkpath(plan.saveDir)) {
        m_writerState.writerError = QStringLiteral("Failed to create save directory: %1").arg(plan.saveDir);
        setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
        return false;
    }

    const QString metadataFileName = metadataFileNameForPlan(plan.baseName, plan.metadataFileName);
    const QString metadataFilePath = QDir(plan.saveDir).filePath(metadataFileName);
    if (!writeSessionMetadataFile(metadataFilePath, plan.sessionMetadataJson, m_writerState.writerError)) {
        setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
        return false;
    }

    for (const QString& cameraId : m_captureState.activeCameraIds) {
        auto output = std::make_shared<CameraOutput>();
        output->rawPath = buildSessionFilePath(plan.saveDir,
                                               plan.baseName,
                                               cameraId,
                                               recordingExtension(plan.format));
        output->metadataFileName = metadataFileName;
        if (requiresFrameInfo(plan.format)) {
            output->frameInfoPath = buildSessionFilePath(plan.saveDir,
                                                         plan.baseName,
                                                         cameraId,
                                                         QStringLiteral(".csv"),
                                                         QStringLiteral("_frameinfo"));

            output->frameInfoFile.setFileName(output->frameInfoPath);
            if (!output->frameInfoFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                m_writerState.writerError = QStringLiteral("Failed to open frame info output for %1").arg(cameraId);
                stopStreamingOutputs();
                setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
                return false;
            }

            writeFrameInfoHeader(*output);
            output->frameInfoFile.flush();
        }

        m_writerState.cameraOutputs.insert(cameraId, output);
        if (m_sessionState.activeSession) {
            m_sessionState.activeSession->setOutputFilePaths(cameraId, output->rawPath, output->frameInfoPath);
            m_sessionState.activeSession->setOutputFramesWritten(cameraId, 0);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        m_writerState.pendingWriteBytes = 0;
        m_writerState.writeQueue.clear();
        m_writerState.writerStopRequested = false;
    }
    emitBufferUsageChanged(0);
    m_writerState.writerThread = std::thread([this]() { writerLoop(); });
    setWriterStatus(RecordingWriterPhase::Writing);
    return true;
}

void RecordingManager::stopStreamingOutputs()
{
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        m_writerState.writerStopRequested = true;
    }
    m_writerState.writeCondition.notify_all();

    if (m_writerState.writerThread.joinable()) {
        m_writerState.writerThread.join();
    }

    for (auto it = m_writerState.cameraOutputs.begin(); it != m_writerState.cameraOutputs.end(); ++it) {
        auto output = it.value();
        if (!output) {
            continue;
        }
        if (output->backend) {
            auto* backend = reinterpret_cast<SaveBackend*>(output->backend);
            delete backend;
            output->backend = nullptr;
        }
        if (output->frameInfoFile.isOpen()) {
            output->frameInfoFile.flush();
            output->frameInfoFile.close();
        }
    }
    m_writerState.cameraOutputs.clear();

    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        m_writerState.writeQueue.clear();
        m_writerState.pendingWriteBytes = 0;
        m_writerState.writerStopRequested = false;
    }
    emitBufferUsageChanged(0);
    emitWriterStatus();
}

void RecordingManager::writerLoop()
{
    // Drain queued frames on the writer thread
    while (true) {
        WriteTask task;
        {
            std::unique_lock<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.writeCondition.wait(lock, [this]() {
                return m_writerState.writerStopRequested || !m_writerState.writeQueue.empty();
            });
            if (m_writerState.writeQueue.empty()) {
                if (m_writerState.writerStopRequested) {
                    break;
                }
                continue;
            }
            task = std::move(m_writerState.writeQueue.front());
            m_writerState.writeQueue.pop_front();
        }

        QString errorMessage;
        if (!writeTask(task, errorMessage)) {
            {
                std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
                if (m_writerState.writerError.isEmpty()) {
                    m_writerState.writerError = errorMessage.isEmpty()
                        ? QStringLiteral("Unknown recording writer error")
                        : errorMessage;
                }
                m_writerState.writerStopRequested = true;
                m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
            }
            emitWriterStatus();
            QMetaObject::invokeMethod(this, [this]() {
                if (m_captureState.isRecording) {
                    stop();
                }
            }, Qt::QueuedConnection);
            break;
        }

        qint64 pendingWriteBytes = 0;
        {
            std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
            m_writerState.pendingWriteBytes -= static_cast<size_t>(task.frame.rawData.size());
            m_writerState.status.addWrittenFrames(1);
            pendingWriteBytes = static_cast<qint64>(m_writerState.pendingWriteBytes);
        }
        emitBufferUsageChanged(pendingWriteBytes);
        if (m_sessionState.activeSession) {
            const qint64 framesWritten =
                m_sessionState.activeSession->ensureFileManifest(task.cameraId).framesWritten + 1;
            m_sessionState.activeSession->setOutputFramesWritten(task.cameraId, framesWritten);
        }
        emitWriterStatus();
    }
}

bool RecordingManager::writeTask(const WriteTask& task, QString& errorMessage)
{
    const auto it = m_writerState.cameraOutputs.constFind(task.cameraId);
    if (it == m_writerState.cameraOutputs.constEnd() || !it.value()) {
        errorMessage = QStringLiteral("Missing output for %1").arg(task.cameraId);
        return false;
    }

    const auto output = it.value();
    auto* backend = reinterpret_cast<SaveBackend*>(output->backend);
    if (!backend) {
        SaveBackend::TiffOptions tiffOpts;
        tiffOpts.useDeflate = m_captureState.enableCompression;
        tiffOpts.zipQuality = m_captureState.compressionLevel;

        auto newBackend = std::make_unique<SaveBackend>();
        if (!newBackend->startStackRaw(output->rawPath,
                                       m_captureState.format,
                                       task.frame.width,
                                       task.frame.height,
                                       task.frame.bits,
                                       tiffOpts)) {
            errorMessage = QStringLiteral("Failed to open raw output for %1: %2")
                .arg(task.cameraId)
                .arg(newBackend->lastError());
            return false;
        }
        output->backend = newBackend.release();
        output->width = task.frame.width;
        output->height = task.frame.height;
        output->bits = task.frame.bits;
        backend = reinterpret_cast<SaveBackend*>(output->backend);
    } else if (task.frame.width != output->width
               || task.frame.height != output->height
               || task.frame.bits != output->bits) {
        errorMessage = QStringLiteral("Frame format changed during recording for %1").arg(task.cameraId);
        return false;
    }

    if (!backend->appendRaw(reinterpret_cast<const uchar*>(task.frame.rawData.constData()),
                            task.frame.rawData.size(),
                            buildImageDescriptionJson(output->metadataFileName))) {
        errorMessage = QStringLiteral("Failed writing raw frame for %1: %2")
            .arg(task.cameraId)
            .arg(backend->lastError());
        return false;
    }

    if (output->frameInfoFile.isOpen()) {
        const QByteArray infoLine = buildFrameInfoLine(task.cameraId, task.frame);
        if (output->frameInfoFile.write(infoLine) != infoLine.size()) {
            errorMessage = QStringLiteral("Failed writing frame info for %1").arg(task.cameraId);
            return false;
        }
    }
    return true;
}

void RecordingManager::writeFrameInfoHeader(CameraOutput& output)
{
    static const QByteArray header(
        "camera_id,frame_index,timestamp_ns,width,height,bits_per_sample,stride,pixel_format,pixel_format_id,raw_bytes\n");
    output.frameInfoFile.write(header);
}

QByteArray RecordingManager::buildFrameInfoLine(const QString& cameraId, const RecordingFrame& frame) const
{
    return QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9,%10\n")
        .arg(cameraId)
        .arg(frame.header.frameIndex)
        .arg(frame.header.timestampNs)
        .arg(frame.width)
        .arg(frame.height)
        .arg(frame.bits)
        .arg(frame.header.stride)
        .arg(pixelFormatName(frame.header.pixelFormat))
        .arg(frame.header.pixelFormat)
        .arg(frame.rawData.size())
        .toUtf8();
}

QString RecordingManager::formatName(RecordingFormat format) const
{
    return recordingFormatName(format);
}

bool RecordingManager::start(const Settings& settings, const QStringList& activeCameraIds)
{
    if (m_captureState.isRecording) {
        qWarning().noquote() << "Recording already running";
        return false;
    }

    CapturePlan plan;
    QString errorMessage;
    if (!buildCapturePlan(settings, activeCameraIds, plan, errorMessage)) {
        qWarning().noquote() << errorMessage;
        return false;
    }

    resetCaptureState(plan);
    if (!plan.useMda) {
        primeLastFrameIndices();
    }
    resetSessionState(plan);

    if (!startStreamingOutputs(plan)) {
        qWarning().noquote() << (m_writerState.writerError.isEmpty() ? QStringLiteral("Failed to start streaming outputs")
                                                         : m_writerState.writerError);
        m_sessionState.activeSession.reset();
        return false;
    }

    m_captureState.elapsedTimer.start();
    m_captureState.isRecording = true;
    m_mdaState.usingMda = plan.useMda;
    m_mdaState.streamMda = plan.streamMda;
    m_mdaState.streamIntervalMs = plan.streamMda ? plan.mdaIntervalMs : 0.0;
    m_mdaState.lastStreamCaptureMs = plan.streamMda ? static_cast<qint64>(-m_mdaState.streamIntervalMs) : 0;
    m_captureState.phase = plan.useMda
                  ? kRecordingPhaseRecordingMda
                  : (m_captureState.burstMode ? kRecordingPhaseRecordingBurst : kRecordingPhaseRecording);
    emit recordingStateChanged(true);
    emitProgress();

    qInfo().noquote() << QString("Recording started (%1 camera(s))").arg(m_captureState.activeCameraIds.size());

    if (plan.useMda) {
        if (!startMdaCapture()) {
            stop();
            return false;
        }
    }
    return true;
}

void RecordingManager::stop()
{
    if (!m_captureState.isRecording) return;
    if (m_mdaState.usingMda && m_mdaState.manager && m_mdaState.manager->isRunning()) {
        m_mdaState.manager->requestCancel();
    }
    if (m_mpcm) {
        m_mpcm->setPollingPaused(false);
    }

    setWriterStatus(RecordingWriterPhase::Stopping);
    stopStreamingOutputs();

    m_captureState.isRecording = false;
    m_captureState.phase = kRecordingPhaseStopped;
    emit recordingStateChanged(false);
    emitProgress();

    qInfo().noquote() << "Recording stopped";

    auto session = m_sessionState.activeSession;
    finalizeActiveSession();
    if (m_writerState.writerError.isEmpty()) {
        setWriterStatus(RecordingWriterPhase::Completed);
    } else {
        setWriterStatus(RecordingWriterPhase::Failed, m_writerState.writerError);
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

void RecordingManager::onNewRawFrameReady(const QString& cameraId,
                                          const SharedFrameHeader& header,
                                          const QByteArray& rawData)
{
    ingestFrame(FramePacket{cameraId, header, rawData, FramePacket::Source::PreviewStream});
}

void RecordingManager::primeLastFrameIndices()
{
    m_captureState.lastFrameIndex.clear();
    for (const QString& cameraId : m_captureState.activeCameraIds) {
        SharedFrameHeader header{};
        QByteArray rawData;
        bool ok = false;
        if (m_latestFrameFetcher) {
            ok = m_latestFrameFetcher(cameraId, header, rawData);
        } else if (m_mpcm) {
            ok = m_mpcm->getLatestRaw(cameraId, header, rawData);
        }
        if (ok) {
            m_captureState.lastFrameIndex[cameraId] = header.frameIndex;
        } else {
            m_captureState.lastFrameIndex[cameraId] = 0;
        }
    }
}

void RecordingManager::emitProgress()
{
    qint64 frameCurrent = 0;
    const int burstCurrent = m_captureState.burstMode ? m_captureState.currentBurst : 0;
    const int burstTarget = m_captureState.burstMode ? m_captureState.targetBursts : 0;

    if (!m_captureState.activeCameraIds.isEmpty()) {
        qint64 minFrames = (std::numeric_limits<qint64>::max)();
        for (const QString& cameraId : m_captureState.activeCameraIds) {
            const qint64 count = m_captureState.framesCapturedThisBurst.value(cameraId, 0);
            minFrames = (std::min)(minFrames, count);
        }
        frameCurrent = minFrames;
    }

    const int tCount = (m_mdaState.timePoints > 0) ? m_mdaState.timePoints : 1;
    const int zCount = m_mdaState.zPositions.empty() ? 1 : static_cast<int>(m_mdaState.zPositions.size());
    const int xyCount = m_mdaState.positions.empty() ? 1 : static_cast<int>(m_mdaState.positions.size());
    const qint64 frameTarget = m_mdaState.usingMda ? static_cast<qint64>(tCount) * zCount * xyCount
                                          : m_captureState.framesPerBurst;

    int mdaTimeIndex = 0;
    int mdaZIndex = 0;
    int mdaPositionIndex = 0;
    bool hasXY = false;
    double x = 0.0;
    double y = 0.0;
    bool hasZ = false;
    double z = 0.0;
    if (m_mdaState.usingMda && m_mdaState.hasLastEvent) {
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
    if (m_captureState.waitingBetweenBursts) {
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

bool RecordingManager::enqueueFrame(const RecordingFrame& frame, const QString& cameraId)
{
    // Stop capture if the writer queue grows too large
    const size_t frameBytes = static_cast<size_t>(frame.rawData.size());
    qint64 pendingWriteBytes = 0;
    {
        std::lock_guard<std::mutex> lock(m_writerState.writeMutex);
        if (m_writerState.pendingWriteBytes + frameBytes > m_writerState.recordedMaxBytes) {
            if (m_writerState.writerError.isEmpty()) {
                m_writerState.writerError = QStringLiteral("Recording write queue exceeded limit");
            }
            m_writerState.status.setPhase(RecordingWriterPhase::Failed, m_writerState.writerError);
            qWarning().noquote() << "Recording write queue full, stopping capture";
            QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
            emitWriterStatus();
            return false;
        }
        m_writerState.writeQueue.push_back(WriteTask{cameraId, frame});
        m_writerState.pendingWriteBytes += frameBytes;
        pendingWriteBytes = static_cast<qint64>(m_writerState.pendingWriteBytes);
    }
    emitBufferUsageChanged(pendingWriteBytes);
    m_writerState.writeCondition.notify_one();
    emitWriterStatus();
    return true;
}

bool RecordingManager::shouldAcceptFrame(const FramePacket& packet) const
{
    if (!m_captureState.isRecording) return false;
    if (!m_captureState.activeCameraIds.contains(packet.cameraId)) return false;
    if (m_mdaState.usingMda && packet.source != FramePacket::Source::Mda) return false;
    if (!m_sessionState.activeSession || packet.rawData.isEmpty()) return false;
    return packet.header.frameIndex > m_captureState.lastFrameIndex.value(packet.cameraId, 0);
}

void RecordingManager::ingestFrame(const FramePacket& packet)
{
    // Drop duplicates and enforce burst timing
    if (!shouldAcceptFrame(packet)) return;

    if (m_captureState.waitingBetweenBursts) {
        const qint64 waited = m_captureState.elapsedTimer.elapsed() - m_captureState.lastBurstEndMs;
        if (waited < static_cast<qint64>(m_captureState.burstIntervalMs)) {
            emitProgress();
            return;
        }
        m_captureState.waitingBetweenBursts = false;
        m_captureState.phase = m_mdaState.usingMda
                      ? kRecordingPhaseRecordingMda
                      : (m_captureState.burstMode ? kRecordingPhaseRecordingBurst : kRecordingPhaseRecording);
        if (m_mdaState.streamMda && m_mdaState.streamIntervalMs > 0.0) {
            m_mdaState.lastStreamCaptureMs = m_captureState.elapsedTimer.elapsed() - static_cast<qint64>(m_mdaState.streamIntervalMs);
        }
    }

    if (m_mdaState.streamMda && m_mdaState.streamIntervalMs > 0.0) {
        const qint64 now = m_captureState.elapsedTimer.elapsed();
        const qint64 intervalMs = static_cast<qint64>(m_mdaState.streamIntervalMs);
        if (intervalMs > 0 && (now - m_mdaState.lastStreamCaptureMs) < intervalMs) {
            return;
        }
        m_mdaState.lastStreamCaptureMs = now;
    }

    if (!m_mdaState.usingMda && !shouldCaptureCamera(packet.cameraId)) {
        return;
    }

    if (!m_writerState.writerError.isEmpty()) {
        stop();
        return;
    }

    RecordingFrame frame;
    frame.header = packet.header;
    frame.rawData = packet.rawData;
    frame.width = static_cast<int>(packet.header.width);
    frame.height = static_cast<int>(packet.header.height);
    frame.bits = resolveFrameBits(packet.header);
    if (frame.width <= 0 || frame.height <= 0 || frame.bits <= 0) {
        qWarning().noquote() << QStringLiteral("Skipping invalid frame for %1").arg(packet.cameraId);
        return;
    }

    if (!enqueueFrame(frame, packet.cameraId)) {
        return;
    }
    m_captureState.lastFrameIndex[packet.cameraId] = packet.header.frameIndex;

    m_captureState.framesCapturedThisBurst[packet.cameraId] += 1;
    m_captureState.framesCapturedTotal[packet.cameraId] += 1;
    emitProgress();

    (void)advanceBurstStateIfNeeded();
}

void RecordingManager::handleMdaFrame(const MDAOutput& frame)
{
    if (!m_captureState.isRecording || !m_mdaState.usingMda) return;

    m_mdaState.lastEvent = frame.event;
    m_mdaState.hasLastEvent = true;

    if (!frame.frames.isEmpty()) {
        for (auto it = frame.frames.constBegin(); it != frame.frames.constEnd(); ++it) {
            const QString& cameraId = it.key();
            const CameraFrame& camFrame = it.value();
            if (!m_captureState.activeCameraIds.contains(cameraId)) {
                continue;
            }

            SharedFrameHeader header{};
            header.state = 2;
            header.width = static_cast<quint32>(camFrame.width);
            header.height = static_cast<quint32>(camFrame.height);
            header.bitsPerSample = static_cast<quint16>(camFrame.bitsPerSample);
            header.channels = 1;
            header.pixelFormat =
                (camFrame.bitsPerSample > 8)
                    ? static_cast<quint32>(SharedPixelFormat::Mono16)
                    : static_cast<quint32>(SharedPixelFormat::Mono8);
            const int bytesPerPixel =
                (camFrame.bytesPerPixel > 0) ? camFrame.bytesPerPixel : (camFrame.bitsPerSample > 8 ? 2 : 1);
            header.stride = static_cast<quint32>(camFrame.width * bytesPerPixel);
            header.frameIndex = m_captureState.lastFrameIndex.value(cameraId, 0) + 1;
            header.timestampNs = static_cast<quint64>(frame.timestampMs) * 1000000ull;

            ingestFrame(FramePacket{cameraId, header, camFrame.raw, FramePacket::Source::Mda});
        }
        return;
    }

    if (m_mdaState.cameraId.isEmpty()) return;

    SharedFrameHeader header{};
    header.state = 2;
    header.width = static_cast<quint32>(frame.width);
    header.height = static_cast<quint32>(frame.height);
    header.bitsPerSample = static_cast<quint16>(frame.bitsPerSample);
    header.channels = 1;
    header.pixelFormat =
        (frame.bitsPerSample > 8)
            ? static_cast<quint32>(SharedPixelFormat::Mono16)
            : static_cast<quint32>(SharedPixelFormat::Mono8);
    const int bytesPerPixel = (frame.bytesPerPixel > 0) ? frame.bytesPerPixel : (frame.bitsPerSample > 8 ? 2 : 1);
    header.stride = static_cast<quint32>(frame.width * bytesPerPixel);
    header.frameIndex = ++m_mdaState.frameIndex;
    header.timestampNs = static_cast<quint64>(frame.timestampMs) * 1000000ull;

    ingestFrame(FramePacket{m_mdaState.cameraId, header, frame.raw, FramePacket::Source::Mda});
}

bool RecordingManager::startMdaCapture()
{
    if (!m_mmcore) {
        qWarning().noquote() << "MMCore not available for MDA";
        return false;
    }
    if (m_captureState.activeCameraIds.isEmpty()) {
        qWarning().noquote() << "MDA requires at least one camera";
        return false;
    }
    if (m_captureState.activeCameraIds.size() > 1 && !m_mpcm) {
        qWarning().noquote() << "Multi-camera MDA requires MultiProcessCameraManager";
        return false;
    }
    if (m_captureState.activeCameraIds.size() > 1 && m_mpcm) {
        m_mpcm->setPollingPaused(true);
    }

    m_mdaState.cameraId = m_captureState.activeCameraIds.first();
    m_mdaState.frameIndex = 0;
    m_mdaState.usingMda = true;
    m_captureState.lastFrameIndex.clear();
    for (const QString& cameraId : m_captureState.activeCameraIds) {
        m_captureState.lastFrameIndex[cameraId] = 0;
    }

    m_mdaState.burstsRemaining = m_captureState.burstMode ? m_captureState.targetBursts : 1;

    if (!m_mdaState.manager) {
        m_mdaState.manager = new MDAManager(m_mmcore, this);
    }
    if (m_captureState.activeCameraIds.size() > 1) {
        m_mdaState.manager->setCameras(m_captureState.activeCameraIds);
    } else {
        m_mdaState.manager->setCameras({});
    }
    m_mdaState.manager->setMultiProcessCameraManager(m_mpcm);
    if (!m_mdaState.signalsConnected) {
        connect(m_mdaState.manager, &MDAManager::frameReady, this, [this](const MDAOutput& frame) {
            handleMdaFrame(frame);
        });
        connect(m_mdaState.manager, &MDAManager::sequenceFinished, this, [this]() {
            if (m_mdaState.usingMda && m_captureState.isRecording) {
                m_mdaState.burstsRemaining -= 1;
                if (m_mdaState.burstsRemaining > 0) {
                    m_captureState.waitingBetweenBursts = true;
                    m_captureState.lastBurstEndMs = m_captureState.elapsedTimer.elapsed();
                    m_captureState.phase = kRecordingPhaseWaitingNextBurst;
                    emitProgress();
                    const int waitMs = static_cast<int>(m_captureState.burstIntervalMs);
                    QTimer::singleShot(waitMs, this, [this]() { startMdaRun(); });
                } else {
                    stop();
                }
            }
        });
        connect(m_mdaState.manager, &MDAManager::sequenceCanceled, this, [this]() {
            if (m_mdaState.usingMda && m_captureState.isRecording) {
                stop();
            }
        });
        connect(m_mdaState.manager, &MDAManager::sequenceError, this, [this](const QString& message) {
            qWarning().noquote() << QString("MDA error: %1").arg(message);
            if (m_mdaState.usingMda && m_captureState.isRecording) {
                stop();
            }
        });
        m_mdaState.signalsConnected = true;
    }

    startMdaRun();
    return true;
}

void RecordingManager::startMdaRun()
{
    if (!m_captureState.isRecording || !m_mdaState.usingMda) {
        return;
    }
    if (m_mdaState.burstsRemaining <= 0) {
        stop();
        return;
    }

    for (const QString& cameraId : m_captureState.activeCameraIds) {
        m_captureState.framesCapturedThisBurst[cameraId] = 0;
    }

    m_captureState.waitingBetweenBursts = false;
    m_captureState.currentBurst += 1;
    m_captureState.phase = kRecordingPhaseRecordingMda;
    emitProgress();

    m_mdaState.manager->start(m_mdaState.timePoints, m_mdaState.intervalMs, 0.0, m_mdaState.positions, m_mdaState.zPositions, m_mdaState.order, false);
}

bool RecordingManager::shouldCaptureCamera(const QString& cameraId) const
{
    return m_captureState.framesCapturedThisBurst.value(cameraId, 0) < m_captureState.framesPerBurst;
}

bool RecordingManager::allCamerasReachedTarget() const
{
    if (m_captureState.activeCameraIds.isEmpty()) return true;
    for (const QString& cameraId : m_captureState.activeCameraIds) {
        if (m_captureState.framesCapturedThisBurst.value(cameraId, 0) < m_captureState.framesPerBurst) {
            return false;
        }
    }
    return true;
}

bool RecordingManager::advanceBurstStateIfNeeded()
{
    if (m_mdaState.usingMda) {
        return false;
    }
    if (!m_captureState.burstMode) {
        if (allCamerasReachedTarget()) {
            stop();
            return true;
        }
        return false;
    }

    if (!allCamerasReachedTarget()) {
        return false;
    }

    m_captureState.currentBurst += 1;
    m_captureState.phase = kRecordingPhaseWaitingNextBurst;
    emitProgress();

    if (m_captureState.currentBurst >= m_captureState.targetBursts) {
        stop();
        return true;
    }

    for (const QString& cameraId : m_captureState.activeCameraIds) {
        m_captureState.framesCapturedThisBurst[cameraId] = 0;
    }

    m_captureState.waitingBetweenBursts = true;
    m_captureState.lastBurstEndMs = m_captureState.elapsedTimer.elapsed();
    return false;
}

QString RecordingManager::saveSessionToDisk(const std::shared_ptr<RecordingSessionData>& session)
{
    // Save buffered sessions that were not streamed live
    if (!session) {
        return QStringLiteral("Error: Missing recording session");
    }
    if (session->isSaved()) {
        return session->saveMessage().isEmpty()
            ? QStringLiteral("Success: Recording was already saved during acquisition")
            : session->saveMessage();
    }
    const auto& capturePlan = session->capturePlan();
    if (capturePlan.cameraIds.isEmpty()) {
        return updateSessionResult(session, QStringLiteral("Error: No cameras to save"), false);
    }

    if (!session->hasAnyFrames()) {
        return updateSessionResult(session, QStringLiteral("Error: No frames captured"), false);
    }

    if (!QDir().mkpath(capturePlan.saveDir)) {
        return updateSessionResult(
            session,
            QStringLiteral("Error: Failed to create save directory: %1").arg(capturePlan.saveDir),
            false);
    }

    const QString metadataFileName = metadataFileNameForPlan(capturePlan.baseName, capturePlan.metadataFileName);
    const QString metadataFilePath = QDir(capturePlan.saveDir).filePath(metadataFileName);
    QString metadataErrorMessage;
    if (!writeSessionMetadataFile(metadataFilePath, capturePlan.sessionMetadataJson, metadataErrorMessage)) {
        return updateSessionResult(session, QStringLiteral("Error: %1").arg(metadataErrorMessage), false);
    }

    session->prepareForSave(session->streamedToDisk());
    session->setWriterPhase(RecordingWriterPhase::Starting);
    for (const QString& cameraId : session->cameraIds()) {
        const auto* frames = session->framesForCamera(cameraId);
        if (!frames || frames->empty()) {
            continue;
        }
        session->setWriterPhase(RecordingWriterPhase::Writing);
        SaveBackend rawSaver;
        SaveBackend::TiffOptions tiffOpts;
        tiffOpts.useDeflate = capturePlan.enableCompression;
        tiffOpts.zipQuality = capturePlan.compressionLevel;

        const RecordingFrame& first = frames->front();
        const QString rawPath = buildSessionFilePath(capturePlan.saveDir,
                                                     capturePlan.baseName,
                                                     cameraId,
                                                     recordingExtension(capturePlan.format));
        if (!rawSaver.startStackRaw(rawPath, capturePlan.format, first.width, first.height, first.bits, tiffOpts)) {
            const QString errorMessage = QString("Failed to start raw output for %1").arg(cameraId);
            session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
            return updateSessionResult(
                session,
                QStringLiteral("Error: %1").arg(errorMessage),
                false);
        }
        int saved = 0;
        for (const RecordingFrame& frame : *frames) {
            if (!rawSaver.appendRaw(reinterpret_cast<const uchar*>(frame.rawData.constData()),
                                    frame.rawData.size(),
                                    buildImageDescriptionJson(metadataFileName))) {
                rawSaver.stopStack();
                const QString errorMessage = QString("Failed to append raw frame %1 for %2").arg(saved).arg(cameraId);
                session->setWriterPhase(RecordingWriterPhase::Failed, errorMessage);
                return updateSessionResult(
                    session,
                    QStringLiteral("Error: %1").arg(errorMessage),
                    false);
            }
            saved += 1;
        }
        rawSaver.stopStack();
        session->setOutputFilePaths(cameraId, rawPath, QString());
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
