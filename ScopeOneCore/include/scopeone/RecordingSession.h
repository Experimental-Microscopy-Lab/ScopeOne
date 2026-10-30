#pragma once

#include "scopeone/ExperimentDocument.h"
#include "scopeone/ImageFrame.h"
#include "scopeone/scopeone_core_export.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>
#include <vector>

namespace scopeone::core
{
    class ScopeOneCore;

    namespace internal
    {
        class RecordingManager;
    }

    // The frames and the document of one recording, and how far writing them has come
    enum class RecordingWriterPhase
    {
        Idle,
        Starting,
        Writing,
        Stopping,
        Completed,
        Failed
    };

    class RecordingWriterStatus
    {
    public:
        RecordingWriterPhase phase() const { return m_phase; }
        qint64 pendingWriteBytes() const { return m_pendingWriteBytes; }
        qint64 maxPendingWriteBytes() const { return m_maxPendingWriteBytes; }
        qint64 framesCaptured() const { return m_framesCaptured; }
        qint64 framesWritten() const { return m_framesWritten; }
        qint64 droppedFrames() const { return m_droppedFrames; }
        qint64 bytesWritten() const { return m_bytesWritten; }
        const QString& errorMessage() const { return m_errorMessage; }

        void reset(qint64 maxPendingWriteBytes = 0)
        {
            *this = RecordingWriterStatus{};
            m_maxPendingWriteBytes = maxPendingWriteBytes;
        }

        void setPhase(RecordingWriterPhase phase, const QString& errorMessage = QString())
        {
            m_phase = phase;
            m_errorMessage = errorMessage;
        }

        void setPendingWriteBytes(qint64 pendingWriteBytes)
        {
            m_pendingWriteBytes = pendingWriteBytes;
        }

        void setMaxPendingWriteBytes(qint64 maxPendingWriteBytes)
        {
            m_maxPendingWriteBytes = maxPendingWriteBytes;
        }

        void setFramesCaptured(qint64 framesCaptured)
        {
            m_framesCaptured = framesCaptured;
        }

        void addWrittenFrames(qint64 framesWritten)
        {
            m_framesWritten += framesWritten;
        }

        void addDroppedFrames(qint64 droppedFrames)
        {
            m_droppedFrames += droppedFrames;
        }

        void addWrittenBytes(qint64 bytesWritten)
        {
            m_bytesWritten += bytesWritten;
        }

    private:
        RecordingWriterPhase m_phase{RecordingWriterPhase::Idle};
        qint64 m_pendingWriteBytes{0};
        qint64 m_maxPendingWriteBytes{0};
        qint64 m_framesCaptured{0};
        qint64 m_framesWritten{0};
        qint64 m_droppedFrames{0};
        qint64 m_bytesWritten{0};
        QString m_errorMessage;
    };

    class SCOPEONE_CORE_EXPORT RecordingSessionData
    {
    public:
        const QStringList& cameraIds() const { return m_manifest.plan.cameraIds; }
        const ExperimentPlan& capturePlan() const { return m_manifest.plan; }
        const ExperimentDocument& experimentDocument() const { return m_manifest; }
        ExperimentRunState runState() const { return m_manifest.runState; }
        bool streamedToDisk() const { return m_manifest.output.streamedToDisk; }
        double cameraPixelSizeUm(const QString& cameraId) const
        {
            return m_cameraPixelSizesUm.value(cameraId.trimmed(), 0.0);
        }
        // Sets how the planes of the images are arranged along the channel, Z and time axes
        void setStackShape(int channels, int slices, int frames, const QString& order)
        {
            m_manifest.plan.stackChannels = channels;
            m_manifest.plan.stackSlices = slices;
            m_manifest.plan.stackFrames = frames;
            m_manifest.plan.stackOrder = order;
        }
        // Sets the size of a pixel of the images of a camera in this session, in micrometers
        void setPixelSizeUm(const QString& cameraId, double pixelSizeUm)
        {
            m_cameraPixelSizesUm.insert(cameraId.trimmed(), pixelSizeUm);
        }

        QStringList recordedCameraIds() const
        {
            QStringList ids;
            for (const QString& cameraId : m_manifest.plan.cameraIds)
            {
                const QString trimmedCameraId = cameraId.trimmed();
                if (!trimmedCameraId.isEmpty() && !ids.contains(trimmedCameraId))
                {
                    ids.append(trimmedCameraId);
                }
            }
            for (auto it = m_frames.constBegin(); it != m_frames.constEnd(); ++it)
            {
                if (!it.value().empty() && !ids.contains(it.key()))
                {
                    ids.append(it.key());
                }
            }
            for (auto it = m_manifest.output.files.constBegin(); it != m_manifest.output.files.constEnd(); ++it)
            {
                const QString trimmedCameraId = it.key().trimmed();
                if (!trimmedCameraId.isEmpty() && !ids.contains(trimmedCameraId))
                {
                    ids.append(trimmedCameraId);
                }
            }
            return ids;
        }

        const QHash<QString, RecordingFileManifest>& outputFiles() const
        {
            return m_manifest.output.files;
        }

        qint64 outputFrameCount(const QString& cameraId) const
        {
            const auto it = m_manifest.output.files.constFind(cameraId.trimmed());
            return it == m_manifest.output.files.constEnd() ? 0 : it.value().framesWritten;
        }

        qint64 recordedFrameCount(const QString& cameraId) const
        {
            const int bufferedFrames = frameCount(cameraId);
            return bufferedFrames > 0 ? bufferedFrames : outputFrameCount(cameraId);
        }

        qint64 recordedFrameCount() const
        {
            qint64 total = 0;
            for (const QString& cameraId : recordedCameraIds())
            {
                total += recordedFrameCount(cameraId);
            }
            return total;
        }

        bool hasRecordedOutput() const
        {
            return recordedFrameCount() > 0;
        }

        // One recorded frame, from memory or from the files that were written. Invalid when
        // there is no such frame.
        ImageFrame recordedFrame(const QString& cameraId, int index) const
        {
            return imageFrameAt(cameraId, index);
        }

        bool isSaved() const { return m_saved; }
        const QString& saveMessage() const { return m_saveMessage; }

    private:
        friend class ScopeOneCore;
        friend class internal::RecordingManager;

        int frameCount(const QString& cameraId) const
        {
            const auto it = m_frames.constFind(cameraId.trimmed());
            return it == m_frames.constEnd() ? 0 : static_cast<int>(it.value().size());
        }
        bool hasAnyFrames() const
        {
            return !m_frames.isEmpty();
        }
        void setSaveResult(bool saved, const QString& message)
        {
            m_saved = saved;
            m_saveMessage = message;
        }
        void setWriterPhase(RecordingWriterPhase phase, const QString& errorMessage = QString())
        {
            m_writerStatus.setPhase(phase, errorMessage);
        }
        void addWrittenFrames(qint64 framesWritten)
        {
            m_writerStatus.addWrittenFrames(framesWritten);
        }
        void setWriterStatusSnapshot(const RecordingWriterStatus& status)
        {
            m_writerStatus = status;
        }
        void prepareForSave(bool streamedToDisk, qint64 maxPendingWriteBytes = 0)
        {
            m_manifest.output.streamedToDisk = streamedToDisk;
            clearOutputFiles();
            m_saved = false;
            m_saveMessage.clear();
            m_writerStatus.reset(maxPendingWriteBytes);
        }
        void setCapturePlan(const ExperimentPlan& plan)
        {
            m_manifest.plan = plan;
            QStringList cameraIds;
            for (const QString& cameraId : m_manifest.plan.cameraIds)
            {
                const QString trimmedCameraId = cameraId.trimmed();
                if (!trimmedCameraId.isEmpty() && !cameraIds.contains(trimmedCameraId))
                {
                    cameraIds.append(trimmedCameraId);
                }
            }
            m_manifest.plan.cameraIds = cameraIds;
        }
        void setSoftwareSnapshot(const SoftwareSnapshot& software) { m_manifest.software = software; }
        void setDeviceProperties(const QJsonObject& properties) { m_manifest.deviceProperties = properties; }
        void setCameraPixelSizesUm(const QHash<QString, double>& pixelSizesUm)
        {
            m_cameraPixelSizesUm = pixelSizesUm;
        }
        void setRunState(ExperimentRunState state,
                         quint64 completedTimestampNs = 0,
                         const QString& errorMessage = QString())
        {
            m_manifest.runState = state;
            m_manifest.completedTimestampNs = completedTimestampNs;
            m_manifest.errorMessage = errorMessage;
        }
        void setStartedTimestampNs(quint64 timestampNs) { m_manifest.startedTimestampNs = timestampNs; }
        void appendEventRecord(const AcquisitionEventRecord& record) { m_manifest.events.append(record); }
        void setPresentationState(const QList<DocumentLayer>& layers,
                                  const QList<DocumentMarkup>& markups)
        {
            m_manifest.layers = layers;
            m_manifest.markups = markups;
        }
        ImageFrame imageFrameAt(const QString& cameraId, int index) const;
        bool appendImageFrame(const ImageFrame& frame)
        {
            const QString cameraId = frame.cameraId.trimmed();
            if (!frame.isValid() || cameraId.isEmpty())
            {
                return false;
            }
            ImageFrame storedFrame(frame);
            storedFrame.cameraId = cameraId;
            m_frames[cameraId].push_back(std::move(storedFrame));
            if (!m_manifest.plan.cameraIds.contains(cameraId))
            {
                m_manifest.plan.cameraIds.append(cameraId);
            }
            return true;
        }
        static std::shared_ptr<RecordingSessionData> fromImageFrames(
            const QList<ImageFrame>& frames,
            const ExperimentPlan& capturePlan);
        void clearOutputFiles() { m_manifest.clearOutput(); }
        RecordingFileManifest& ensureFileManifest(const QString& cameraId)
        {
            return m_manifest.ensureFile(cameraId.trimmed());
        }
        void setOutputFilePaths(const QString& cameraId, const QString& rawPath, const QString& frameInfoPath)
        {
            auto& fileManifest = ensureFileManifest(cameraId);
            fileManifest.rawPath = rawPath;
            fileManifest.frameInfoPath = frameInfoPath;
        }
        void setOutputFramesWritten(const QString& cameraId, qint64 framesWritten)
        {
            ensureFileManifest(cameraId).framesWritten = framesWritten;
        }
        std::shared_ptr<RecordingSessionData> cloneForSave() const
        {
            return std::make_shared<RecordingSessionData>(*this);
        }
        void applySaveStateFrom(const RecordingSessionData& source)
        {
            m_saved = source.m_saved;
            m_saveMessage = source.m_saveMessage;
            m_writerStatus = source.m_writerStatus;
            if (source.m_saved)
            {
                m_manifest.plan = source.m_manifest.plan;
                m_manifest.output = source.m_manifest.output;
            }
        }

        ImageFrame outputImageFrameAt(const QString& cameraId, int index) const;
        const ImageFrame* frameAt(const QString& cameraId, int index) const;

        ExperimentDocument m_manifest;
        QHash<QString, std::vector<ImageFrame>> m_frames;
        QHash<QString, double> m_cameraPixelSizesUm;
        bool m_saved{false};
        QString m_saveMessage;
        RecordingWriterStatus m_writerStatus;
    };
}
