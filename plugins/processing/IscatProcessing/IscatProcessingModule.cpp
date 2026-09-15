#include "IscatProcessingModule.h"

#include <QByteArray>
#include <algorithm>
#include <cstddef>
#include <opencv2/imgproc.hpp>
#include <utility>
#include <vector>

namespace scopeone::iscat
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

    QString IscatProcessingModule::id() const
    {
        return QStringLiteral("iscat.processing");
    }

    QString IscatProcessingModule::name() const
    {
        return QStringLiteral("iSCAT Processing");
    }

    QVariantMap IscatProcessingModule::parameters() const
    {
        return {{QStringLiteral("mode"), m_mode},
                {QStringLiteral("blur_sigma"), m_blurSigma},
                {QStringLiteral("median_window"), m_medianWindow},
                {QStringLiteral("ema_alpha"), m_emaAlpha},
                {QStringLiteral("capture_reference"), m_captureReference},
                {QStringLiteral("batch_size"), m_batchSize},
                {QStringLiteral("contrast_gain"), m_contrastGain},
                {QStringLiteral("enable_high_pass"), m_enableHighPass},
                {QStringLiteral("high_pass_sigma"), m_highPassSigma},
                {QStringLiteral("output_mode"), m_outputMode}};
    }

    void IscatProcessingModule::setParameters(const QVariantMap& parameters)
    {
        const int previousMode = m_mode;
        const double previousBlurSigma = m_blurSigma;
        const int previousMedianWindow = m_medianWindow;
        const int previousBatchSize = m_batchSize;

        if (parameters.contains(QStringLiteral("mode")))
        {
            m_mode = qBound(0, parameters.value(QStringLiteral("mode")).toInt(), 4);
        }
        if (parameters.contains(QStringLiteral("blur_sigma")))
        {
            m_blurSigma = qBound(1.0,
                                  parameters.value(QStringLiteral("blur_sigma")).toDouble(),
                                  100.0);
        }
        if (parameters.contains(QStringLiteral("median_window")))
        {
            m_medianWindow = qBound(3,
                                    parameters.value(QStringLiteral("median_window")).toInt(),
                                    101);
            if ((m_medianWindow % 2) == 0)
            {
                ++m_medianWindow;
            }
        }
        if (parameters.contains(QStringLiteral("ema_alpha")))
        {
            m_emaAlpha = qBound(0.001,
                                parameters.value(QStringLiteral("ema_alpha")).toDouble(),
                                0.5);
        }
        if (parameters.contains(QStringLiteral("capture_reference")))
        {
            m_captureReference = parameters.value(QStringLiteral("capture_reference")).toBool();
        }
        if (parameters.contains(QStringLiteral("batch_size")))
        {
            m_batchSize = qBound(1,
                                 parameters.value(QStringLiteral("batch_size")).toInt(),
                                 500);
        }
        if (parameters.contains(QStringLiteral("contrast_gain")))
        {
            m_contrastGain = qBound(1.0,
                                    parameters.value(QStringLiteral("contrast_gain")).toDouble(),
                                    500.0);
        }
        if (parameters.contains(QStringLiteral("enable_high_pass")))
        {
            m_enableHighPass = parameters.value(QStringLiteral("enable_high_pass")).toBool();
        }
        if (parameters.contains(QStringLiteral("high_pass_sigma")))
        {
            m_highPassSigma = qBound(2.0,
                                     parameters.value(QStringLiteral("high_pass_sigma")).toDouble(),
                                     200.0);
        }
        if (parameters.contains(QStringLiteral("output_mode")))
        {
            m_outputMode = qBound(0,
                                  parameters.value(QStringLiteral("output_mode")).toInt(),
                                  2);
        }

        if (previousMode != m_mode
            || previousBlurSigma != m_blurSigma
            || previousMedianWindow != m_medianWindow
            || previousBatchSize != m_batchSize)
        {
            clearState();
        }
    }

    std::unique_ptr<scopeone::core::ProcessingModule> IscatProcessingModule::createRuntime() const
    {
        auto module = std::make_unique<IscatProcessingModule>();
        module->setParameters(parameters());
        return module;
    }

    bool IscatProcessingModule::resetState()
    {
        clearState();
        return true;
    }

    void IscatProcessingModule::clearState()
    {
        m_medianBuffer.clear();
        m_lockedMedianBackground.release();
        m_emaAccumulator.release();
        m_snapshotBackground.release();
        m_draBatchA.clear();
        m_draBatchB.clear();
        m_draSumA.release();
        m_draSumB.release();
    }

    cv::Mat IscatProcessingModule::computePixelMedian(const std::deque<cv::Mat>& frames) const
    {
        cv::Mat median(frames.front().size(), CV_32F);
        cv::parallel_for_(cv::Range(0, median.rows),
                          [&](const cv::Range& range)
                          {
                              std::vector<float> values;
                              values.reserve(frames.size());
                              for (int y = range.start; y < range.end; ++y)
                              {
                                  float* outputRow = median.ptr<float>(y);
                                  for (int x = 0; x < median.cols; ++x)
                                  {
                                      values.clear();
                                      for (const cv::Mat& frame : frames)
                                      {
                                          values.push_back(frame.at<float>(y, x));
                                      }
                                      const auto middle = values.begin()
                                          + static_cast<std::ptrdiff_t>(values.size() / 2);
                                      std::nth_element(values.begin(), middle, values.end());
                                      outputRow[x] = *middle;
                                  }
                              }
                          });
        return median;
    }

    scopeone::core::ProcessingResult IscatProcessingModule::finalizeContrastOutput(
        const cv::Mat& input,
        const cv::Mat& background,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_outputMode == 2)
        {
            cv::Mat outputValues;
            background.convertTo(outputValues, CV_32F);
            cv::max(outputValues, 0.0f, outputValues);
            cv::min(outputValues, 65535.0f, outputValues);
            return {makeMono16Output(frame, outputValues), {}};
        }

        cv::Mat safeBackground;
        cv::max(background, 1.0f, safeBackground);
        cv::Mat contrast;
        cv::subtract(input, background, contrast);
        cv::divide(contrast, safeBackground, contrast);
        if (m_enableHighPass)
        {
            cv::Mat lowFrequency;
            cv::GaussianBlur(contrast,
                             lowFrequency,
                             cv::Size(0, 0),
                             m_highPassSigma);
            contrast -= lowFrequency;
        }

        cv::Mat outputValues;
        if (m_outputMode == 0)
        {
            outputValues = contrast * (m_contrastGain * 32768.0) + 32768.0;
        }
        else
        {
            cv::absdiff(contrast, cv::Scalar::all(0.0), contrast);
            outputValues = contrast * (m_contrastGain * 65535.0);
        }
        cv::max(outputValues, 0.0f, outputValues);
        cv::min(outputValues, 65535.0f, outputValues);
        return {makeMono16Output(frame, outputValues), {}};
    }

    scopeone::core::ProcessingResult IscatProcessingModule::processFlatField(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        cv::Mat background;
        cv::GaussianBlur(input,
                         background,
                         cv::Size(0, 0),
                         m_blurSigma);
        return finalizeContrastOutput(input, background, frame);
    }

    scopeone::core::ProcessingResult IscatProcessingModule::processTemporalMedian(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_captureReference)
        {
            m_lockedMedianBackground.release();
            m_medianBuffer.clear();
            m_captureReference = false;
        }
        if (!m_lockedMedianBackground.empty())
        {
            return finalizeContrastOutput(input, m_lockedMedianBackground, frame);
        }

        m_medianBuffer.push_back(input.clone());
        if (m_medianBuffer.size() < static_cast<size_t>(m_medianWindow))
        {
            return {frame, {}};
        }

        cv::Mat median = computePixelMedian(m_medianBuffer);
        cv::GaussianBlur(median,
                         m_lockedMedianBackground,
                         cv::Size(0, 0),
                         m_blurSigma);
        m_medianBuffer.clear();
        return finalizeContrastOutput(input, m_lockedMedianBackground, frame);
    }

    scopeone::core::ProcessingResult IscatProcessingModule::processEma(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_emaAccumulator.empty())
        {
            input.copyTo(m_emaAccumulator);
        }
        else
        {
            m_emaAccumulator = (1.0 - m_emaAlpha) * m_emaAccumulator
                + m_emaAlpha * input;
        }

        cv::Mat background;
        cv::GaussianBlur(m_emaAccumulator,
                         background,
                         cv::Size(0, 0),
                         m_blurSigma);
        return finalizeContrastOutput(input, background, frame);
    }

    scopeone::core::ProcessingResult IscatProcessingModule::processSnapshot(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_snapshotBackground.empty() || m_captureReference)
        {
            cv::GaussianBlur(input,
                             m_snapshotBackground,
                             cv::Size(0, 0),
                             m_blurSigma);
            m_captureReference = false;
        }
        return finalizeContrastOutput(input, m_snapshotBackground, frame);
    }

    scopeone::core::ProcessingResult IscatProcessingModule::processDra(
        const cv::Mat& input,
        const scopeone::core::ImageFrame& frame)
    {
        if (m_draBatchA.size() < static_cast<size_t>(m_batchSize))
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

        if (m_draBatchB.size() < static_cast<size_t>(m_batchSize))
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
            if (m_draBatchB.size() < static_cast<size_t>(m_batchSize))
            {
                return {frame, {}};
            }

            return finalizeContrastOutput(m_draSumB / m_batchSize,
                                          m_draSumA / m_batchSize,
                                          frame);
        }

        const cv::Mat& oldestA = m_draBatchA.front();
        const cv::Mat& bridge = m_draBatchB.front();
        m_draSumA += bridge - oldestA;
        m_draSumB += input - bridge;
        m_draBatchA.pop_front();
        m_draBatchA.push_back(bridge);
        m_draBatchB.pop_front();
        m_draBatchB.push_back(input.clone());

        return finalizeContrastOutput(m_draSumB / m_batchSize,
                                      m_draSumA / m_batchSize,
                                      frame);
    }

    scopeone::core::ProcessingResult IscatProcessingModule::process(
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
            return processTemporalMedian(input, frame);
        case 2:
            return processEma(input, frame);
        case 3:
            return processSnapshot(input, frame);
        case 4:
            return processDra(input, frame);
        }
        return {};
    }
}
