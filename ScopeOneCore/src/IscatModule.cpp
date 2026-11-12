#include "internal/IscatModule.h"

#include "internal/ProcessingModuleRegistry.h"
#include "internal/ProcessingParameters.h"

#include <QByteArray>
#include <algorithm>
#include <opencv2/imgproc.hpp>
#include <utility>

namespace scopeone::core::internal
{
    namespace
    {
        scopeone::core::ImageFrame makeMono16Output(
            const scopeone::core::ImageFrame& reference,
            const cv::Mat& values)
        {
            QByteArray bytes(static_cast<qsizetype>(values.cols)
                                 * values.rows
                                 * static_cast<int>(sizeof(quint16)),
                             Qt::Uninitialized);
            cv::Mat output(values.rows,
                           values.cols,
                           CV_16UC1,
                           bytes.data(),
                           static_cast<size_t>(values.cols * sizeof(quint16)));
            values.convertTo(output, CV_16U);

            scopeone::core::ImageFrame frame = reference;
            frame.width = values.cols;
            frame.height = values.rows;
            frame.stride = values.cols * static_cast<int>(sizeof(quint16));
            frame.bitsPerSample = 16;
            frame.pixelFormat = scopeone::core::ImagePixelFormat::Mono16;
            frame.bytes = std::move(bytes);
            return frame;
        }
    }

    QString IscatModule::id() const
    {
        return QStringLiteral("iscat.processing");
    }

    QString IscatModule::name() const
    {
        return QStringLiteral("iSCAT Processing");
    }

    QVariantMap IscatModule::parameters() const
    {
        return {{QStringLiteral("mode"), m_mode},
                {QStringLiteral("frames"), m_frames},
                {QStringLiteral("blur_sigma"), m_blurSigma},
                {QStringLiteral("capture_reference"), m_captureReference},
                {QStringLiteral("contrast_gain"), m_contrastGain}};
    }

    void IscatModule::setParameters(const QVariantMap& parameters)
    {
        const int previousMode = m_mode;
        const int previousFrames = m_frames;

        if (parameters.contains(QStringLiteral("mode")))
        {
            m_mode = qBound(0, parameters.value(QStringLiteral("mode")).toInt(), 2);
        }
        if (parameters.contains(QStringLiteral("frames")))
        {
            m_frames = qBound(1, parameters.value(QStringLiteral("frames")).toInt(), 500);
        }
        if (parameters.contains(QStringLiteral("blur_sigma")))
        {
            m_blurSigma = qBound(1.0, parameters.value(QStringLiteral("blur_sigma")).toDouble(), 100.0);
        }
        if (parameters.contains(QStringLiteral("capture_reference")))
        {
            m_captureReference = parameters.value(QStringLiteral("capture_reference")).toBool();
        }
        if (parameters.contains(QStringLiteral("contrast_gain")))
        {
            m_contrastGain = qBound(1.0, parameters.value(QStringLiteral("contrast_gain")).toDouble(), 500.0);
        }

        if (previousMode != m_mode || previousFrames != m_frames)
        {
            clearState();
        }
    }

    std::unique_ptr<scopeone::core::ProcessingModule> IscatModule::createRuntime() const
    {
        auto module = std::make_unique<IscatModule>();
        module->setParameters(parameters());
        return module;
    }

    bool IscatModule::resetState()
    {
        clearState();
        return true;
    }

    void IscatModule::clearState()
    {
        m_referenceSum.release();
        m_referenceCount = 0;
        m_reference.release();
        m_draBatchA.clear();
        m_draBatchB.clear();
        m_draSumA.release();
        m_draSumB.release();
    }

    // The contrast centered at 32768, scaled by the gain
    scopeone::core::ProcessingResult IscatModule::contrastOutput(
        const cv::Mat& input,
        const cv::Mat& background,
        const scopeone::core::ImageFrame& frame)
    {
        cv::Mat safeBackground;
        cv::max(background, 1.0f, safeBackground);
        cv::Mat contrast;
        cv::subtract(input, background, contrast);
        cv::divide(contrast, safeBackground, contrast);
        cv::Mat outputValues = contrast * (m_contrastGain * 32768.0) + 32768.0;
        cv::max(outputValues, 0.0f, outputValues);
        cv::min(outputValues, 65535.0f, outputValues);
        return {makeMono16Output(frame, outputValues), {}};
    }

    scopeone::core::ProcessingResult IscatModule::processFlatField(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        cv::Mat background;
        cv::GaussianBlur(input, background, cv::Size(0, 0), m_blurSigma);
        return contrastOutput(input, background, frame);
    }

    // The reference is the mean of the frames after Capture Reference, unblurred so that the
    // static scattering of the coverslip cancels and particles that land later stay in the image
    scopeone::core::ProcessingResult IscatModule::processReference(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_captureReference)
        {
            m_referenceSum.release();
            m_referenceCount = 0;
            m_reference.release();
            m_captureReference = false;
        }
        if (!m_reference.empty())
        {
            return contrastOutput(input, m_reference, frame);
        }

        if (m_referenceSum.empty())
        {
            input.copyTo(m_referenceSum);
        }
        else
        {
            m_referenceSum += input;
        }
        if (++m_referenceCount < m_frames)
        {
            return {frame, {}};
        }
        m_reference = m_referenceSum / m_frames;
        m_referenceSum.release();
        return contrastOutput(input, m_reference, frame);
    }

    scopeone::core::ProcessingResult IscatModule::processDra(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_draBatchA.size() < static_cast<size_t>(m_frames))
        {
            m_draBatchA.push_back(input.clone());
            if (m_draSumA.empty())
            {
                input.copyTo(m_draSumA);
            }
            else
            {
                m_draSumA += input;
            }
            return {frame, {}};
        }

        if (m_draBatchB.size() < static_cast<size_t>(m_frames))
        {
            m_draBatchB.push_back(input.clone());
            if (m_draSumB.empty())
            {
                input.copyTo(m_draSumB);
            }
            else
            {
                m_draSumB += input;
            }
            if (m_draBatchB.size() < static_cast<size_t>(m_frames))
            {
                return {frame, {}};
            }

            return contrastOutput(m_draSumB / m_frames, m_draSumA / m_frames, frame);
        }

        const cv::Mat& oldestA = m_draBatchA.front();
        const cv::Mat& bridge = m_draBatchB.front();
        m_draSumA += bridge - oldestA;
        m_draSumB += input - bridge;
        m_draBatchA.pop_front();
        m_draBatchA.push_back(bridge);
        m_draBatchB.pop_front();
        m_draBatchB.push_back(input.clone());

        return contrastOutput(m_draSumB / m_frames, m_draSumA / m_frames, frame);
    }

    scopeone::core::ProcessingResult IscatModule::process(
        const scopeone::core::ImageFrame& frame,
        int)
    {
        if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16()))
        {
            return {scopeone::core::ImageFrame{}, QStringLiteral("Unsupported iSCAT input frame")};
        }

        const int sourceType = frame.isMono16() ? CV_16UC1 : CV_8UC1;
        const cv::Mat source(frame.height,
                             frame.width,
                             sourceType,
                             const_cast<char*>(frame.bytes.constData()),
                             frame.stride);
        cv::Mat input;
        source.convertTo(input, CV_32F, frame.isMono8() ? 257.0 : 1.0);
        if (m_stateSize != input.size())
        {
            clearState();
            m_stateSize = input.size();
        }

        switch (m_mode)
        {
        case 0:
            return processFlatField(input, frame);
        case 1:
            return processReference(input, frame);
        case 2:
            return processDra(input, frame);
        }
        return {};
    }
}

namespace scopeone::core::internal
{
    SCOPEONE_REGISTER_PROCESSING_MODULE(
        IscatModule,
        QStringLiteral("iscat.processing"),
        QStringLiteral("iSCAT Processing"),
        1,
        {choiceParameter("mode", "Mode", 0, {"Flat Field", "Reference", "DRA"}),
         shownWhen(integerParameter("frames", "Frames", 16, 1, 500), "mode", {1, 2}),
         shownWhen(realParameter("blur_sigma", "Blur sigma", 15.0, 1.0, 100.0, 0.5, 1), "mode", {0}),
         shownWhen(booleanParameter("capture_reference", "Capture Reference", false), "mode", {1}),
         realParameter("contrast_gain", "Contrast gain", 20.0, 1.0, 500.0, 1.0, 1)},
        true)
}
