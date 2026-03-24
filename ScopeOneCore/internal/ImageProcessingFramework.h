#pragma once

#include <QObject>
#include <QMutex>
#include <QThreadPool>
#include <QHash>
#include <memory>
#include <vector>
#include <atomic>
#include "internal/ProcessingModule.h"
#include "internal/SpatiotemporalBinningModule.h"
#include "internal/GaussianBlurModule.h"
#include "internal/FFTModule.h"
#include "internal/MedianFilterModule.h"
#include "internal/BackgroundCalibrationModule.h"

namespace scopeone::core::internal {

class ProcessingPipeline : public QObject
{
    Q_OBJECT

public:
    explicit ProcessingPipeline(QObject* parent = nullptr);

    void addModule(std::unique_ptr<ProcessingModule> module);
    void removeModule(int index);

    ImageFrame process(const ImageFrame& input);

    int getModuleCount() const { return static_cast<int>(m_modules.size()); }
    ProcessingModule* getModule(int index) const;

private:
    std::vector<std::unique_ptr<ProcessingModule>> m_modules;
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
    void processCameraQueue(const QString& cameraKey);
    std::shared_ptr<ProcessingPipeline> m_pipeline;
    std::atomic<bool> m_realTimeEnabled;
    void submitFrame(const ImageFrame& frame);
    mutable QMutex m_frameMutex;
    QHash<QString, CameraSlot> m_cameraSlots;

    QThreadPool* m_threadPool;
};

}
