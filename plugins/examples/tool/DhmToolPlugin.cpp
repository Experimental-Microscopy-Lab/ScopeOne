#include "DhmReconstruction.h"
#include "scopeone/ToolFrameStream.h"
#include "scopeone/ToolPlugin.h"
#include "scopeone/ToolTask.h"

#include <QFormLayout>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <memory>

namespace
{
    class DhmTool final : public QWidget
    {
    public:
        DhmTool(scopeone::ui::ScopeOneToolContext& context, QWidget* parent)
            : QWidget(parent), m_context(context)
        {
            setWindowFlag(Qt::Window, true);
            setWindowTitle(QStringLiteral("DHM Reconstruction"));
            resize(320, 190);

            auto* layout = new QVBoxLayout(this);
            auto* form = new QFormLayout();
            m_offsetX = createSpinBox(-10000, 10000, 48);
            m_offsetY = createSpinBox(-10000, 10000, 32);
            m_radius = createSpinBox(1, 10000, 24);
            form->addRow(QStringLiteral("Sideband X"), m_offsetX);
            form->addRow(QStringLiteral("Sideband Y"), m_offsetY);
            form->addRow(QStringLiteral("Radius"), m_radius);
            layout->addLayout(form);

            m_liveCheckBox = new QCheckBox(QStringLiteral("Live reconstruction"), this);
            m_liveCheckBox->setChecked(true);
            layout->addWidget(m_liveCheckBox);

            auto* buttons = new QHBoxLayout();
            m_reconstructButton = new QPushButton(QStringLiteral("Reconstruct once"), this);
            m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
            m_cancelButton->setEnabled(false);
            buttons->addWidget(m_reconstructButton);
            buttons->addWidget(m_cancelButton);
            layout->addLayout(buttons);

            m_status = new QLabel(QStringLiteral("Ready"), this);
            layout->addWidget(m_status);

            m_stream = new scopeone::ui::ScopeOneToolFrameStream(m_context.core(), this);

            connect(m_reconstructButton, &QPushButton::clicked, this,
                    [this]() { startReconstruction(); });
            connect(m_cancelButton, &QPushButton::clicked, this,
                    [this]()
                    {
                        m_liveCheckBox->setChecked(false);
                        if (m_task)
                        {
                            m_task->cancel();
                        }
                    });
            connect(m_stream, &scopeone::ui::ScopeOneToolFrameStream::frameReady,
                    this, [this](const scopeone::core::ImageFrame& frame)
            {
                if (!m_liveCheckBox->isChecked() || !frame.isValid())
                {
                    return;
                }
                if (m_sourceId.isEmpty())
                {
                    m_sourceId = frame.cameraId;
                    m_stream->setSourceId(m_sourceId);
                }
                queueFrame(frame);
            });
            connect(m_liveCheckBox, &QCheckBox::toggled, this, [this](bool enabled)
            {
                m_stream->setEnabled(enabled);
                if (!enabled)
                {
                    m_status->setText(m_task ? QStringLiteral("Finishing current frame")
                                             : QStringLiteral("Ready"));
                    return;
                }
                const auto frame = m_context.currentFrame();
                if (frame.isValid()
                    && scopeone::core::ScopeOneCore::isRawLayerKey(
                        m_context.currentLayerKey()))
                {
                    m_sourceId = frame.cameraId;
                    m_stream->setSourceId(m_sourceId);
                    queueFrame(frame);
                }
            });

            const auto frame = m_context.currentFrame();
            if (frame.isValid()
                && scopeone::core::ScopeOneCore::isRawLayerKey(m_context.currentLayerKey()))
            {
                m_sourceId = frame.cameraId;
                m_stream->setSourceId(m_sourceId);
                queueFrame(frame);
            }
        }

    private:
        static QSpinBox* createSpinBox(int minimum, int maximum, int value)
        {
            auto* spinBox = new QSpinBox();
            spinBox->setRange(minimum, maximum);
            spinBox->setValue(value);
            return spinBox;
        }

        void startReconstruction()
        {
            const auto input = m_context.currentFrame();
            if (!input.isValid())
            {
                m_status->setText(QStringLiteral("No image loaded"));
                return;
            }

            m_sourceId = input.cameraId;
            m_stream->setSourceId(m_sourceId);
            queueFrame(input);
        }

        void queueFrame(const scopeone::core::ImageFrame& input)
        {
            m_stream->setProcessing(true);
            const int offsetX = m_offsetX->value();
            const int offsetY = m_offsetY->value();
            const int radius = std::min(m_radius->value(),
                                        std::max(1, std::min(input.width, input.height) / 2));
            m_result.reset();

            m_task = new scopeone::ui::ScopeOneToolTask(
                [this, input, offsetX, offsetY, radius](const std::atomic_bool& cancel,
                                                         const std::function<void(int)>& progress)
                {
                    auto result = scopeone::dhm::reconstructPhase(input,
                                                                   offsetX,
                                                                   offsetY,
                                                                   radius,
                                                                   cancel,
                                                                   progress);
                    if (result.isValid())
                    {
                        m_result = std::make_shared<scopeone::core::ImageFrame>(std::move(result));
                    }
                },
                this);

            connect(m_task, &scopeone::ui::ScopeOneToolTask::progressChanged,
                    this, [this](int percent)
            {
                m_status->setText(QStringLiteral("Reconstructing %1%%").arg(percent));
            });
            connect(m_task, &scopeone::ui::ScopeOneToolTask::finished, this,
                    [this]() { publishResult(); });
            connect(m_task, &scopeone::ui::ScopeOneToolTask::canceled, this,
                    [this]() { finishTask(QStringLiteral("Canceled")); });
            connect(m_task, &scopeone::ui::ScopeOneToolTask::failed, this,
                    [this](const QString& message) { finishTask(message); });

            m_reconstructButton->setEnabled(false);
            m_cancelButton->setEnabled(true);
            m_status->setText(QStringLiteral("Reconstructing 0%"));
            m_task->start();
        }

        void publishResult()
        {
            if (!m_result)
            {
                finishTask(QStringLiteral("Reconstruction failed"));
                return;
            }
            const auto stored = m_context.publishToolStreamFrame(
                QStringLiteral("dhm.phase"), *m_result, QStringLiteral("DHM Phase"));
            if (stored.isValid())
            {
                if (!m_layersPresented)
                {
                    QStringList layers;
                    const QString rawLayer = scopeone::core::ScopeOneCore::rawLayerKey(m_sourceId);
                    if (m_context.core().graphFrame(rawLayer).isValid())
                    {
                        layers.append(rawLayer);
                    }
                    layers.append(scopeone::core::ScopeOneCore::toolLayerKey(stored.cameraId));
                    m_context.showLayers(layers, layers.size() > 1);
                    m_layersPresented = true;
                }
                finishTask(QStringLiteral("DHM phase ready"));
            }
            else
            {
                finishTask(QStringLiteral("Failed to publish DHM phase"));
            }
        }

        void finishTask(const QString& status)
        {
            m_status->setText(status);
            m_reconstructButton->setEnabled(true);
            m_cancelButton->setEnabled(false);
            if (m_task)
            {
                m_task->deleteLater();
                m_task = nullptr;
            }
            if (!m_liveCheckBox->isChecked())
            {
                m_stream->clearPendingFrame();
            }
            m_stream->setProcessing(false);
        }

        scopeone::ui::ScopeOneToolContext& m_context;
        QSpinBox* m_offsetX{nullptr};
        QSpinBox* m_offsetY{nullptr};
        QSpinBox* m_radius{nullptr};
        QCheckBox* m_liveCheckBox{nullptr};
        QPushButton* m_reconstructButton{nullptr};
        QPushButton* m_cancelButton{nullptr};
        QLabel* m_status{nullptr};
        scopeone::ui::ScopeOneToolTask* m_task{nullptr};
        scopeone::ui::ScopeOneToolFrameStream* m_stream{nullptr};
        std::shared_ptr<scopeone::core::ImageFrame> m_result;
        QString m_sourceId;
        bool m_layersPresented{false};
    };

    class DhmToolPlugin final : public QObject,
                                public scopeone::ui::ScopeOneToolPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneToolPlugin_iid FILE "dhm_plugin.json")
        Q_INTERFACES(scopeone::ui::ScopeOneToolPlugin)

    public:
        QList<scopeone::ui::ToolDescriptor> tools() const override
        {
            return {{QStringLiteral("scopeone.dhm_reconstruction"),
                     QStringLiteral("DHM Reconstruction"),
                     QStringLiteral("Reconstruction"),
                     scopeone::ui::ToolWindowMode::ModelessSingleton,
                     false}};
        }

        QWidget* createTool(const QString& toolId,
                            scopeone::ui::ScopeOneToolContext& context,
                            QWidget* parent) override
        {
            return toolId == QStringLiteral("scopeone.dhm_reconstruction")
                       ? new DhmTool(context, parent)
                       : nullptr;
        }
    };
}

#include "DhmToolPlugin.moc"
