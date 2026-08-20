#include "ImageProcessingWidget.h"

#include "scopeone/ScopeOneCore.h"
#include "scopeone/ImageSceneModel.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QDebug>

namespace scopeone::ui
{
    namespace
    {
        using ProcessingModuleDescriptor = scopeone::core::ProcessingModuleDescriptor;
        using ProcessingModuleInfo = scopeone::core::ScopeOneCore::ProcessingModuleInfo;
        using ProcessingParameterDescriptor = scopeone::core::ProcessingParameterDescriptor;
        using ProcessingParameterType = scopeone::core::ProcessingParameterType;

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
        };
    }

    ImageProcessingWidget::ImageProcessingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent), m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("ImageProcessingWidget requires ScopeOneCore");
        }
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
        connect(core, &scopeone::core::ScopeOneCore::layerProcessingFinished,
                this, [this](quint64 requestId,
                             const QString& sourceLayerKey,
                             const scopeone::core::ImageFrame& frame,
                             const QString& error)
                {
                    if (requestId != m_offlineProcessingRequestId)
                    {
                        return;
                    }
                    finishOfflineProcessing();
                    if (!error.isEmpty() || !frame.isValid())
                    {
                        QMessageBox::warning(this, tr("Processing Failed"), error);
                        return;
                    }
                    const QString sourceId = QStringLiteral("processed:%1")
                                                 .arg(scopeone::core::ScopeOneCore::sourceIdFromLayerKey(
                                                     sourceLayerKey));
                    const auto output = m_scopeonecore->publishStaticFrame(
                        sourceId, frame, tr("Processed Image"));
                    if (output.isValid())
                    {
                        emit processedLayerReady(scopeone::core::ScopeOneCore::staticLayerKey(sourceId));
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
                    if (requestId != m_offlineProcessingRequestId)
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

        updateProcessingSettings();
        refreshSources();
        updateModuleList();
        updateConfigWidget();
        updateRunButtons();
    }

    void ImageProcessingWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        auto* splitter = new QSplitter(Qt::Vertical, this);
        setupRunControls();
        setupSourceControls();
        setupModuleList();
        setupModuleConfig();
        auto* topWidget = new QWidget(this);
        auto* topLayout = new QVBoxLayout(topWidget);
        topLayout->addWidget(m_runControlsWidget);
        topLayout->addWidget(m_sourceControlsWidget);
        topLayout->addWidget(m_moduleList->parentWidget());
        splitter->addWidget(topWidget);
        splitter->addWidget(m_configStack->parentWidget());
        splitter->setStretchFactor(0, 2);
        splitter->setStretchFactor(1, 3);
        mainLayout->addWidget(splitter);
    }

    void ImageProcessingWidget::setupSourceControls()
    {
        m_sourceControlsWidget = new QGroupBox(tr("Processing Source"), this);
        auto* layout = new QVBoxLayout(m_sourceControlsWidget);
        m_sourceCombo = new QComboBox(m_sourceControlsWidget);
        layout->addWidget(m_sourceCombo);
        auto* buttons = new QHBoxLayout;
        m_processImageButton = new QPushButton(tr("Process Image"), m_sourceControlsWidget);
        m_processStackButton = new QPushButton(tr("Process Stack"), m_sourceControlsWidget);
        m_cancelProcessingButton = new QPushButton(tr("Cancel"), m_sourceControlsWidget);
        m_cancelProcessingButton->setEnabled(false);
        connect(m_processImageButton, &QPushButton::clicked,
                this, &ImageProcessingWidget::onProcessImage);
        connect(m_processStackButton, &QPushButton::clicked,
                this, &ImageProcessingWidget::onProcessStack);
        connect(m_cancelProcessingButton, &QPushButton::clicked,
                this, &ImageProcessingWidget::onCancelProcessing);
        connect(m_sourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { updateRunButtons(); });
        buttons->addWidget(m_processImageButton);
        buttons->addWidget(m_processStackButton);
        buttons->addWidget(m_cancelProcessingButton);
        layout->addLayout(buttons);
        m_processingProgress = new QProgressBar(m_sourceControlsWidget);
        m_processingProgress->setVisible(false);
        layout->addWidget(m_processingProgress);
    }

    void ImageProcessingWidget::setupRunControls()
    {
        m_runControlsWidget = new QWidget(this);
        auto* layout = new QHBoxLayout(m_runControlsWidget);
        m_startButton = new QPushButton(tr("Start Processing"), m_runControlsWidget);
        m_stopButton = new QPushButton(tr("Stop Processing"), m_runControlsWidget);
        m_processingBitDepthCombo = new QComboBox(m_runControlsWidget);
        m_liveSourceCombo = new QComboBox(m_runControlsWidget);
        m_processingBitDepthCombo->addItem(
            tr("8-bit"), static_cast<int>(scopeone::core::ProcessingBitDepth::Bit8));
        m_processingBitDepthCombo->addItem(
            tr("16-bit"), static_cast<int>(scopeone::core::ProcessingBitDepth::Bit16));
        connect(m_startButton, &QPushButton::clicked, this, &ImageProcessingWidget::onStartProcessing);
        connect(m_stopButton, &QPushButton::clicked, this, &ImageProcessingWidget::onStopProcessing);
        connect(m_processingBitDepthCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ImageProcessingWidget::onProcessingBitDepthChanged);
        connect(m_liveSourceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]()
                {
                    if (!m_scopeonecore->setRealTimeProcessingSource(
                            m_liveSourceCombo->currentData().toString()))
                    {
                        updateProcessingSettings();
                    }
                });
        layout->addWidget(m_startButton);
        layout->addWidget(m_stopButton);
        layout->addWidget(m_processingBitDepthCombo);
        layout->addWidget(m_liveSourceCombo);
        layout->addStretch();
    }

    void ImageProcessingWidget::setupModuleList()
    {
        auto* group = new QGroupBox(tr("Processing Modules"), this);
        auto* layout = new QVBoxLayout(group);
        m_moduleList = new QListWidget(group);
        connect(m_moduleList, &QListWidget::currentRowChanged,
                this, &ImageProcessingWidget::onModuleSelectionChanged);
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
        connect(m_addModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onAddModuleClicked);
        connect(m_removeModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onRemoveModuleClicked);
        controls->addWidget(m_addModuleButton);
        controls->addWidget(m_removeModuleButton);
        layout->addLayout(controls);
    }

    void ImageProcessingWidget::setupModuleConfig()
    {
        auto* group = new QGroupBox(tr("Module Configuration"), this);
        auto* layout = new QVBoxLayout(group);
        m_configStack = new QStackedWidget(group);
        m_emptyConfigWidget = new QWidget(m_configStack);
        auto* emptyLayout = new QVBoxLayout(m_emptyConfigWidget);
        emptyLayout->addWidget(new QLabel(tr("Select a module to configure"), m_emptyConfigWidget));
        emptyLayout->addStretch();
        m_configStack->addWidget(m_emptyConfigWidget);
        layout->addWidget(m_configStack);
    }

    void ImageProcessingWidget::updateModuleList()
    {
        const int currentRow = m_moduleList->currentRow();
        const auto modules = m_scopeonecore->processingModules();
        const QSignalBlocker blocker(m_moduleList);
        m_moduleList->clear();
        for (const ProcessingModuleInfo& module : modules)
        {
            m_moduleList->addItem(module.name());
        }
        if (!modules.isEmpty())
        {
            m_moduleList->setCurrentRow(qBound(0, currentRow, modules.size() - 1));
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
        m_startButton->setEnabled(!running && idle && hasModules);
        m_stopButton->setEnabled(running);
        m_processingBitDepthCombo->setEnabled(!running && idle);
        m_liveSourceCombo->setEnabled(!running && idle);
        m_moduleList->parentWidget()->setEnabled(!running && idle);
        m_configStack->parentWidget()->setEnabled(!running && idle);
        const QString sourceType = m_sourceCombo->currentData(Qt::UserRole).toString();
        const bool hasPipeline = !m_scopeonecore->processingModules().isEmpty();
        m_sourceCombo->setEnabled(!running && idle);
        m_processImageButton->setEnabled(!running && idle && hasPipeline
                                         && sourceType == QStringLiteral("layer"));
        m_processStackButton->setEnabled(!running && idle && hasPipeline
                                         && sourceType == QStringLiteral("stack"));
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
            m_liveSourceCombo->addItem(tr("Live: All Cameras"), QString{});
            for (const QString& cameraId : m_scopeonecore->cameraIds())
            {
                m_liveSourceCombo->addItem(tr("Live: %1").arg(cameraId), cameraId);
            }
            const int liveIndex = m_liveSourceCombo->findData(liveSource);
            m_liveSourceCombo->setCurrentIndex(qMax(0, liveIndex));
        }
        m_sourceCombo->clear();

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

    void ImageProcessingWidget::onProcessImage()
    {
        m_offlineProcessingRequestId = m_scopeonecore->requestLayerProcessing(
            m_sourceCombo->currentData(Qt::UserRole + 1).toString());
        if (m_offlineProcessingRequestId == 0)
        {
            QMessageBox::warning(this, tr("Processing Failed"), tr("No image is available"));
            return;
        }
        m_processingProgress->setRange(0, 0);
        m_processingProgress->setVisible(true);
        updateRunButtons();
    }

    void ImageProcessingWidget::onProcessStack()
    {
        m_offlineProcessingRequestId = m_scopeonecore->requestRecordingSessionStackProcessing(
            m_sourceCombo->currentData(Qt::UserRole + 1).toString(),
            m_sourceCombo->currentData(Qt::UserRole + 2).toString());
        if (m_offlineProcessingRequestId == 0)
        {
            QMessageBox::warning(this, tr("Processing Failed"), tr("No stack is available"));
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
        updateModuleList();
        updateConfigWidget();
    }

    void ImageProcessingWidget::onModuleSelectionChanged()
    {
        updateConfigWidget();
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

    void ImageProcessingWidget::onStartProcessing()
    {
        if (!m_scopeonecore->setRealTimeProcessingEnabled(true))
        {
            QMessageBox::information(this, tr("Information"), tr("Please add a processing module first"));
            return;
        }
        updateRunButtons();
        qInfo().noquote() << "Processing started";
    }

    void ImageProcessingWidget::onStopProcessing()
    {
        if (m_scopeonecore->setRealTimeProcessingEnabled(false))
        {
            updateRunButtons();
            qInfo().noquote() << "Processing stopped";
        }
    }
}
