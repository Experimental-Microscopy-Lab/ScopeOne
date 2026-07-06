#include "internal/ImageProcessingFramework.h"
#include <QDebug>
#include <QMutexLocker>
#include <QtGlobal>
#include <algorithm>
#include <limits>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        // Clones a concrete module and copies its parameter values
        std::unique_ptr<ProcessingModule> cloneModule(const ProcessingModule* source, QObject* parent)
        {
            std::unique_ptr<ProcessingModule> module;
            if (qobject_cast<const FFTModule*>(source))
            {
                module = std::make_unique<FFTModule>(parent);
            }
            else if (qobject_cast<const BackgroundCalibrationModule*>(source))
            {
                module = std::make_unique<BackgroundCalibrationModule>(parent);
            }
            else if (qobject_cast<const SpatiotemporalBinningModule*>(source))
            {
                module = std::make_unique<SpatiotemporalBinningModule>(parent);
            }
            else if (qobject_cast<const GaussianBlurModule*>(source))
            {
                module = std::make_unique<GaussianBlurModule>(parent);
            }
            else if (qobject_cast<const DifferentialRollingModule*>(source))
            {
                module = std::make_unique<DifferentialRollingModule>(parent);
            }
            else
            {
                qFatal("Unsupported processing module type");
            }

            module->setParameters(source->getParameters());
            return module;
        }

        // Carries frame identity through module outputs
        void inheritFrameMetadata(ImageFrame& nextFrame, const ImageFrame& previousFrame)
        {
            nextFrame.cameraId = nextFrame.cameraId.trimmed();
            if (nextFrame.cameraId.isEmpty())
            {
                nextFrame.cameraId = previousFrame.cameraId.trimmed();
            }
            if (nextFrame.frameIndex == 0)
            {
                nextFrame.frameIndex = previousFrame.frameIndex;
            }
            if (nextFrame.timestampNs == 0)
            {
                nextFrame.timestampNs = previousFrame.timestampNs;
            }
            if (!nextFrame.hasSourceRoi() && previousFrame.hasSourceRoi())
            {
                nextFrame.sourceRoiX = previousFrame.sourceRoiX;
                nextFrame.sourceRoiY = previousFrame.sourceRoiY;
                nextFrame.sourceRoiWidth = previousFrame.sourceRoiWidth;
                nextFrame.sourceRoiHeight = previousFrame.sourceRoiHeight;
            }
        }

        // Normalizes frame identity before it enters a runtime pipeline
        ImageFrame normalizedFrameSource(const ImageFrame& frame)
        {
            ImageFrame normalizedFrame(frame);
            normalizedFrame.cameraId = frame.cameraId.trimmed();
            return normalizedFrame;
        }
    } // namespace

    // Creates an empty configurable processing pipeline
    ProcessingPipeline::ProcessingPipeline(QObject* parent)
        : QObject(parent)
    {
    }

    // Adds one module to the shared configuration pipeline
    void ProcessingPipeline::addModule(std::unique_ptr<ProcessingModule> module)
    {
        if (!module)
        {
            qFatal("ProcessingPipeline requires a valid module");
        }
        QMutexLocker locker(&m_modulesMutex);
        m_modules.push_back(std::move(module));
    }

    // Removes one module from the shared configuration pipeline
    bool ProcessingPipeline::removeModule(int index)
    {
        QMutexLocker locker(&m_modulesMutex);
        if (index >= 0 && index < static_cast<int>(m_modules.size()))
        {
            m_modules.erase(m_modules.begin() + index);
            return true;
        }
        return false;
    }

    // Creates an independent runtime pipeline with copied module parameters
    std::shared_ptr<ProcessingPipeline> ProcessingPipeline::clone(QObject* parent) const
    {
        auto pipeline = std::make_shared<ProcessingPipeline>(parent);
        QMutexLocker locker(&m_modulesMutex);
        for (const auto& module : m_modules)
        {
            pipeline->addModule(cloneModule(module.get(), pipeline.get()));
        }
        return pipeline;
    }

    // Visits every configured module while holding the module list lock
    void ProcessingPipeline::forEachModule(const std::function<void(const ProcessingModule *)>& visitor) const
    {
        QMutexLocker locker(&m_modulesMutex);
        for (const auto& module : m_modules)
        {
            visitor(module.get());
        }
    }

    // Gives controlled mutable access to one configured module
    bool ProcessingPipeline::withModule(int index, const std::function<bool(ProcessingModule *)>& visitor)
    {
        QMutexLocker locker(&m_modulesMutex);
        if (index < 0 || index >= static_cast<int>(m_modules.size()))
        {
            return false;
        }
        return visitor(m_modules[static_cast<size_t>(index)].get());
    }

    // Runs all modules in order for one frame
    ImageFrame ProcessingPipeline::process(const ImageFrame& input, int processingBitDepth)
    {
        return processRange(input,
                            processingBitDepth,
                            0,
                            (std::numeric_limits<int>::max)());
    }

    // Runs modules from a specific pipeline position
    ImageFrame ProcessingPipeline::processFrom(int startModuleIndex,
                                               const ImageFrame& input,
                                               int processingBitDepth)
    {
        return processRange(input,
                            processingBitDepth,
                            startModuleIndex,
                            (std::numeric_limits<int>::max)());
    }

    // Runs modules through a specific pipeline position
    ImageFrame ProcessingPipeline::processThrough(int endModuleIndex,
                                                  const ImageFrame& input,
                                                  int processingBitDepth)
    {
        int endModuleIndexExclusive = 0;
        if (endModuleIndex >= (std::numeric_limits<int>::max)() - 1)
        {
            endModuleIndexExclusive = (std::numeric_limits<int>::max)();
        }
        else if (endModuleIndex >= 0)
        {
            endModuleIndexExclusive = endModuleIndex + 1;
        }
        return processRange(input,
                            processingBitDepth,
                            0,
                            endModuleIndexExclusive);
    }

    // Runs a pipeline segment and returns the final output frame
    ImageFrame ProcessingPipeline::processRange(const ImageFrame& input,
                                                int processingBitDepth,
                                                int startModuleIndex,
                                                int endModuleIndexExclusive)
    {
        if (!input.isValid())
        {
            return input;
        }

        ModuleInput currentInput(input, processingBitDepth);

        QMutexLocker locker(&m_modulesMutex);
        const int startIndex = std::clamp(startModuleIndex, 0, static_cast<int>(m_modules.size()));
        const int endIndex = std::clamp(endModuleIndexExclusive, startIndex, static_cast<int>(m_modules.size()));
        for (int moduleIndex = startIndex; moduleIndex < endIndex; ++moduleIndex)
        {
            ProcessingModule* module = m_modules[static_cast<size_t>(moduleIndex)].get();
            ModuleOutput moduleOutput;
            const bool success = module->process(currentInput, moduleOutput);
            if (success && moduleOutput.isValid())
            {
                ImageFrame nextFrame = moduleOutput.frame;
                inheritFrameMetadata(nextFrame, currentInput.frame);

                currentInput.frame = std::move(nextFrame);
                currentInput.processingBitDepth = processingBitDepth;
            }
            else if (moduleOutput.hasError())
            {
                qWarning() << "Module" << module->getModuleName() << "failed:" << moduleOutput.error;
                return {};
            }
            else
            {
                qWarning() << "Module" << module->getModuleName() << "failed";
                return {};
            }
        }

        return currentInput.frame;
    }

    // Returns the current configured module count
    int ProcessingPipeline::getModuleCount() const
    {
        QMutexLocker locker(&m_modulesMutex);
        return static_cast<int>(m_modules.size());
    }

    // Creates the processing manager and sizes the worker pool
    ImageProcessingManager::ImageProcessingManager(QObject* parent)
        : QObject(parent)
          , m_pipeline(std::make_shared<ProcessingPipeline>())
          , m_realTimeEnabled(false)
          , m_threadPool(QThreadPool::globalInstance())
    {
        int idealThreadCount = QThread::idealThreadCount();
        m_threadPool->setMaxThreadCount(qMax(2, idealThreadCount - 1));
    }

    // Waits briefly for active processing workers to finish
    ImageProcessingManager::~ImageProcessingManager()
    {
        m_threadPool->waitForDone(5000);
    }

    // Returns the shared configuration pipeline
    ProcessingPipeline* ImageProcessingManager::pipeline() const
    {
        return m_pipeline.get();
    }

    // Enables or disables real time processing for incoming frames
    void ImageProcessingManager::enableRealTimeProcessing(bool enabled)
    {
        m_realTimeEnabled = enabled;
    }

    // Sets the processing bit depth and rebuilds runtime pipelines
    void ImageProcessingManager::setProcessingBitDepth(int bitDepth)
    {
        m_processingBitDepth = bitDepth >= 16 ? 16 : 8;
        clearRuntimePipelines();
    }

    // Drops per camera runtime pipelines so they rebuild from configuration
    void ImageProcessingManager::clearRuntimePipelines()
    {
        QMutexLocker locker(&m_frameMutex);
        m_livePipelines.clear();
        m_offlinePipelines.clear();
    }

    // Queues one frame for asynchronous processing
    void ImageProcessingManager::processFrameAsync(const ImageFrame& frame)
    {
        if (!m_realTimeEnabled || !frame.isValid())
        {
            return;
        }
        submitFrame(frame);
    }

    // Runs one frame through the runtime pipeline for its source
    ImageFrame ImageProcessingManager::processFrame(const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return frame;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        return offlinePipelineForCamera(getCameraKey(normalizedFrame))->process(normalizedFrame,
                                                                                m_processingBitDepth.load());
    }

    // Continues one frame through the runtime pipeline for its source
    ImageFrame ImageProcessingManager::processFrameFrom(int startModuleIndex, const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return frame;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        return offlinePipelineForCamera(getCameraKey(normalizedFrame))->processFrom(startModuleIndex,
                                                                                    normalizedFrame,
                                                                                    m_processingBitDepth.load());
    }

    // Runs one frame through a runtime pipeline stage for its source
    ImageFrame ImageProcessingManager::processFrameThrough(int endModuleIndex, const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return frame;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        return offlinePipelineForCamera(getCameraKey(normalizedFrame))->processThrough(endModuleIndex,
                                                                                       normalizedFrame,
                                                                                       m_processingBitDepth.load());
    }

    // Builds a stable key for camera specific processing state
    QString ImageProcessingManager::getCameraKey(const ImageFrame& frame) const
    {
        const QString key = frame.cameraId.trimmed();
        return key.isEmpty() ? QStringLiteral("__default__") : key;
    }

    // Stores the newest frame and starts a worker when needed
    void ImageProcessingManager::submitFrame(const ImageFrame& frame)
    {
        if (!m_realTimeEnabled || !frame.isValid())
        {
            return;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        const QString cameraKey = getCameraKey(normalizedFrame);
        bool shouldStartWorker = false;

        {
            QMutexLocker locker(&m_frameMutex);
            CameraSlot& slot = m_cameraSlots[cameraKey];
            slot.latestFrame = normalizedFrame;
            slot.hasFrame = true;
            if (!slot.processing)
            {
                slot.processing = true;
                shouldStartWorker = true;
            }
        }

        if (shouldStartWorker)
        {
            m_threadPool->start([this, cameraKey]()
            {
                processCameraQueue(cameraKey);
            });
        }
    }

    // Returns the live runtime pipeline owned by one camera preview stream
    std::shared_ptr<ProcessingPipeline> ImageProcessingManager::livePipelineForCamera(const QString& cameraKey)
    {
        return pipelineForCamera(m_livePipelines, cameraKey);
    }

    // Returns the offline runtime pipeline used by synchronous frame processing
    std::shared_ptr<ProcessingPipeline> ImageProcessingManager::offlinePipelineForCamera(const QString& cameraKey)
    {
        return pipelineForCamera(m_offlinePipelines, cameraKey);
    }

    // Returns a cloned runtime pipeline from the selected state table
    std::shared_ptr<ProcessingPipeline> ImageProcessingManager::pipelineForCamera(
        QHash<QString, std::shared_ptr<ProcessingPipeline>>& pipelines,
        const QString& cameraKey)
    {
        QMutexLocker locker(&m_frameMutex);
        auto it = pipelines.find(cameraKey);
        if (it != pipelines.end())
        {
            return it.value();
        }

        auto pipeline = m_pipeline->clone();
        pipelines.insert(cameraKey, pipeline);
        return pipeline;
    }

    // Drains the newest frame queue for one camera processing state
    void ImageProcessingManager::processCameraQueue(const QString& cameraKey)
    {
        while (true)
        {
            ImageFrame frame;

            {
                QMutexLocker locker(&m_frameMutex);
                auto it = m_cameraSlots.find(cameraKey);
                if (it == m_cameraSlots.end() || !it->hasFrame)
                {
                    if (it != m_cameraSlots.end())
                    {
                        it->processing = false;
                    }
                    return;
                }
                frame = it->latestFrame;
                it->hasFrame = false;
            }

            std::shared_ptr<ProcessingPipeline> pipeline = livePipelineForCamera(cameraKey);

            try
            {
                const ImageFrame result = pipeline->process(frame, m_processingBitDepth.load());
                emit imageProcessed(result);
            }
            catch (const std::exception& e)
            {
                emit processingError(QString("Processing failed: %1").arg(e.what()));
            }
        }
    }
} // namespace scopeone::core::internal
