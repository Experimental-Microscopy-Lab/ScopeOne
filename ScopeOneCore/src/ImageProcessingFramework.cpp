#include "internal/ImageProcessingFramework.h"
#include <QDebug>
#include <QMutexLocker>
#include <algorithm>

namespace scopeone::core::internal {

ProcessingPipeline::ProcessingPipeline(QObject* parent)
    : QObject(parent)
{
}

void ProcessingPipeline::addModule(std::unique_ptr<ProcessingModule> module)
{
    if (module) {
        m_modules.push_back(std::move(module));
    }
}

void ProcessingPipeline::removeModule(int index)
{
    if (index >= 0 && index < static_cast<int>(m_modules.size())) {
        m_modules.erase(m_modules.begin() + index);
    }
}

ImageFrame ProcessingPipeline::process(const ImageFrame& input)
{
    // Run modules in order and keep the last valid frame
    ImageFrame result(input);

    if (!input.isValid()) {
        return result;
    }

    ModuleInput currentInput(input);

    for (const auto& module : m_modules) {
        if (!module) {
            continue;
        }

        ModuleOutput moduleOutput;
        const bool success = module->process(currentInput, moduleOutput);
        if (success && moduleOutput.isValid()) {
            ImageFrame nextFrame = moduleOutput.frame;
            if (nextFrame.cameraId.isEmpty()) {
                nextFrame.cameraId = currentInput.frame.cameraId;
            }

            currentInput.frame = std::move(nextFrame);
            result = currentInput.frame;
        } else if (moduleOutput.hasError()) {
            qWarning() << "Module" << module->getModuleName() << "failed:" << moduleOutput.error;
        } else {
            qWarning() << "Module" << module->getModuleName() << "failed";
        }
    }

    if (!result.isValid()) {
        result = currentInput.frame;
    }

    return result;
}

ProcessingModule* ProcessingPipeline::getModule(int index) const
{
    if (index >= 0 && index < static_cast<int>(m_modules.size())) {
        return m_modules[index].get();
    }
    return nullptr;
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

void ImageProcessingManager::processFrameAsync(const ImageFrame& frame)
{
    if (!m_realTimeEnabled || !frame.isValid()) {
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
    if (!m_realTimeEnabled || !frame.isValid()) {
        return;
    }

    const QString cameraKey = getCameraKey(frame);
    bool shouldStartWorker = false;

    {
        QMutexLocker locker(&m_frameMutex);
        CameraSlot& slot = m_cameraSlots[cameraKey];
        slot.latestFrame = frame; // Keep only latest frame for this camera
        slot.hasFrame = true;
        if (!slot.processing) {
            slot.processing = true;
            shouldStartWorker = true;
        }
    }

    if (shouldStartWorker) {
        m_threadPool->start([this, cameraKey]() {
            processCameraQueue(cameraKey);
        });
    }
}

void ImageProcessingManager::processCameraQueue(const QString& cameraKey)
{
    // One worker drains one camera queue
    while (true) {
        ImageFrame frame;

        {
            QMutexLocker locker(&m_frameMutex);
            auto it = m_cameraSlots.find(cameraKey);
            if (it == m_cameraSlots.end() || !it->hasFrame) {
                if (it != m_cameraSlots.end()) {
                    it->processing = false;
                }
                return;
            }
            frame = it->latestFrame;
            it->hasFrame = false;
        }

        ProcessingPipeline* pipeline = m_pipeline.get();

        if (!pipeline) {
            continue;
        }

        try {
            const ImageFrame result = pipeline->process(frame);
            emit imageProcessed(result);
        } catch (const std::exception& e) {
            emit processingError(QString("Processing failed: %1").arg(e.what()));
        }
    }
}

} // namespace scopeone::core::internal
