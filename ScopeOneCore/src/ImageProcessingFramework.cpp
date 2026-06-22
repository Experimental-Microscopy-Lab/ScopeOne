#include "internal/ImageProcessingFramework.h"
#include <QDebug>
#include <QMutexLocker>
#include <algorithm>

namespace scopeone::core::internal
{
    namespace
    {
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

            if (module)
            {
                module->setParameters(source->getParameters());
            }
            return module;
        }
    } // namespace

    ProcessingPipeline::ProcessingPipeline(QObject* parent)
        : QObject(parent)
    {
    }

    void ProcessingPipeline::addModule(std::unique_ptr<ProcessingModule> module)
    {
        if (module)
        {
            QMutexLocker locker(&m_modulesMutex);
            m_modules.push_back(std::move(module));
        }
    }

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

    void ProcessingPipeline::forEachModule(const std::function<void(const ProcessingModule *)> & visitor)
    const
{
    QMutexLocker locker(&m_modulesMutex);
    for (const auto& module : m_modules) {
        visitor(module.get());
    }
}

    bool ProcessingPipeline::withModule(int index, const std::function<bool(ProcessingModule *)>& visitor)
    {
        QMutexLocker locker(&m_modulesMutex);
        if (index < 0 || index >= static_cast<int>(m_modules.size()))
        {
            return false;
        }
        return visitor(m_modules[static_cast<size_t>(index)].get());
    }

    ImageFrame ProcessingPipeline::process(const ImageFrame& input, int processingBitDepth)
    {
        // Run modules in order and keep the last valid frame
        ImageFrame result(input);

        if (!input.isValid())
        {
            return result;
        }

        ModuleInput currentInput(input, processingBitDepth);

        QMutexLocker locker(&m_modulesMutex);
        for (const auto& module : m_modules)
        {
            ModuleOutput moduleOutput;
            const bool success = module->process(currentInput, moduleOutput);
            if (success && moduleOutput.isValid())
            {
                ImageFrame nextFrame = moduleOutput.frame;
                if (nextFrame.cameraId.isEmpty())
                {
                    nextFrame.cameraId = currentInput.frame.cameraId;
                }

                currentInput.frame = std::move(nextFrame);
                currentInput.processingBitDepth = processingBitDepth;
                result = currentInput.frame;
            }
            else if (moduleOutput.hasError())
            {
                qWarning() << "Module" << module->getModuleName() << "failed:" << moduleOutput.error;
            }
            else
            {
                qWarning() << "Module" << module->getModuleName() << "failed";
            }
        }

        return result;
    }

    int ProcessingPipeline::getModuleCount() const
    {
        QMutexLocker locker(&m_modulesMutex);
        return static_cast<int>(m_modules.size());
    }

    ImageProcessingManager::ImageProcessingManager(QObject* parent)
        : QObject(parent)
          , m_pipeline(std::make_shared<ProcessingPipeline>())
          , m_realTimeEnabled(false)
          , m_threadPool(QThreadPool::globalInstance())
    {
        int idealThreadCount = QThread::idealThreadCount();
        m_threadPool->setMaxThreadCount(qMax(2, idealThreadCount - 1));
    }

    ImageProcessingManager::~ImageProcessingManager()
    {
        m_threadPool->waitForDone(5000);
    }

    ProcessingPipeline* ImageProcessingManager::pipeline() const
    {
        return m_pipeline.get();
    }

    void ImageProcessingManager::enableRealTimeProcessing(bool enabled)
    {
        m_realTimeEnabled = enabled;
    }

    void ImageProcessingManager::setProcessingBitDepth(int bitDepth)
    {
        m_processingBitDepth = bitDepth >= 16 ? 16 : 8;
        clearRuntimePipelines();
    }

    void ImageProcessingManager::clearRuntimePipelines()
    {
        QMutexLocker locker(&m_frameMutex);
        m_cameraPipelines.clear();
    }

    void ImageProcessingManager::processFrameAsync(const ImageFrame& frame)
    {
        if (!m_realTimeEnabled || !frame.isValid())
        {
            return;
        }
        submitFrame(frame);
    }

    QString ImageProcessingManager::getCameraKey(const ImageFrame& frame) const
    {
        const QString key = frame.cameraId;
        return key.isEmpty() ? QStringLiteral("__default__") : key;
    }

    void ImageProcessingManager::submitFrame(const ImageFrame& frame)
    {
        // Keep only the newest pending frame per camera
        if (!m_realTimeEnabled || !frame.isValid())
        {
            return;
        }

        const QString cameraKey = getCameraKey(frame);
        bool shouldStartWorker = false;

        {
            QMutexLocker locker(&m_frameMutex);
            CameraSlot& slot = m_cameraSlots[cameraKey];
            slot.latestFrame = frame; // Keep only latest frame for this camera
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

    std::shared_ptr<ProcessingPipeline> ImageProcessingManager::pipelineForCamera(const QString& cameraKey)
    {
        QMutexLocker locker(&m_frameMutex);
        auto it = m_cameraPipelines.find(cameraKey);
        if (it != m_cameraPipelines.end())
        {
            return it.value();
        }

        auto pipeline = m_pipeline->clone();
        m_cameraPipelines.insert(cameraKey, pipeline);
        return pipeline;
    }

    void ImageProcessingManager::processCameraQueue(const QString& cameraKey)
    {
        // One worker drains one camera queue
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

            std::shared_ptr<ProcessingPipeline> pipeline = pipelineForCamera(cameraKey);

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
