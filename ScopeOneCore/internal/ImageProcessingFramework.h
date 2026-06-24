#pragma once

#include <QObject>
#include <QMutex>
#include <QThreadPool>
#include <QHash>
#include <functional>
#include <memory>
#include <vector>
#include <atomic>
#include "internal/ProcessingModule.h"
#include "internal/SpatiotemporalBinningModule.h"
#include "internal/GaussianBlurModule.h"
#include "internal/FFTModule.h"
#include "internal/BackgroundCalibrationModule.h"
#include "internal/DifferentialRollingModule.h"

namespace scopeone::core::internal
{
    class ProcessingPipeline : public QObject
    {
        Q_OBJECT

    public:
        explicit ProcessingPipeline(QObject* parent = nullptr);

        void addModule(std::unique_ptr<ProcessingModule> module);
        bool removeModule(int index);
        std::shared_ptr<ProcessingPipeline> clone(QObject* parent = nullptr) const;
        void forEachModule(const std::function<void(const ProcessingModule *)>& visitor) const;
        bool withModule(int index, const std::function<bool(ProcessingModule *)>& visitor);

        ImageFrame process(const ImageFrame& input, int processingBitDepth);

        int getModuleCount() const;

    private:
        std::vector<std::unique_ptr<ProcessingModule>> m_modules;
        mutable QMutex m_modulesMutex;
    };

    class ImageProcessingManager : public QObject
    {
        Q_OBJECT

    public:
        explicit ImageProcessingManager(QObject* parent = nullptr);
        ~ImageProcessingManager();

        ProcessingPipeline* pipeline() const;

        void enableRealTimeProcessing(bool enabled);
        bool isRealTimeProcessingEnabled() const { return m_realTimeEnabled; }
        int processingBitDepth() const { return m_processingBitDepth.load(); }
        void setProcessingBitDepth(int bitDepth);
        void clearRuntimePipelines();

        void processFrameAsync(const ImageFrame& frame);

    signals:
        void imageProcessed(const ImageFrame& frame);
        void processingError(const QString& error);

    private:
        struct CameraSlot
        {
            ImageFrame latestFrame;
            bool hasFrame{false};
            bool processing{false};
        };

        QString getCameraKey(const ImageFrame& frame) const;
        std::shared_ptr<ProcessingPipeline> pipelineForCamera(const QString& cameraKey);
        void processCameraQueue(const QString& cameraKey);
        std::shared_ptr<ProcessingPipeline> m_pipeline;
        QHash<QString, std::shared_ptr<ProcessingPipeline>> m_cameraPipelines;
        std::atomic<bool> m_realTimeEnabled;
        std::atomic<int> m_processingBitDepth{16};
        void submitFrame(const ImageFrame& frame);
        mutable QMutex m_frameMutex;
        QHash<QString, CameraSlot> m_cameraSlots;

        QThreadPool* m_threadPool;
    };
}
