#include "internal/ImageProcessingFramework.h"
#include "internal/FrameBufferUtils.h"
#include <QMutexLocker>
#include <QThread>
#include <QtGlobal>
#include <algorithm>
#include <limits>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        // Normalizes frame identity before it enters a runtime pipeline
        ImageFrame normalizedFrameSource(const ImageFrame& frame)
        {
            ImageFrame normalizedFrame(frame);
            normalizedFrame.cameraId = frame.cameraId.trimmed();
            return normalizedFrame;
        }
    } // namespace

    // Creates a runtime pipeline from configured module instances
    ProcessingPipelineRuntime::ProcessingPipelineRuntime(
        std::vector<std::unique_ptr<ProcessingModule>> modules)
        : m_modules(std::move(modules))
    {
    }

    // Runs all runtime modules in order for one frame
    ProcessingResult ProcessingPipelineRuntime::process(const ImageFrame& input, int processingBitDepth)
    {
        return processValue(ProcessingValue{input}, processingBitDepth);
    }

    ProcessingResult ProcessingPipelineRuntime::processValue(const ProcessingValue& input,
                                                              int processingBitDepth)
    {
        return processRange(input,
                            processingBitDepth,
                            0,
                            (std::numeric_limits<int>::max)());
    }

    // Runs runtime modules from a specific pipeline position
    ProcessingResult ProcessingPipelineRuntime::processFrom(int startModuleIndex,
                                                            const ImageFrame& input,
                                                            int processingBitDepth)
    {
        return processRange(ProcessingValue{input},
                            processingBitDepth,
                            startModuleIndex,
                            (std::numeric_limits<int>::max)());
    }

    // Runs runtime modules through a specific pipeline position
    ProcessingResult ProcessingPipelineRuntime::processThrough(int endModuleIndex,
                                                               const ImageFrame& input,
                                                               int processingBitDepth)
    {
        const int endModuleIndexExclusive = endModuleIndex >= (std::numeric_limits<int>::max)() - 1
                                                ? (std::numeric_limits<int>::max)()
                                                : qMax(0, endModuleIndex + 1);
        return processRange(ProcessingValue{input},
                            processingBitDepth,
                            0,
                            endModuleIndexExclusive);
    }

    // Runs a runtime pipeline segment and returns its frame or error
    ProcessingResult ProcessingPipelineRuntime::processRange(const ProcessingValue& input,
                                                             int processingBitDepth,
                                                             int startModuleIndex,
                                                             int endModuleIndexExclusive)
    {
        if (std::holds_alternative<ImageFrame>(input)
            && !std::get<ImageFrame>(input).isValid())
        {
            return ProcessingResult(ImageFrame{}, QStringLiteral("Invalid processing input"));
        }

        ProcessingValue currentValue(input);
        ImageFrame displayFrame;
        const int startIndex = std::clamp(startModuleIndex, 0, static_cast<int>(m_modules.size()));
        const int endIndex = std::clamp(endModuleIndexExclusive, startIndex, static_cast<int>(m_modules.size()));
        for (int moduleIndex = startIndex; moduleIndex < endIndex; ++moduleIndex)
        {
            ProcessingModule* module = m_modules[static_cast<size_t>(moduleIndex)].get();
            if (!module->isEnabled())
            {
                continue;
            }
            ProcessingResult result = module->processValue(currentValue, processingBitDepth);
            if (!result.succeeded())
            {
                if (result.error.isEmpty())
                {
                    result.error = QString("%1 returned an invalid frame").arg(module->name());
                }
                return result;
            }

            if (result.hasImage())
            {
                ImageFrame image = std::holds_alternative<ImageFrame>(result.value)
                                        ? std::get<ImageFrame>(result.value)
                                        : result.frame;
                if (std::holds_alternative<ImageFrame>(currentValue))
                {
                    copyFrameMetadata(std::get<ImageFrame>(currentValue), image);
                }
                result.value = image;
                result.frame = image;
            }
            else if (result.frame.isValid())
            {
                displayFrame = result.frame;
            }
            currentValue = std::move(result.value);
        }
        ProcessingResult result(std::move(currentValue));
        if (!result.hasImage())
        {
            result.frame = std::move(displayFrame);
        }
        return result;
    }

    // Adds one module to the editable pipeline definition
    void ProcessingPipelineDefinition::addModule(std::unique_ptr<ProcessingModule> module)
    {
        QMutexLocker locker(&m_modulesMutex);
        m_modules.push_back(std::move(module));
    }

    // Removes one module from the editable pipeline definition
    bool ProcessingPipelineDefinition::removeModule(int index)
    {
        QMutexLocker locker(&m_modulesMutex);
        if (index >= 0 && index < static_cast<int>(m_modules.size()))
        {
            m_modules.erase(m_modules.begin() + index);
            return true;
        }
        return false;
    }

    // Moves one configured module to a new pipeline position
    bool ProcessingPipelineDefinition::moveModule(int from, int to)
    {
        QMutexLocker locker(&m_modulesMutex);
        if (from < 0 || from >= static_cast<int>(m_modules.size())
            || to < 0 || to >= static_cast<int>(m_modules.size()))
        {
            return false;
        }
        if (from == to)
        {
            return true;
        }
        auto module = std::move(m_modules[static_cast<size_t>(from)]);
        m_modules.erase(m_modules.begin() + from);
        m_modules.insert(m_modules.begin() + to, std::move(module));
        return true;
    }

    // Creates an independent runtime from the current definition
    std::shared_ptr<ProcessingPipelineRuntime> ProcessingPipelineDefinition::createRuntime() const
    {
        std::vector<std::unique_ptr<ProcessingModule>> modules;
        QMutexLocker locker(&m_modulesMutex);
        modules.reserve(m_modules.size());
        for (const auto& module : m_modules)
        {
            auto runtimeModule = module->createRuntime();
            runtimeModule->setEnabled(module->isEnabled());
            modules.push_back(std::move(runtimeModule));
        }
        return std::make_shared<ProcessingPipelineRuntime>(std::move(modules));
    }

    // Visits every configured module while holding the module list lock
    void ProcessingPipelineDefinition::forEachModule(
        const std::function<void(const ProcessingModule *)>& visitor) const
    {
        QMutexLocker locker(&m_modulesMutex);
        for (const auto& module : m_modules)
        {
            visitor(module.get());
        }
    }

    // Gives controlled mutable access to one configured module
    bool ProcessingPipelineDefinition::withModule(
        int index,
        const std::function<void(ProcessingModule *)>& visitor)
    {
        QMutexLocker locker(&m_modulesMutex);
        if (index < 0 || index >= static_cast<int>(m_modules.size()))
        {
            return false;
        }
        visitor(m_modules[static_cast<size_t>(index)].get());
        return true;
    }

    // Returns the current configured module count
    int ProcessingPipelineDefinition::moduleCount() const
    {
        QMutexLocker locker(&m_modulesMutex);
        return static_cast<int>(m_modules.size());
    }

    // Creates the processing manager and sizes the worker pool
    ImageProcessingManager::ImageProcessingManager(QObject* parent)
        : QObject(parent)
          , m_realTimeEnabled(false)
    {
        const int idealThreadCount = QThread::idealThreadCount();
        m_threadPool.setMaxThreadCount(qMax(2, idealThreadCount - 1));
    }

    // Waits for owned processing workers to finish
    ImageProcessingManager::~ImageProcessingManager()
    {
        m_threadPool.waitForDone();
    }

    // Returns the editable processing definition
    ProcessingPipelineDefinition& ImageProcessingManager::definition()
    {
        return m_definition;
    }

    // Enables or disables real time processing for incoming frames
    void ImageProcessingManager::enableRealTimeProcessing(bool enabled)
    {
        QMutexLocker locker(&m_frameMutex);
        if (m_realTimeEnabled.load() == enabled)
        {
            return;
        }

        m_realTimeEnabled = enabled;
        ++m_liveGeneration;
        for (CameraSlot& slot : m_cameraSlots)
        {
            slot.latestFrame = {};
            slot.hasFrame = false;
        }
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
        ++m_liveGeneration;
        for (CameraSlot& slot : m_cameraSlots)
        {
            slot.latestFrame = {};
            slot.hasFrame = false;
        }
    }

    // Stores the newest frame and starts a worker when needed
    void ImageProcessingManager::processFrameAsync(const ImageFrame& frame,
                                                   quint64 processingToken,
                                                   const std::function<bool()>& tokenIsCurrent)
    {
        if (!m_realTimeEnabled || !frame.isValid())
        {
            return;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        const QString cameraKey = getCameraKey(normalizedFrame);
        bool shouldStartWorker = false;
        quint64 replacedProcessingToken = 0;

        {
            QMutexLocker locker(&m_frameMutex);
            if (!m_realTimeEnabled.load())
            {
                return;
            }
            if (processingToken != 0 && (!tokenIsCurrent || !tokenIsCurrent()))
            {
                return;
            }
            CameraSlot& slot = m_cameraSlots[cameraKey];
            if (slot.hasFrame)
            {
                replacedProcessingToken = slot.processingToken;
            }
            slot.latestFrame = normalizedFrame;
            slot.latestFrameGeneration = m_liveGeneration;
            slot.processingToken = processingToken;
            slot.hasFrame = true;
            if (!slot.processing)
            {
                slot.processing = true;
                shouldStartWorker = true;
            }
        }

        if (replacedProcessingToken != 0)
        {
            emit processingFrameFinished(normalizedFrame.cameraId, replacedProcessingToken);
        }

        if (shouldStartWorker)
        {
            m_threadPool.start([this, cameraKey]()
            {
                processCameraQueue(cameraKey);
            });
        }
    }

    // Runs one frame through the runtime pipeline for its source
    ImageFrame ImageProcessingManager::processFrame(const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return frame;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        return frameFromResult(
            offlinePipelineForCamera(getCameraKey(normalizedFrame))->process(normalizedFrame,
                                                                              m_processingBitDepth.load()));
    }

    // Continues one frame through the runtime pipeline for its source
    ImageFrame ImageProcessingManager::processFrameFrom(int startModuleIndex, const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return frame;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        return frameFromResult(
            offlinePipelineForCamera(getCameraKey(normalizedFrame))->processFrom(startModuleIndex,
                                                                                  normalizedFrame,
                                                                                  m_processingBitDepth.load()));
    }

    // Runs one frame through a runtime pipeline stage for its source
    ImageFrame ImageProcessingManager::processFrameThrough(int endModuleIndex, const ImageFrame& frame)
    {
        if (!frame.isValid())
        {
            return frame;
        }

        const ImageFrame normalizedFrame = normalizedFrameSource(frame);
        return frameFromResult(
            offlinePipelineForCamera(getCameraKey(normalizedFrame))->processThrough(endModuleIndex,
                                                                                     normalizedFrame,
                                                                                     m_processingBitDepth.load()));
    }

    // Converts a processing result into the existing frame API
    ImageFrame ImageProcessingManager::frameFromResult(ProcessingResult result)
    {
        if (!result.succeeded())
        {
            emit processingError(result.error.isEmpty()
                                     ? QStringLiteral("Processing pipeline returned no output")
                                     : result.error);
            return {};
        }
        return result.hasImage()
                   ? std::get<ImageFrame>(std::move(result.value))
                   : std::move(result.frame);
    }

    // Builds a stable key for camera specific processing state
    QString ImageProcessingManager::getCameraKey(const ImageFrame& frame) const
    {
        const QString key = frame.cameraId.trimmed();
        return key.isEmpty() ? QStringLiteral("__default__") : key;
    }

    // Returns the live runtime pipeline owned by one camera preview stream
    std::shared_ptr<ProcessingPipelineRuntime> ImageProcessingManager::livePipelineForCamera(
        const QString& cameraKey)
    {
        return pipelineForCamera(m_livePipelines, cameraKey);
    }

    // Returns the offline runtime pipeline used by synchronous frame processing
    std::shared_ptr<ProcessingPipelineRuntime> ImageProcessingManager::offlinePipelineForCamera(
        const QString& cameraKey)
    {
        return pipelineForCamera(m_offlinePipelines, cameraKey);
    }

    // Returns a runtime pipeline from the selected state table
    std::shared_ptr<ProcessingPipelineRuntime> ImageProcessingManager::pipelineForCamera(
        QHash<QString, std::shared_ptr<ProcessingPipelineRuntime>>& pipelines,
        const QString& cameraKey)
    {
        QMutexLocker locker(&m_frameMutex);
        auto it = pipelines.find(cameraKey);
        if (it != pipelines.end())
        {
            return it.value();
        }

        auto pipeline = m_definition.createRuntime();
        pipelines.insert(cameraKey, pipeline);
        return pipeline;
    }

    // Drains the newest frame queue for one camera processing state
    void ImageProcessingManager::processCameraQueue(const QString& cameraKey)
    {
        while (true)
        {
            ImageFrame frame;
            quint64 frameGeneration = 0;
            quint64 processingToken = 0;

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
                frameGeneration = it->latestFrameGeneration;
                processingToken = it->processingToken;
                it->hasFrame = false;
            }

            try
            {
                std::shared_ptr<ProcessingPipelineRuntime> pipeline = livePipelineForCamera(cameraKey);
                ProcessingResult result = pipeline->process(frame, m_processingBitDepth.load());
                if (result.succeeded())
                {
                    bool publish = false;
                    {
                        QMutexLocker locker(&m_frameMutex);
                        const auto it = m_cameraSlots.constFind(cameraKey);
                        publish = it != m_cameraSlots.constEnd()
                            && m_realTimeEnabled.load()
                            && frameGeneration == m_liveGeneration;
                    }
                    if (publish)
                    {
                        // Publish completed data before downstream display coalescing
                        emit imageProcessed(result.frame);
                    }
                }
                else
                {
                    emit processingError(result.error.isEmpty()
                                             ? QStringLiteral("Processing returned an invalid frame")
                                             : result.error);
                }
            }
            catch (const std::exception& e)
            {
                emit processingError(QString("Processing failed: %1").arg(e.what()));
            }
            if (processingToken != 0)
            {
                emit processingFrameFinished(frame.cameraId, processingToken);
            }
        }
    }

} // namespace scopeone::core::internal
