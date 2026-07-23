#pragma once

#include <QObject>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>
#include <QHash>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include "internal/ProcessingModule.h"

namespace scopeone::core::internal
{
    class ProcessingPipelineRuntime
    {
    public:
        explicit ProcessingPipelineRuntime(std::vector<std::unique_ptr<ProcessingModule>> modules);

        ProcessingResult process(const ImageFrame& input, int processingBitDepth);
        ProcessingResult processFrom(int startModuleIndex, const ImageFrame& input, int processingBitDepth);
        ProcessingResult processThrough(int endModuleIndex, const ImageFrame& input, int processingBitDepth);

    private:
        ProcessingResult processRange(const ImageFrame& input,
                                      int processingBitDepth,
                                      int startModuleIndex,
                                      int endModuleIndexExclusive);

        std::vector<std::unique_ptr<ProcessingModule>> m_modules;
    };

    class ProcessingPipelineDefinition
    {
    public:
        void addModule(std::unique_ptr<ProcessingModule> module);
        bool removeModule(int index);
        std::shared_ptr<ProcessingPipelineRuntime> createRuntime() const;
        void forEachModule(const std::function<void(const ProcessingModule *)>& visitor) const;
        bool withModule(int index, const std::function<void(ProcessingModule *)>& visitor);
        int moduleCount() const;

    private:
        std::vector<std::unique_ptr<ProcessingModule>> m_modules;
        mutable QMutex m_modulesMutex;
    };

    class ImageProcessingManager : public QObject
    {
        Q_OBJECT

    public:
        explicit ImageProcessingManager(QObject* parent = nullptr);
        ~ImageProcessingManager() override;

        ProcessingPipelineDefinition& definition();

        void enableRealTimeProcessing(bool enabled);
        bool isRealTimeProcessingEnabled() const { return m_realTimeEnabled; }
        int processingBitDepth() const { return m_processingBitDepth.load(); }
        void setProcessingBitDepth(int bitDepth);
        void clearRuntimePipelines();

        void processFrameAsync(const ImageFrame& frame);
        ImageFrame processFrame(const ImageFrame& frame);
        ImageFrame processFrameFrom(int startModuleIndex, const ImageFrame& frame);
        ImageFrame processFrameThrough(int endModuleIndex, const ImageFrame& frame);

    signals:
        void imageProcessed(const ImageFrame& frame, quint64 completedFrameCount);
        void processingError(const QString& error);

    private:
        struct CameraSlot
        {
            ImageFrame latestFrame;
            quint64 latestFrameGeneration{0};
            bool hasFrame{false};
            bool processing{false};
            ImageFrame latestProcessedFrame;
            quint64 completedFrameCount{0};
        };

        QString getCameraKey(const ImageFrame& frame) const;
        std::shared_ptr<ProcessingPipelineRuntime> livePipelineForCamera(const QString& cameraKey);
        std::shared_ptr<ProcessingPipelineRuntime> offlinePipelineForCamera(const QString& cameraKey);
        std::shared_ptr<ProcessingPipelineRuntime> pipelineForCamera(
            QHash<QString, std::shared_ptr<ProcessingPipelineRuntime>>& pipelines,
            const QString& cameraKey);
        void processCameraQueue(const QString& cameraKey);
        void flushProcessedOutputs();
        ImageFrame frameFromResult(ProcessingResult result);

        ProcessingPipelineDefinition m_definition;
        QHash<QString, std::shared_ptr<ProcessingPipelineRuntime>> m_livePipelines;
        QHash<QString, std::shared_ptr<ProcessingPipelineRuntime>> m_offlinePipelines;
        std::atomic<bool> m_realTimeEnabled;
        std::atomic<int> m_processingBitDepth{16};
        mutable QMutex m_frameMutex;
        QHash<QString, CameraSlot> m_cameraSlots;
        quint64 m_liveGeneration{1};

        QThreadPool m_threadPool;
        QTimer m_outputTimer;
    };
}
