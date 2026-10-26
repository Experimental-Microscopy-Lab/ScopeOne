#pragma once

#include "internal/ProcessingModule.h"
#include "internal/ProcessingModuleRegistry.h"

#include <deque>
#include <opencv2/core.hpp>

namespace scopeone::core::internal
{
    // One image operation of the menus and of the pipeline, such as math, filters, thresholds
    // and binary operations. The id picks the operation and the parameters follow its descriptor.
    class ImageOperationModule : public ProcessingModule
    {
    public:
        ImageOperationModule(QString id, QString name, QVariantMap defaults);

        QString id() const override { return m_id; }
        QString name() const override { return m_name; }
        QVariantMap parameters() const override { return m_params; }
        void setParameters(const QVariantMap& params) override;
        std::unique_ptr<ProcessingModule> createRuntime() const override;
        bool resetState() override;
        ProcessingResult process(const ImageFrame& frame, int processingBitDepth) override;

    private:
        ProcessingResult rollingProjection(const ImageFrame& frame);

        QString m_id;
        QString m_name;
        QVariantMap m_params;
        std::deque<cv::Mat> m_window;
        cv::Mat m_sum;
        int m_windowWidth{0};
        int m_windowHeight{0};
        int m_windowType{0};
    };

    void registerImageOperationModules(ProcessingModuleRegistry& registry);
} // namespace scopeone::core::internal
