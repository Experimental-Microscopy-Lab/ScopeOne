#include "ImageProcessingWidget.h"

#include "ImageWorkspace.h"

#include "scopeone/ScopeOneCore.h"
#include "scopeone/ImageSceneModel.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineF>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPalette>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QRadioButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QDebug>

#include <cmath>
#include <functional>

namespace scopeone::ui
{
    namespace
    {
        using ProcessingModuleDescriptor = scopeone::core::ProcessingModuleDescriptor;
        using ProcessingModuleInfo = scopeone::core::ScopeOneCore::ProcessingModuleInfo;
        using ProcessingParameterDescriptor = scopeone::core::ProcessingParameterDescriptor;
        using ProcessingParameterType = scopeone::core::ProcessingParameterType;

        class MaskPreviewWidget final : public QWidget
        {
        public:
            explicit MaskPreviewWidget(QWidget* parent)
                : QWidget(parent)
            {
                setMinimumSize(220, 220);
                setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
                setMouseTracking(true);
            }

            void setParameters(const QVariantMap& parameters)
            {
                m_parameters = parameters;
                update();
            }

            void setParametersChanged(std::function<void(const QVariantMap&)> callback)
            {
                m_parametersChanged = std::move(callback);
            }

        protected:
            void paintEvent(QPaintEvent*) override
            {
                QPainter painter(this);
                painter.fillRect(rect(), QColor(24, 27, 31));
                painter.setRenderHint(QPainter::Antialiasing);
                const QRectF area = plotArea();
                painter.setPen(QColor(82, 88, 96));
                painter.drawLine(area.center().x(), area.top(), area.center().x(), area.bottom());
                painter.drawLine(area.left(), area.center().y(), area.right(), area.center().y());

                const QPointF center = toWidget(m_parameters.value(QStringLiteral("center_x")).toDouble(),
                                                m_parameters.value(QStringLiteral("center_y")).toDouble());
                const double sizeX = m_parameters.value(QStringLiteral("size_x"), 0.1).toDouble() * area.width();
                const double sizeY = m_parameters.value(QStringLiteral("size_y"), 0.1).toDouble() * area.height();
                const double rotation = m_parameters.value(QStringLiteral("rotation")).toDouble();
                const int shape = m_parameters.value(QStringLiteral("shape")).toInt();
                painter.save();
                painter.translate(center);
                painter.rotate(-rotation);
                painter.setPen(QPen(QColor(90, 210, 255), 2));
                painter.setBrush(QColor(90, 210, 255, 70));
                if (shape == 1)
                {
                    painter.drawRect(QRectF(-sizeX / 2.0, -sizeY / 2.0, sizeX, sizeY));
                }
                else
                {
                    painter.drawEllipse(QRectF(-sizeX / 2.0, -sizeY / 2.0, sizeX, sizeY));
                    if (shape == 2)
                    {
                        const double inner = m_parameters.value(QStringLiteral("inner_size")).toDouble()
                                              * area.width();
                        painter.setBrush(QColor(24, 27, 31));
                        painter.drawEllipse(QRectF(-inner / 2.0, -inner / 2.0, inner, inner));
                    }
                }
                painter.restore();

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 120, 90));
                painter.drawEllipse(center, 4, 4);
                painter.setBrush(QColor(90, 210, 255));
                painter.drawEllipse(toWidget(m_parameters.value(QStringLiteral("center_x")).toDouble()
                                                 + m_parameters.value(QStringLiteral("size_x"), 0.1).toDouble() / 2.0,
                                             m_parameters.value(QStringLiteral("center_y")).toDouble()),
                                    5, 5);
            }

            void mousePressEvent(QMouseEvent* event) override
            {
                const QPointF center = toWidget(m_parameters.value(QStringLiteral("center_x")).toDouble(),
                                                m_parameters.value(QStringLiteral("center_y")).toDouble());
                const QPointF handle = toWidget(m_parameters.value(QStringLiteral("center_x")).toDouble()
                                                    + m_parameters.value(QStringLiteral("size_x"), 0.1).toDouble() / 2.0,
                                                m_parameters.value(QStringLiteral("center_y")).toDouble());
                if (QLineF(event->position(), center).length() < 14.0)
                {
                    m_dragMode = DragMode::Move;
                }
                else if (QLineF(event->position(), handle).length() < 14.0)
                {
                    m_dragMode = DragMode::Resize;
                }
                else
                {
                    m_dragMode = DragMode::None;
                }
                m_lastPosition = event->position();
            }

            void mouseMoveEvent(QMouseEvent* event) override
            {
                if (m_dragMode == DragMode::None)
                {
                    return;
                }
                QVariantMap parameters = m_parameters;
                if (m_dragMode == DragMode::Move)
                {
                    const QPointF delta = event->position() - m_lastPosition;
                    parameters[QStringLiteral("center_x")] = qBound(-0.5,
                                                                    parameters.value(QStringLiteral("center_x")).toDouble()
                                                                        + delta.x() / plotArea().width(),
                                                                    0.5);
                    parameters[QStringLiteral("center_y")] = qBound(-0.5,
                                                                    parameters.value(QStringLiteral("center_y")).toDouble()
                                                                        + delta.y() / plotArea().height(),
                                                                    0.5);
                }
                else
                {
                    const QPointF center = toWidget(parameters.value(QStringLiteral("center_x")).toDouble(),
                                                    parameters.value(QStringLiteral("center_y")).toDouble());
                    parameters[QStringLiteral("size_x")] = qBound(0.001,
                                                                  2.0 * std::abs(event->position().x() - center.x())
                                                                      / plotArea().width(),
                                                                  1.0);
                    parameters[QStringLiteral("size_y")] = qBound(0.001,
                                                                  2.0 * std::abs(event->position().y() - center.y())
                                                                      / plotArea().height(),
                                                                  1.0);
                }
                m_lastPosition = event->position();
                if (m_parametersChanged)
                {
                    m_parametersChanged(parameters);
                }
            }

            void mouseReleaseEvent(QMouseEvent*) override
            {
                m_dragMode = DragMode::None;
            }

        private:
            enum class DragMode
            {
                None,
                Move,
                Resize
            };

            QPointF toWidget(double x, double y) const
            {
                const QRectF area = plotArea();
                return {area.left() + (x + 0.5) * area.width(),
                        area.top() + (y + 0.5) * area.height()};
            }

            QRectF plotArea() const
            {
                const qreal side = qMin(width(), height());
                return QRectF((width() - side) / 2.0,
                              (height() - side) / 2.0,
                              side,
                              side);
            }

            QVariantMap m_parameters;
            std::function<void(const QVariantMap&)> m_parametersChanged;
            QPointF m_lastPosition;
            DragMode m_dragMode{DragMode::None};
        };

        void configureSpinBox(QAbstractSpinBox* spinBox)
        {
            spinBox->setKeyboardTracking(false);
            spinBox->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
        }

        class ModuleConfigWidget final : public QWidget
        {
        public:
            ModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                               int moduleIndex,
                               const ProcessingModuleInfo& info,
                               QWidget* parent)
                : QWidget(parent), m_core(core), m_moduleIndex(moduleIndex)
            {
                auto* layout = new QVBoxLayout(this);
                auto* group = new QGroupBox(info.name(), this);
                auto* form = new QFormLayout(group);
                if (info.descriptor().id == QStringLiteral("mask"))
                {
                    auto* preview = new MaskPreviewWidget(group);
                    preview->setParameters(info.parameters());
                    preview->setParametersChanged([this, preview](const QVariantMap& parameters)
                    {
                        preview->setParameters(parameters);
                        m_core->setProcessingModuleParameters(m_moduleIndex, parameters);
                    });
                    form->addRow(preview);
                    m_maskPreview = preview;
                }
                for (const ProcessingParameterDescriptor& parameter : info.descriptor().parameters)
                {
                    QWidget* editor = createEditor(parameter,
                                                   info.parameters().value(parameter.key,
                                                                           parameter.defaultValue),
                                                   group);
                    m_editors.insert(parameter.key, editor);
                    m_descriptors.insert(parameter.key, parameter);
                    form->addRow(parameter.name + QLatin1Char(':'), editor);
                }
                layout->addWidget(group);

                if (info.descriptor().resettable)
                {
                    auto* resetButton = new QPushButton(tr("Reset State"), this);
                    connect(resetButton, &QPushButton::clicked, this, [this]()
                    {
                        m_core->resetProcessingModuleState(m_moduleIndex);
                    });
                    layout->addWidget(resetButton);
                }
                layout->addStretch();
            }

            void setParameters(const QVariantMap& parameters)
            {
                for (auto it = m_descriptors.constBegin(); it != m_descriptors.constEnd(); ++it)
                {
                    QWidget* editor = m_editors.value(it.key());
                    const QVariant value = parameters.value(it.key(), it->defaultValue);
                    const QSignalBlocker blocker(editor);
                    switch (it->type)
                    {
                    case ProcessingParameterType::Integer:
                        qobject_cast<QSpinBox*>(editor)->setValue(value.toInt());
                        break;
                    case ProcessingParameterType::Real:
                        qobject_cast<QDoubleSpinBox*>(editor)->setValue(value.toDouble());
                        break;
                    case ProcessingParameterType::Boolean:
                        qobject_cast<QCheckBox*>(editor)->setChecked(value.toBool());
                        break;
                    case ProcessingParameterType::Choice:
                    {
                        auto* combo = qobject_cast<QComboBox*>(editor);
                        combo->setCurrentIndex(combo->findData(value));
                        break;
                    }
                    }
                }
                if (m_maskPreview)
                {
                    m_maskPreview->setParameters(parameters);
                }
            }

        private:
            QWidget* createEditor(const ProcessingParameterDescriptor& descriptor,
                                  const QVariant& value,
                                  QWidget* parent)
            {
                switch (descriptor.type)
                {
                case ProcessingParameterType::Integer:
                {
                    auto* editor = new QSpinBox(parent);
                    editor->setRange(descriptor.minimum.toInt(), descriptor.maximum.toInt());
                    editor->setSingleStep(qMax(1, descriptor.step.toInt()));
                    editor->setValue(value.toInt());
                    configureSpinBox(editor);
                    connect(editor, QOverload<int>::of(&QSpinBox::valueChanged),
                            this, [this]() { apply(); });
                    return editor;
                }
                case ProcessingParameterType::Real:
                {
                    auto* editor = new QDoubleSpinBox(parent);
                    editor->setRange(descriptor.minimum.toDouble(), descriptor.maximum.toDouble());
                    editor->setSingleStep(descriptor.step.toDouble());
                    editor->setDecimals(descriptor.decimals);
                    editor->setValue(value.toDouble());
                    configureSpinBox(editor);
                    connect(editor, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                            this, [this]() { apply(); });
                    return editor;
                }
                case ProcessingParameterType::Boolean:
                {
                    auto* editor = new QCheckBox(parent);
                    editor->setChecked(value.toBool());
                    connect(editor, &QCheckBox::toggled, this, [this]() { apply(); });
                    return editor;
                }
                case ProcessingParameterType::Choice:
                {
                    auto* editor = new QComboBox(parent);
                    for (const auto& choice : descriptor.choices)
                    {
                        editor->addItem(choice.name, choice.value);
                    }
                    editor->setCurrentIndex(editor->findData(value));
                    connect(editor, QOverload<int>::of(&QComboBox::currentIndexChanged),
                            this, [this]() { apply(); });
                    return editor;
                }
                }
                return new QWidget(parent);
            }

            QVariant editorValue(const QString& key) const
            {
                QWidget* editor = m_editors.value(key);
                switch (m_descriptors.value(key).type)
                {
                case ProcessingParameterType::Integer:
                    return qobject_cast<QSpinBox*>(editor)->value();
                case ProcessingParameterType::Real:
                    return qobject_cast<QDoubleSpinBox*>(editor)->value();
                case ProcessingParameterType::Boolean:
                    return qobject_cast<QCheckBox*>(editor)->isChecked();
                case ProcessingParameterType::Choice:
                    return qobject_cast<QComboBox*>(editor)->currentData();
                }
                return {};
            }

            void apply()
            {
                QVariantMap parameters;
                for (auto it = m_descriptors.constBegin(); it != m_descriptors.constEnd(); ++it)
                {
                    parameters.insert(it.key(), editorValue(it.key()));
                }
                m_core->setProcessingModuleParameters(m_moduleIndex, parameters);
            }

            scopeone::core::ScopeOneCore* m_core;
            int m_moduleIndex;
            QHash<QString, QWidget*> m_editors;
            QHash<QString, ProcessingParameterDescriptor> m_descriptors;
            MaskPreviewWidget* m_maskPreview{nullptr};
        };
    }

    ImageProcessingWidget::ImageProcessingWidget(scopeone::core::ScopeOneCore* core,
                                                 ImageWorkspace* workspace,
                                                 QWidget* parent)
        : QWidget(parent), m_scopeonecore(core), m_workspace(workspace)
    {
        setAutoFillBackground(true);
        setBackgroundRole(QPalette::Window);
        m_processingRunning = core->isRealTimeProcessingEnabled();
        setupUI();

        connect(core, &scopeone::core::ScopeOneCore::processingModulesChanged,
                this, [this]()
                {
                    updateModuleList();
                    updateConfigWidget();
                    updateRunButtons();
                });
        connect(core, &scopeone::core::ScopeOneCore::processingModuleParametersChanged,
                this, [this](int moduleIndex)
                {
                    if (moduleIndex == m_moduleList->currentRow())
                    {
                        const auto modules = m_scopeonecore->processingModules();
                        if (moduleIndex >= 0 && moduleIndex < modules.size())
                        {
                            static_cast<ModuleConfigWidget*>(m_configStack->currentWidget())
                                ->setParameters(modules.at(moduleIndex).parameters());
                        }
                    }
                });
        connect(core, &scopeone::core::ScopeOneCore::processingSettingsChanged,
                this, [this]()
                {
                    updateProcessingSettings();
                    updateRunButtons();
                    syncProcessingState();
                });
        connect(core, &scopeone::core::ScopeOneCore::processingError,
                this, [](const QString& error)
                {
                    qWarning().noquote() << QStringLiteral("Processing error: %1").arg(error);
                });
        connect(core, &scopeone::core::ScopeOneCore::hardwareDevicesChanged,
                this, &ImageProcessingWidget::refreshSources);
        connect(core, &scopeone::core::ScopeOneCore::recordingSessionsChanged,
                this, &ImageProcessingWidget::refreshSources);
        connect(core->imageSceneModel(), &scopeone::core::ImageSceneModel::layersChanged,
                this, &ImageProcessingWidget::refreshSources);
        connect(core, &scopeone::core::ScopeOneCore::imageProcessingFinished,
                this, [this](quint64 requestId,
                             const QString& inputSourceId,
                             const scopeone::core::ImageFrame& frame,
                             const QString& error)
                {
                    if (requestId != m_offlineProcessingRequestId
                        || !m_directProcessingRequest)
                    {
                        return;
                    }
                    finishOfflineProcessing();
                    if (!error.isEmpty() || !frame.isValid())
                    {
                        QMessageBox::warning(this, tr("Processing Failed"), error);
                        return;
                    }
                    const QString outputSourceId = QStringLiteral("processed:%1")
                                                       .arg(scopeone::core::ScopeOneCore::sourceIdFromLayerKey(
                                                           inputSourceId));
                    const auto output = m_scopeonecore->publishStaticFrame(
                        outputSourceId, frame, tr("Processed Image"));
                    if (output.isValid())
                    {
                        emit processedLayerReady(
                            scopeone::core::ScopeOneCore::staticLayerKey(outputSourceId));
                    }
                });
        connect(core, &scopeone::core::ScopeOneCore::stackProcessingProgress,
                this, [this](quint64 requestId, qint64 completed, qint64 total)
                {
                    if (requestId == m_offlineProcessingRequestId)
                    {
                        m_processingProgress->setMaximum(static_cast<int>(total));
                        m_processingProgress->setValue(static_cast<int>(completed));
                    }
                });
        connect(core, &scopeone::core::ScopeOneCore::stackProcessingFinished,
                this, [this](quint64 requestId,
                             const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>& session,
                             const QString& error)
                {
                    if (requestId != m_offlineProcessingRequestId
                        || !m_directProcessingRequest)
                    {
                        return;
                    }
                    finishOfflineProcessing();
                    if (!error.isEmpty() || !session)
                    {
                        QMessageBox::warning(this, tr("Processing Failed"), error);
                        return;
                    }
                    emit processedStackReady(session);
                    refreshSources();
                });
        connect(workspace, &ImageWorkspace::activeDocumentChanged,
                this, [this]()
                {
                    refreshSources();
                    const int index = m_sourceCombo->findData(
                        m_workspace->activeDocumentId(), Qt::UserRole + 1);
                    if (index >= 0)
                    {
                        m_sourceCombo->setCurrentIndex(index);
                    }
                });
        connect(workspace, &ImageWorkspace::documentsChanged,
                this, &ImageProcessingWidget::refreshSources);
        connect(workspace, &ImageWorkspace::documentProcessingProgress,
                this, [this](quint64 requestId, qint64 completed, qint64 total)
                {
                    if (!m_directProcessingRequest
                        && requestId == m_offlineProcessingRequestId)
                    {
                        m_processingProgress->setMaximum(static_cast<int>(total));
                        m_processingProgress->setValue(static_cast<int>(completed));
                    }
                });
        connect(workspace, &ImageWorkspace::documentProcessingFinished,
                this, [this](quint64 requestId, const QString&, const QString& error)
                {
                    if (m_directProcessingRequest
                        || requestId != m_offlineProcessingRequestId)
                    {
                        return;
                    }
                    finishOfflineProcessing();
                    if (!error.isEmpty())
                    {
                        QMessageBox::warning(this, tr("Processing Failed"), error);
                    }
                });

        updateProcessingSettings();
        refreshSources();
        updateModuleList();
        updateConfigWidget();
        updateRunButtons();
    }

    void ImageProcessingWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        auto* scrollArea = new QScrollArea(this);
        scrollArea->setWidgetResizable(true);
        scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollArea->setFrameShape(QFrame::NoFrame);

        auto* content = new QWidget(scrollArea);
        content->setAutoFillBackground(true);
        content->setBackgroundRole(QPalette::Window);
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(5, 5, 5, 5);
        QWidget* pipelineGroup = setupModuleList();
        QWidget* parametersGroup = setupModuleConfig();
        setupExecutionControls();
        setupInputControls();
        contentLayout->addWidget(m_inputControlsWidget);
        contentLayout->addWidget(pipelineGroup);
        contentLayout->addWidget(parametersGroup, 1);
        contentLayout->addWidget(m_executionControlsWidget);
        scrollArea->setWidget(content);
        mainLayout->addWidget(scrollArea);
    }

    void ImageProcessingWidget::setupInputControls()
    {
        m_inputControlsWidget = new QGroupBox(tr("Pipeline Input"), this);
        auto* layout = new QGridLayout(m_inputControlsWidget);
        m_liveModeRadio = new QRadioButton(tr("Live Stream"), m_inputControlsWidget);
        m_staticModeRadio = new QRadioButton(tr("Image / Stack"), m_inputControlsWidget);
        m_liveModeRadio->setChecked(true);
        layout->addWidget(m_liveModeRadio, 0, 0);
        layout->addWidget(m_staticModeRadio, 0, 1);
        m_sourceLabel = new QLabel(m_inputControlsWidget);
        layout->addWidget(m_sourceLabel, 1, 0);
        m_liveSourceCombo = new QComboBox(m_inputControlsWidget);
        m_liveSourceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(m_liveSourceCombo, 1, 1, 1, 2);
        m_sourceCombo = new QComboBox(m_inputControlsWidget);
        m_sourceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(m_sourceCombo, 1, 1, 1, 2);
        m_rangeLabel = new QLabel(tr("Range"), m_inputControlsWidget);
        layout->addWidget(m_rangeLabel, 2, 0);
        m_offlineScopeCombo = new QComboBox(m_inputControlsWidget);
        m_offlineScopeCombo->addItem(tr("Current frame"), false);
        m_offlineScopeCombo->addItem(tr("Entire stack"), true);
        layout->addWidget(m_offlineScopeCombo, 2, 1, 1, 2);
        layout->setColumnStretch(2, 1);
        connect(m_liveModeRadio, &QRadioButton::toggled,
                this, &ImageProcessingWidget::onInputModeChanged);
        connect(m_staticModeRadio, &QRadioButton::toggled,
                this, &ImageProcessingWidget::onInputModeChanged);
        connect(m_liveSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]()
                {
                    m_scopeonecore->setRealTimeProcessingSource(
                        m_liveSourceCombo->currentData().toString());
                    updateRunButtons();
                });
        connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { updateRunButtons(); });
        connect(m_offlineScopeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { updateRunButtons(); });
        onInputModeChanged();
    }

    void ImageProcessingWidget::setupExecutionControls()
    {
        m_executionControlsWidget = new QGroupBox(tr("Pipeline Output & Execution"), this);
        auto* layout = new QGridLayout(m_executionControlsWidget);
        layout->addWidget(new QLabel(tr("Bit depth"), m_executionControlsWidget), 0, 0);
        m_processingBitDepthCombo = new QComboBox(m_executionControlsWidget);
        m_processingBitDepthCombo->addItem(
            tr("8-bit"), static_cast<int>(scopeone::core::ProcessingBitDepth::Bit8));
        m_processingBitDepthCombo->addItem(
            tr("16-bit"), static_cast<int>(scopeone::core::ProcessingBitDepth::Bit16));
        connect(m_processingBitDepthCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ImageProcessingWidget::onProcessingBitDepthChanged);
        layout->addWidget(m_processingBitDepthCombo, 0, 1);
        m_executeButton = new QPushButton(m_executionControlsWidget);
        m_cancelProcessingButton = new QPushButton(tr("Cancel"), m_executionControlsWidget);
        m_cancelProcessingButton->setEnabled(false);
        connect(m_executeButton, &QPushButton::clicked,
                this, [this]()
                {
                    if (m_liveModeRadio->isChecked())
                    {
                        onLiveProcessingToggled(!m_scopeonecore->isRealTimeProcessingEnabled());
                    }
                    else
                    {
                        onRunOfflineProcessing();
                    }
                });
        connect(m_cancelProcessingButton, &QPushButton::clicked,
                this, &ImageProcessingWidget::onCancelProcessing);
        layout->addWidget(m_executeButton, 1, 0, 1, 2);
        layout->addWidget(m_cancelProcessingButton, 1, 2);
        m_processingProgress = new QProgressBar(m_executionControlsWidget);
        m_processingProgress->setVisible(false);
        layout->addWidget(m_processingProgress, 2, 0, 1, 3);
    }

    void ImageProcessingWidget::onInputModeChanged()
    {
        const bool liveMode = m_liveModeRadio->isChecked();
        m_sourceLabel->setText(liveMode ? tr("Target") : tr("Source"));
        m_liveSourceCombo->setVisible(liveMode);
        m_sourceCombo->setVisible(!liveMode);
        m_rangeLabel->setVisible(!liveMode);
        m_offlineScopeCombo->setVisible(!liveMode);
        updateRunButtons();
    }

    QWidget* ImageProcessingWidget::setupModuleList()
    {
        auto* group = new QGroupBox(tr("Pipeline"), this);
        auto* layout = new QVBoxLayout(group);
        m_moduleList = new QListWidget(group);
        connect(m_moduleList, &QListWidget::currentRowChanged,
                this, &ImageProcessingWidget::onModuleSelectionChanged);
        connect(m_moduleList, &QListWidget::itemChanged,
                this, &ImageProcessingWidget::onModuleItemChanged);
        layout->addWidget(m_moduleList);
        auto* controls = new QHBoxLayout;
        m_moduleTypeCombo = new QComboBox(group);
        for (const ProcessingModuleDescriptor& descriptor : m_scopeonecore->availableProcessingModules())
        {
            m_moduleTypeCombo->addItem(descriptor.name, descriptor.id);
        }
        controls->addWidget(m_moduleTypeCombo);
        m_addModuleButton = new QPushButton(tr("Add"), group);
        m_removeModuleButton = new QPushButton(tr("Remove"), group);
        m_moveModuleUpButton = new QPushButton(tr("Up"), group);
        m_moveModuleDownButton = new QPushButton(tr("Down"), group);
        connect(m_addModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onAddModuleClicked);
        connect(m_removeModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onRemoveModuleClicked);
        connect(m_moveModuleUpButton, &QPushButton::clicked,
                this, &ImageProcessingWidget::onMoveModuleUpClicked);
        connect(m_moveModuleDownButton, &QPushButton::clicked,
                this, &ImageProcessingWidget::onMoveModuleDownClicked);
        controls->addWidget(m_addModuleButton);
        controls->addWidget(m_moveModuleUpButton);
        controls->addWidget(m_moveModuleDownButton);
        controls->addWidget(m_removeModuleButton);
        layout->addLayout(controls);
        return group;
    }

    QWidget* ImageProcessingWidget::setupModuleConfig()
    {
        auto* group = new QGroupBox(tr("Step Parameters"), this);
        auto* layout = new QVBoxLayout(group);
        m_configStack = new QStackedWidget(group);
        m_emptyConfigWidget = new QWidget(m_configStack);
        auto* emptyLayout = new QVBoxLayout(m_emptyConfigWidget);
        emptyLayout->addWidget(new QLabel(tr("Select a module to configure"), m_emptyConfigWidget));
        emptyLayout->addStretch();
        m_configStack->addWidget(m_emptyConfigWidget);
        layout->addWidget(m_configStack);
        return group;
    }

    void ImageProcessingWidget::updateModuleList()
    {
        const int currentRow = m_moduleList->currentRow();
        const auto modules = m_scopeonecore->processingModules();
        const QSignalBlocker blocker(m_moduleList);
        m_moduleList->clear();
        for (int index = 0; index < modules.size(); ++index)
        {
            const ProcessingModuleInfo& module = modules.at(index);
            auto* item = new QListWidgetItem(
                tr("%1. %2").arg(index + 1).arg(module.name()), m_moduleList);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(module.enabled() ? Qt::Checked : Qt::Unchecked);
            item->setToolTip(module.enabled()
                                 ? tr("Enabled")
                                 : tr("Bypassed"));
        }
        if (!modules.isEmpty())
        {
            m_moduleList->setCurrentRow(qBound(0, currentRow, modules.size() - 1));
        }
        else
        {
            m_moduleList->clearSelection();
        }
    }

    void ImageProcessingWidget::updateConfigWidget()
    {
        while (m_configStack->count() > 1)
        {
            QWidget* widget = m_configStack->widget(1);
            m_configStack->removeWidget(widget);
            widget->deleteLater();
        }
        m_configStack->setCurrentWidget(m_emptyConfigWidget);
        const int row = m_moduleList->currentRow();
        const auto modules = m_scopeonecore->processingModules();
        if (row < 0 || row >= modules.size())
        {
            return;
        }
        auto* editor = new ModuleConfigWidget(m_scopeonecore, row, modules.at(row), m_configStack);
        m_configStack->addWidget(editor);
        m_configStack->setCurrentWidget(editor);
    }

    void ImageProcessingWidget::updateRunButtons()
    {
        const bool running = m_scopeonecore->isRealTimeProcessingEnabled();
        const bool hasModules = !m_scopeonecore->processingModules().isEmpty();
        const bool idle = m_offlineProcessingRequestId == 0;
        m_processingBitDepthCombo->setEnabled(!running && idle);
        m_moduleList->setEnabled(!running && idle);
        m_moduleTypeCombo->setEnabled(!running && idle);
        m_addModuleButton->setEnabled(!running && idle);
        m_removeModuleButton->setEnabled(!running && idle && m_moduleList->currentRow() >= 0);
        m_moveModuleUpButton->setEnabled(!running && idle && m_moduleList->currentRow() > 0);
        m_moveModuleDownButton->setEnabled(!running && idle
                                           && m_moduleList->currentRow() >= 0
                                           && m_moduleList->currentRow() < m_moduleList->count() - 1);
        m_configStack->setEnabled(!running && idle);

        const bool liveMode = m_liveModeRadio->isChecked();
        const QString sourceType = m_sourceCombo->currentData(Qt::UserRole).toString();
        const bool hasPipeline = hasModules;
        const QString sourceId = m_sourceCombo->currentData(Qt::UserRole + 1).toString();
        const bool hasSource = !sourceType.isEmpty();
        const bool sourceAvailable = hasSource
                                     && (sourceType == QStringLiteral("layer")
                                         || sourceType == QStringLiteral("stack")
                                         || m_workspace->document(sourceId).ready);
        const bool canProcessImage = sourceType == QStringLiteral("layer")
                                      || sourceType == QStringLiteral("document")
                                      || sourceType == QStringLiteral("document_stack");
        const bool canProcessStack = sourceType == QStringLiteral("stack")
                                      || sourceType == QStringLiteral("document_stack");
        if (!canProcessStack && m_offlineScopeCombo->currentData().toBool())
        {
            const QSignalBlocker blocker(m_offlineScopeCombo);
            m_offlineScopeCombo->setCurrentIndex(0);
        }
        const bool entireStack = m_offlineScopeCombo->currentData().toBool();
        const bool validScope = entireStack ? canProcessStack : canProcessImage;
        const bool canRun = !running && idle && hasPipeline && sourceAvailable && validScope;
        m_liveSourceCombo->setEnabled(!running && idle && liveMode);
        m_sourceCombo->setEnabled(!running && idle && !liveMode);
        m_offlineScopeCombo->setEnabled(!running && idle && !liveMode && canProcessStack);
        m_executeButton->setText(liveMode
                                      ? running ? tr("Stop Live Pipeline")
                                                : tr("Start Live Pipeline")
                                      : tr("Run Processing"));
        m_executeButton->setEnabled(liveMode
                                         ? idle && (running || hasModules)
                                         : canRun);
        QString runDisabledReason;
        if (!idle)
        {
            runDisabledReason = tr("A processing run is already in progress");
        }
        else if (liveMode && !running && !hasModules)
        {
            runDisabledReason = tr("Add at least one pipeline step first");
        }
        else if (!liveMode && running)
        {
            runDisabledReason = tr("Stop live processing before running a static image");
        }
        else if (!liveMode && !hasPipeline)
        {
            runDisabledReason = tr("Add at least one pipeline step first");
        }
        else if (!liveMode && !hasSource)
        {
            runDisabledReason = tr("Select a source to process");
        }
        else if (!liveMode && !sourceAvailable)
        {
            runDisabledReason = tr("The selected source is not available");
        }
        else if (!liveMode && !validScope)
        {
            runDisabledReason = tr("The selected source does not support this range");
        }
        m_executeButton->setToolTip(runDisabledReason);
        m_cancelProcessingButton->setEnabled(!idle);
    }

    void ImageProcessingWidget::syncProcessingState()
    {
        const bool running = m_scopeonecore->isRealTimeProcessingEnabled();
        if (m_processingRunning == running)
        {
            return;
        }
        m_processingRunning = running;
        if (running)
        {
            emit processingStarted();
        }
        else
        {
            emit processingStopped();
        }
    }

    void ImageProcessingWidget::updateProcessingSettings()
    {
        const int value = static_cast<int>(m_scopeonecore->processingBitDepth());
        const QSignalBlocker blocker(m_processingBitDepthCombo);
        m_processingBitDepthCombo->setCurrentIndex(m_processingBitDepthCombo->findData(value));
        const QSignalBlocker sourceBlocker(m_liveSourceCombo);
        const int sourceIndex = m_liveSourceCombo->findData(
            m_scopeonecore->realTimeProcessingSource());
        m_liveSourceCombo->setCurrentIndex(qMax(0, sourceIndex));
    }

    void ImageProcessingWidget::refreshSources()
    {
        const QString currentType = m_sourceCombo->currentData(Qt::UserRole).toString();
        const QString currentFirst = m_sourceCombo->currentData(Qt::UserRole + 1).toString();
        const QString currentSecond = m_sourceCombo->currentData(Qt::UserRole + 2).toString();
        const QSignalBlocker blocker(m_sourceCombo);
        const QString liveSource = m_scopeonecore->realTimeProcessingSource();
        {
            const QSignalBlocker liveBlocker(m_liveSourceCombo);
            m_liveSourceCombo->clear();
            m_liveSourceCombo->addItem(tr("All preview cameras"), QString{});
            for (const QString& cameraId : m_scopeonecore->cameraIds())
            {
                m_liveSourceCombo->addItem(tr("Preview camera: %1").arg(cameraId), cameraId);
            }
            const int liveIndex = m_liveSourceCombo->findData(liveSource);
            m_liveSourceCombo->setCurrentIndex(qMax(0, liveIndex));
        }
        m_sourceCombo->clear();

        const QString activeDocumentId = m_workspace->activeDocumentId();
        for (const ImageDocumentInfo& document : m_workspace->documents())
        {
            const QString sourceType = document.frameCount > 1
                                           ? QStringLiteral("document_stack")
                                           : QStringLiteral("document");
            m_sourceCombo->addItem(
                document.active ? tr("Active document: %1").arg(document.title)
                                : tr("Document: %1").arg(document.title),
                sourceType);
            const int index = m_sourceCombo->count() - 1;
            m_sourceCombo->setItemData(index, document.id, Qt::UserRole + 1);
            if (document.id == activeDocumentId)
            {
                m_sourceCombo->setCurrentIndex(index);
            }
        }

        for (const QString& layerKey : m_scopeonecore->imageSceneModel()->layerIds())
        {
            scopeone::core::DocumentLayer layer;
            if (m_scopeonecore->imageSceneModel()->findLayer(layerKey, layer))
            {
                m_sourceCombo->addItem(layer.name.isEmpty() ? layerKey : layer.name,
                                       QStringLiteral("layer"));
                const int index = m_sourceCombo->count() - 1;
                m_sourceCombo->setItemData(index, layerKey, Qt::UserRole + 1);
            }
        }
        for (const QString& sessionId : m_scopeonecore->recordingSessionIds())
        {
            const auto session = m_scopeonecore->recordingSession(sessionId);
            if (!session)
            {
                continue;
            }
            for (const QString& cameraId : session->recordedCameraIds())
            {
                if (session->recordedFrameCount(cameraId) <= 0)
                {
                    continue;
                }
                m_sourceCombo->addItem(
                    tr("Stack: %1 / %2 (%3 frames)")
                        .arg(sessionId, cameraId)
                        .arg(session->recordedFrameCount(cameraId)),
                    QStringLiteral("stack"));
                const int index = m_sourceCombo->count() - 1;
                m_sourceCombo->setItemData(index, sessionId, Qt::UserRole + 1);
                m_sourceCombo->setItemData(index, cameraId, Qt::UserRole + 2);
            }
        }
        for (int index = 0; index < m_sourceCombo->count(); ++index)
        {
            if (m_sourceCombo->itemData(index, Qt::UserRole).toString() == currentType
                && m_sourceCombo->itemData(index, Qt::UserRole + 1).toString() == currentFirst
                && m_sourceCombo->itemData(index, Qt::UserRole + 2).toString() == currentSecond)
            {
                m_sourceCombo->setCurrentIndex(index);
                break;
            }
        }
        updateRunButtons();
    }

    void ImageProcessingWidget::onRunOfflineProcessing()
    {
        const QString sourceType = m_sourceCombo->currentData(Qt::UserRole).toString();
        const QString sourceId = m_sourceCombo->currentData(Qt::UserRole + 1).toString();
        const bool entireStack = m_offlineScopeCombo->currentData().toBool();
        m_directProcessingRequest = sourceType == QStringLiteral("layer")
                                    || sourceType == QStringLiteral("stack");
        m_offlineProcessingRequestId = entireStack
                                           ? (m_directProcessingRequest
                                                  ? m_scopeonecore->requestRecordingSessionStackProcessing(
                                                        m_sourceCombo->currentData(Qt::UserRole + 1).toString(),
                                                        m_sourceCombo->currentData(Qt::UserRole + 2).toString())
                                                  : m_workspace->processDocument(sourceId, true))
                                           : (m_directProcessingRequest
                                                  ? m_scopeonecore->requestImageProcessing(
                                                        m_scopeonecore->graphFrame(sourceId), sourceId)
                                                  : m_workspace->processDocument(sourceId, false));
        if (m_offlineProcessingRequestId == 0)
        {
            finishOfflineProcessing();
            QMessageBox::warning(this,
                                 tr("Processing Failed"),
                                 entireStack ? tr("No stack is available")
                                             : tr("No image is available"));
            return;
        }
        m_processingProgress->setRange(0, 0);
        m_processingProgress->setVisible(true);
        updateRunButtons();
    }

    void ImageProcessingWidget::onCancelProcessing()
    {
        m_scopeonecore->cancelProcessingRequest(m_offlineProcessingRequestId);
    }

    void ImageProcessingWidget::finishOfflineProcessing()
    {
        m_offlineProcessingRequestId = 0;
        m_directProcessingRequest = false;
        m_processingProgress->setVisible(false);
        updateRunButtons();
    }

    void ImageProcessingWidget::onAddModuleClicked()
    {
        if (!m_scopeonecore->addProcessingModule(m_moduleTypeCombo->currentData().toString()))
        {
            QMessageBox::warning(this, tr("Warning"), tr("Failed to add processing module"));
            return;
        }
        updateModuleList();
        m_moduleList->setCurrentRow(m_moduleList->count() - 1);
    }

    void ImageProcessingWidget::onRemoveModuleClicked()
    {
        const int row = m_moduleList->currentRow();
        if (row < 0 || !m_scopeonecore->removeProcessingModule(row))
        {
            QMessageBox::warning(this, tr("Warning"), tr("Failed to remove processing module"));
            return;
        }
    }

    void ImageProcessingWidget::onModuleSelectionChanged()
    {
        updateConfigWidget();
        updateRunButtons();
    }

    void ImageProcessingWidget::onModuleItemChanged(QListWidgetItem* item)
    {
        const int row = m_moduleList->row(item);
        m_scopeonecore->setProcessingModuleEnabled(row, item->checkState() == Qt::Checked);
    }

    void ImageProcessingWidget::onMoveModuleUpClicked()
    {
        const int row = m_moduleList->currentRow();
        if (row <= 0)
        {
            return;
        }
        if (m_scopeonecore->moveProcessingModule(row, row - 1))
        {
            m_moduleList->setCurrentRow(row - 1);
        }
    }

    void ImageProcessingWidget::onMoveModuleDownClicked()
    {
        const int row = m_moduleList->currentRow();
        if (row < 0 || row >= m_moduleList->count() - 1)
        {
            return;
        }
        if (m_scopeonecore->moveProcessingModule(row, row + 1))
        {
            m_moduleList->setCurrentRow(row + 1);
        }
    }

    void ImageProcessingWidget::onProcessingBitDepthChanged()
    {
        const auto depth = static_cast<scopeone::core::ProcessingBitDepth>(
            m_processingBitDepthCombo->currentData().toInt());
        if (!m_scopeonecore->setProcessingBitDepth(depth))
        {
            updateProcessingSettings();
        }
    }

    void ImageProcessingWidget::onLiveProcessingToggled(bool enabled)
    {
        if (!m_scopeonecore->setRealTimeProcessingEnabled(enabled))
        {
            updateRunButtons();
            return;
        }
        updateRunButtons();
        qInfo().noquote() << (m_scopeonecore->isRealTimeProcessingEnabled()
                                  ? "Processing started"
                                  : "Processing stopped");
    }
}
