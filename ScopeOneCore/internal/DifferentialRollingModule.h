#pragma once

#include "internal/ProcessingModule.h"

#include <QHash>
#include <QMutex>

#include <deque>
#include <vector>

namespace scopeone::core::internal {

class DifferentialRollingModule : public ProcessingModule
{
    Q_OBJECT

public:
    explicit DifferentialRollingModule(QObject* parent = nullptr);

    bool process(const ModuleInput& in, ModuleOutput& out) override;
    QString getModuleName() const override { return "Differential Rolling"; }

    QVariantMap getParameters() const override;
    void setParameters(const QVariantMap& params) override;

    void resetBuffer();

    struct CameraState
    {
        int width{0};
        int height{0};
        std::deque<ImageFrame> batchA;
        std::deque<ImageFrame> batchB;
        std::vector<int> sumA;
        std::vector<int> sumB;
    };

private:
    int m_batchSize{16};
    bool m_normalize{true};
    QHash<QString, CameraState> m_states;
    QMutex m_mutex;
};

} // namespace scopeone::core::internal
