#include "ImageProcessingWidget.h"

#include "scopeone/ScopeOneCore.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QtGlobal>

namespace scopeone::ui
{
    namespace
    {
        using ProcessingModuleInfo = scopeone::core::ScopeOneCore::ProcessingModuleInfo;
        using ProcessingModuleKind = scopeone::core::ScopeOneCore::ProcessingModuleKind;

        void configureParameterSpinBox(QAbstractSpinBox* spinBox)
        {
            spinBox->setKeyboardTracking(false);
            spinBox->setCorrectionMode(QAbstractSpinBox::CorrectToNearestValue);
        }

        class ProcessingModuleConfigWidgetBase : public QWidget
        {
        public:
            ProcessingModuleConfigWidgetBase(scopeone::core::ScopeOneCore* core,
                                             int moduleIndex,
                                             QWidget* parent = nullptr)
                : QWidget(parent)
                  , m_scopeonecore(core)
                  , m_moduleIndex(moduleIndex)
            {
                if (!core)
                {
                    qFatal("ProcessingModuleConfigWidgetBase requires ScopeOneCore");
                }
            }

        protected:
            bool applyParameters(const QVariantMap& parameters)
            {
                return m_scopeonecore->setProcessingModuleParameters(m_moduleIndex, parameters);
            }

            bool resetModule()
            {
                return m_scopeonecore->resetProcessingModuleState(m_moduleIndex);
            }

            scopeone::core::ScopeOneCore* m_scopeonecore{nullptr};
            int m_moduleIndex{-1};
        };

        class FFTModuleConfigWidget : public ProcessingModuleConfigWidgetBase
        {
        public:
            FFTModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                                  int moduleIndex,
                                  const ProcessingModuleInfo& info,
                                  QWidget* parent = nullptr)
                : ProcessingModuleConfigWidgetBase(core, moduleIndex, parent)
            {
                auto* layout = new QVBoxLayout(this);
                auto* group = new QGroupBox("FFT Settings", this);
                auto* groupLayout = new QGridLayout(group);

                groupLayout->addWidget(new QLabel("Output:", group), 0, 0);
                m_outputModeCombo = new QComboBox(group);
                m_outputModeCombo->addItem("FFT Spectrum", 0);
                m_outputModeCombo->addItem("Bandpass FFT Spectrum", 1);
                m_outputModeCombo->addItem("Bandpass IFFT Image", 2);
                groupLayout->addWidget(m_outputModeCombo, 0, 1);

                groupLayout->addWidget(new QLabel("Min feature size:", group), 1, 0);
                m_minFeatureSizeSpin = new QDoubleSpinBox(group);
                m_minFeatureSizeSpin->setRange(0.0, 1000.0);
                m_minFeatureSizeSpin->setDecimals(2);
                configureParameterSpinBox(m_minFeatureSizeSpin);
                groupLayout->addWidget(m_minFeatureSizeSpin, 1, 1);

                groupLayout->addWidget(new QLabel("Max feature size:", group), 2, 0);
                m_maxFeatureSizeSpin = new QDoubleSpinBox(group);
                m_maxFeatureSizeSpin->setRange(0.0, 1000.0);
                m_maxFeatureSizeSpin->setDecimals(2);
                configureParameterSpinBox(m_maxFeatureSizeSpin);
                groupLayout->addWidget(m_maxFeatureSizeSpin, 2, 1);

                groupLayout->addWidget(new QLabel("Filter kind:", group), 3, 0);
                m_filterKindCombo = new QComboBox(group);
                m_filterKindCombo->addItem("Smooth", 0);
                m_filterKindCombo->addItem("Hard", 1);
                groupLayout->addWidget(m_filterKindCombo, 3, 1);

                layout->addWidget(group);
                layout->addStretch();

                const QVariantMap params = info.parameters();
                const int outputModeIndex = m_outputModeCombo->findData(params.value("output_mode").toInt());
                m_outputModeCombo->setCurrentIndex(outputModeIndex);
                m_minFeatureSizeSpin->setValue(params.value("min_feature_size").toDouble());
                m_maxFeatureSizeSpin->setValue(params.value("max_feature_size").toDouble());
                const int filterIndex = m_filterKindCombo->findData(params.value("filter_kind").toInt());
                m_filterKindCombo->setCurrentIndex(filterIndex);

                connect(m_outputModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
                connect(m_minFeatureSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_maxFeatureSizeSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_filterKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
                updateFilterControls();
            }

        private:
            void apply()
            {
                QVariantMap params;
                params["output_mode"] = m_outputModeCombo->currentData().toInt();
                params["min_feature_size"] = m_minFeatureSizeSpin->value();
                params["max_feature_size"] = m_maxFeatureSizeSpin->value();
                params["filter_kind"] = m_filterKindCombo->currentData().toInt();
                updateFilterControls();
                applyParameters(params);
            }

            void updateFilterControls()
            {
                const bool usesBandpass = m_outputModeCombo->currentData().toInt() != 0;
                m_minFeatureSizeSpin->setEnabled(usesBandpass);
                m_maxFeatureSizeSpin->setEnabled(usesBandpass);
                m_filterKindCombo->setEnabled(usesBandpass);
            }

            QComboBox* m_outputModeCombo{nullptr};
            QDoubleSpinBox* m_minFeatureSizeSpin{nullptr};
            QDoubleSpinBox* m_maxFeatureSizeSpin{nullptr};
            QComboBox* m_filterKindCombo{nullptr};
        };

        class SpatiotemporalBinningModuleConfigWidget : public ProcessingModuleConfigWidgetBase
        {
        public:
            SpatiotemporalBinningModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                                                    int moduleIndex,
                                                    const ProcessingModuleInfo& info,
                                                    QWidget* parent = nullptr)
                : ProcessingModuleConfigWidgetBase(core, moduleIndex, parent)
            {
                auto* layout = new QVBoxLayout(this);
                auto* group = new QGroupBox("Spatiotemporal Binning Settings", this);
                auto* groupLayout = new QGridLayout(group);

                groupLayout->addWidget(new QLabel("Spatial X:", group), 0, 0);
                m_spatialBinXSpin = new QSpinBox(group);
                m_spatialBinXSpin->setRange(1, 64);
                configureParameterSpinBox(m_spatialBinXSpin);
                groupLayout->addWidget(m_spatialBinXSpin, 0, 1);

                groupLayout->addWidget(new QLabel("Spatial Y:", group), 1, 0);
                m_spatialBinYSpin = new QSpinBox(group);
                m_spatialBinYSpin->setRange(1, 64);
                configureParameterSpinBox(m_spatialBinYSpin);
                groupLayout->addWidget(m_spatialBinYSpin, 1, 1);

                groupLayout->addWidget(new QLabel("Temporal:", group), 2, 0);
                m_temporalBinSpin = new QSpinBox(group);
                m_temporalBinSpin->setRange(1, 256);
                configureParameterSpinBox(m_temporalBinSpin);
                groupLayout->addWidget(m_temporalBinSpin, 2, 1);

                groupLayout->addWidget(new QLabel("Spatial mode:", group), 3, 0);
                m_spatialModeCombo = new QComboBox(group);
                m_spatialModeCombo->addItem("Mean", 0);
                m_spatialModeCombo->addItem("Sum", 1);
                m_spatialModeCombo->addItem("Minimum", 2);
                m_spatialModeCombo->addItem("Maximum", 3);
                m_spatialModeCombo->addItem("Skip", 4);
                groupLayout->addWidget(m_spatialModeCombo, 3, 1);

                groupLayout->addWidget(new QLabel("Temporal mode:", group), 4, 0);
                m_temporalModeCombo = new QComboBox(group);
                for (int i = 0; i < m_spatialModeCombo->count(); ++i)
                {
                    m_temporalModeCombo->addItem(m_spatialModeCombo->itemText(i),
                                                 m_spatialModeCombo->itemData(i));
                }
                groupLayout->addWidget(m_temporalModeCombo, 4, 1);

                layout->addWidget(group);
                layout->addStretch();

                const QVariantMap params = info.parameters();
                m_spatialBinXSpin->setValue(params.value("spatial_bin_x").toInt());
                m_spatialBinYSpin->setValue(params.value("spatial_bin_y").toInt());
                m_temporalBinSpin->setValue(params.value("temporal_bin").toInt());

                const int spatialModeIndex = m_spatialModeCombo->findData(params.value("spatial_mode").toInt());
                m_spatialModeCombo->setCurrentIndex(spatialModeIndex);
                const int temporalModeIndex = m_temporalModeCombo->findData(params.value("temporal_mode").toInt());
                m_temporalModeCombo->setCurrentIndex(temporalModeIndex);

                connect(m_spatialBinXSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_spatialBinYSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_temporalBinSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_spatialModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
                connect(m_temporalModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
            }

        private:
            void apply()
            {
                QVariantMap params;
                params["spatial_bin_x"] = m_spatialBinXSpin->value();
                params["spatial_bin_y"] = m_spatialBinYSpin->value();
                params["temporal_bin"] = m_temporalBinSpin->value();
                params["spatial_mode"] = m_spatialModeCombo->currentData().toInt();
                params["temporal_mode"] = m_temporalModeCombo->currentData().toInt();
                applyParameters(params);
            }

            QSpinBox* m_spatialBinXSpin{nullptr};
            QSpinBox* m_spatialBinYSpin{nullptr};
            QSpinBox* m_temporalBinSpin{nullptr};
            QComboBox* m_spatialModeCombo{nullptr};
            QComboBox* m_temporalModeCombo{nullptr};
        };

        class GaussianBlurModuleConfigWidget : public ProcessingModuleConfigWidgetBase
        {
        public:
            GaussianBlurModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                                           int moduleIndex,
                                           const ProcessingModuleInfo& info,
                                           QWidget* parent = nullptr)
                : ProcessingModuleConfigWidgetBase(core, moduleIndex, parent)
            {
                auto* layout = new QVBoxLayout(this);
                auto* group = new QGroupBox("Gaussian Blur Settings", this);
                auto* groupLayout = new QGridLayout(group);

                groupLayout->addWidget(new QLabel("Kernel size:", group), 0, 0);
                m_kernelSizeSpin = new QSpinBox(group);
                m_kernelSizeSpin->setRange(1, 99);
                m_kernelSizeSpin->setSingleStep(2);
                configureParameterSpinBox(m_kernelSizeSpin);
                groupLayout->addWidget(m_kernelSizeSpin, 0, 1);

                groupLayout->addWidget(new QLabel("Sigma:", group), 1, 0);
                m_sigmaSpin = new QDoubleSpinBox(group);
                m_sigmaSpin->setRange(0.0, 100.0);
                m_sigmaSpin->setDecimals(2);
                configureParameterSpinBox(m_sigmaSpin);
                groupLayout->addWidget(m_sigmaSpin, 1, 1);

                layout->addWidget(group);
                layout->addStretch();

                const QVariantMap params = info.parameters();
                m_kernelSizeSpin->setValue(params.value("kernel_size").toInt());
                m_sigmaSpin->setValue(params.value("sigma").toDouble());

                connect(m_kernelSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_sigmaSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        this, [this]() { apply(); });
            }

        private:
            void apply()
            {
                QVariantMap params;
                params["kernel_size"] = m_kernelSizeSpin->value();
                params["sigma"] = m_sigmaSpin->value();
                applyParameters(params);
            }

            QSpinBox* m_kernelSizeSpin{nullptr};
            QDoubleSpinBox* m_sigmaSpin{nullptr};
        };

        class DifferentialRollingModuleConfigWidget : public ProcessingModuleConfigWidgetBase
        {
        public:
            DifferentialRollingModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                                                  int moduleIndex,
                                                  const ProcessingModuleInfo& info,
                                                  QWidget* parent = nullptr)
                : ProcessingModuleConfigWidgetBase(core, moduleIndex, parent)
            {
                auto* layout = new QVBoxLayout(this);
                auto* group = new QGroupBox("Differential Rolling Settings", this);
                auto* groupLayout = new QGridLayout(group);
                groupLayout->addWidget(new QLabel("Batch size:", group), 0, 0);

                m_batchSizeSpin = new QSpinBox(group);
                m_batchSizeSpin->setRange(1, 256);
                configureParameterSpinBox(m_batchSizeSpin);
                const QVariantMap params = info.parameters();
                m_batchSizeSpin->setValue(params.value("batch_size").toInt());
                groupLayout->addWidget(m_batchSizeSpin, 0, 1);

                m_normalizeCheck = new QCheckBox("Normalize by batch_1", group);
                m_normalizeCheck->setChecked(params.value("normalize").toBool());
                groupLayout->addWidget(m_normalizeCheck, 1, 0, 1, 2);

                layout->addWidget(group);
                layout->addWidget(new QLabel("Preview is zero-centered grayscale around mid-gray.", this));

                m_resetButton = new QPushButton("Reset Buffer", this);
                connect(m_batchSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this]()
                {
                    apply();
                });
                connect(m_normalizeCheck, &QCheckBox::toggled, this, [this]()
                {
                    apply();
                });
                connect(m_resetButton, &QPushButton::clicked, this, [this]()
                {
                    resetModule();
                });
                layout->addWidget(m_resetButton);
                layout->addStretch();
            }

        private:
            void apply()
            {
                QVariantMap params;
                params["batch_size"] = m_batchSizeSpin->value();
                params["normalize"] = m_normalizeCheck->isChecked();
                applyParameters(params);
            }

            QSpinBox* m_batchSizeSpin{nullptr};
            QCheckBox* m_normalizeCheck{nullptr};
            QPushButton* m_resetButton{nullptr};
        };

        class BackgroundCalibrationModuleConfigWidget : public ProcessingModuleConfigWidgetBase
        {
        public:
            BackgroundCalibrationModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                                                    int moduleIndex,
                                                    const ProcessingModuleInfo& info,
                                                    QWidget* parent = nullptr)
                : ProcessingModuleConfigWidgetBase(core, moduleIndex, parent)
            {
                auto* layout = new QVBoxLayout(this);
                auto* group = new QGroupBox("Background Calibration Settings", this);
                auto* groupLayout = new QGridLayout(group);

                groupLayout->addWidget(new QLabel("Frames:", group), 0, 0);
                m_calibrationFramesSpin = new QSpinBox(group);
                m_calibrationFramesSpin->setRange(3, 1001);
                m_calibrationFramesSpin->setSingleStep(2);
                configureParameterSpinBox(m_calibrationFramesSpin);
                groupLayout->addWidget(m_calibrationFramesSpin, 0, 1);

                groupLayout->addWidget(new QLabel("Mode:", group), 1, 0);
                m_modeCombo = new QComboBox(group);
                m_modeCombo->addItem("Snapshot", 0);
                m_modeCombo->addItem("Running", 1);
                groupLayout->addWidget(m_modeCombo, 1, 1);

                groupLayout->addWidget(new QLabel("Method:", group), 2, 0);
                m_methodCombo = new QComboBox(group);
                m_methodCombo->addItem("Median", 0);
                m_methodCombo->addItem("Mean", 1);
                m_methodCombo->addItem("Maximum", 2);
                m_methodCombo->addItem("Minimum", 3);
                groupLayout->addWidget(m_methodCombo, 2, 1);

                groupLayout->addWidget(new QLabel("Operation:", group), 3, 0);
                m_operationCombo = new QComboBox(group);
                m_operationCombo->addItem("Subtract", 0);
                m_operationCombo->addItem("Add", 1);
                m_operationCombo->addItem("Multiply", 2);
                m_operationCombo->addItem("Divide", 3);
                groupLayout->addWidget(m_operationCombo, 3, 1);

                layout->addWidget(group);

                m_resetButton = new QPushButton("Reset Background", this);
                layout->addWidget(m_resetButton);
                layout->addStretch();

                const QVariantMap params = info.parameters();
                int frames = params.value("calibration_frames").toInt();
                if (frames < 3)
                {
                    frames = 3;
                }
                if ((frames % 2) == 0)
                {
                    ++frames;
                }
                m_calibrationFramesSpin->setValue(frames);
                const int modeIndex = m_modeCombo->findData(params.value("mode").toInt());
                m_modeCombo->setCurrentIndex(modeIndex);
                const int methodIndex = m_methodCombo->findData(params.value("method").toInt());
                m_methodCombo->setCurrentIndex(methodIndex);
                const int operationIndex = m_operationCombo->findData(params.value("operation").toInt());
                m_operationCombo->setCurrentIndex(operationIndex);

                connect(m_calibrationFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                        this, [this]() { apply(); });
                connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
                connect(m_methodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
                connect(m_operationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                        this, [this]() { apply(); });
                connect(m_resetButton, &QPushButton::clicked, this, [this]()
                {
                    resetModule();
                });
            }

        private:
            void apply()
            {
                QVariantMap params;
                params["calibration_frames"] = m_calibrationFramesSpin->value();
                params["mode"] = m_modeCombo->currentData().toInt();
                params["method"] = m_methodCombo->currentData().toInt();
                params["operation"] = m_operationCombo->currentData().toInt();
                applyParameters(params);
            }

            QSpinBox* m_calibrationFramesSpin{nullptr};
            QComboBox* m_modeCombo{nullptr};
            QComboBox* m_methodCombo{nullptr};
            QComboBox* m_operationCombo{nullptr};
            QPushButton* m_resetButton{nullptr};
        };

        QWidget* createConfigWidget(scopeone::core::ScopeOneCore* core,
                                    int moduleIndex,
                                    const ProcessingModuleInfo& info,
                                    QWidget* parent)
        {
            switch (info.kind())
            {
            case ProcessingModuleKind::FFT:
                return new FFTModuleConfigWidget(core, moduleIndex, info, parent);
            case ProcessingModuleKind::SpatiotemporalBinning:
                return new SpatiotemporalBinningModuleConfigWidget(core, moduleIndex, info, parent);
            case ProcessingModuleKind::GaussianBlur:
                return new GaussianBlurModuleConfigWidget(core, moduleIndex, info, parent);
            case ProcessingModuleKind::DifferentialRolling:
                return new DifferentialRollingModuleConfigWidget(core, moduleIndex, info, parent);
            case ProcessingModuleKind::BackgroundCalibration:
                return new BackgroundCalibrationModuleConfigWidget(core, moduleIndex, info, parent);
            case ProcessingModuleKind::Unknown:
                break;
            }
            qFatal("ImageProcessingWidget received an unsupported processing module kind");
            return nullptr;
        }
    } // namespace

    ImageProcessingWidget::ImageProcessingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
        : QWidget(parent)
          , m_scopeonecore(core)
    {
        if (!core)
        {
            qFatal("ImageProcessingWidget requires ScopeOneCore");
        }
        m_processingRunning = m_scopeonecore->isRealTimeProcessingEnabled();

        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processingModulesChanged,
                this, [this]()
                {
                    updateModuleList();
                    updateConfigWidget();
                    updateRunButtons();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processingModuleParametersChanged,
                this, [this](int moduleIndex)
                {
                    if (moduleIndex == m_moduleList->currentRow())
                    {
                        updateConfigWidget();
                    }
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processingSettingsChanged,
                this, [this]()
                {
                    updateProcessingSettings();
                    updateRunButtons();
                    syncProcessingState();
                });
        connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processingError,
                this, [](const QString& error)
                {
                    qWarning().noquote() << QString("Processing error: %1").arg(error);
                });

        setupUI();
        updateProcessingSettings();
        updateModuleList();
        updateConfigWidget();
        updateRunButtons();
    }

    // Builds the image processing widget layout
    void ImageProcessingWidget::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        auto* splitter = new QSplitter(Qt::Vertical, this);

        setupRunControls();
        setupModuleList();
        setupModuleConfig();

        auto* topWidget = new QWidget(this);
        auto* topLayout = new QVBoxLayout(topWidget);
        topLayout->addWidget(m_runControlsWidget);
        topLayout->addWidget(m_moduleList->parentWidget());

        splitter->addWidget(topWidget);
        splitter->addWidget(m_configStack->parentWidget());
        splitter->setStretchFactor(0, 2);
        splitter->setStretchFactor(1, 3);

        mainLayout->addWidget(splitter);
    }

    // Builds processing start stop controls
    void ImageProcessingWidget::setupRunControls()
    {
        m_runControlsWidget = new QWidget(this);
        auto* layout = new QHBoxLayout(m_runControlsWidget);
        m_startButton = new QPushButton("Start Processing", m_runControlsWidget);
        m_stopButton = new QPushButton("Stop Processing", m_runControlsWidget);
        m_processingBitDepthCombo = new QComboBox(m_runControlsWidget);
        m_processingBitDepthCombo->addItem(
            "8-bit", static_cast<int>(scopeone::core::ScopeOneCore::ProcessingBitDepth::Bit8));
        m_processingBitDepthCombo->addItem(
            "16-bit", static_cast<int>(scopeone::core::ScopeOneCore::ProcessingBitDepth::Bit16));
        connect(m_startButton, &QPushButton::clicked, this, &ImageProcessingWidget::onStartProcessing);
        connect(m_stopButton, &QPushButton::clicked, this, &ImageProcessingWidget::onStopProcessing);
        connect(m_processingBitDepthCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &ImageProcessingWidget::onProcessingBitDepthChanged);

        layout->addWidget(m_startButton);
        layout->addWidget(m_stopButton);
        layout->addWidget(m_processingBitDepthCombo);
        layout->addStretch();
    }

    // Builds the module list and add remove controls
    void ImageProcessingWidget::setupModuleList()
    {
        auto* group = new QGroupBox("Processing Modules", this);
        auto* layout = new QVBoxLayout(group);

        m_moduleList = new QListWidget(group);
        connect(m_moduleList, &QListWidget::currentRowChanged,
                this, &ImageProcessingWidget::onModuleSelectionChanged);
        layout->addWidget(m_moduleList);

        auto* controlsLayout = new QHBoxLayout();
        m_moduleTypeCombo = new QComboBox(group);
        m_moduleTypeCombo->addItem("Spatiotemporal Binning",
                                   static_cast<int>(ProcessingModuleKind::SpatiotemporalBinning));
        m_moduleTypeCombo->addItem("Gaussian Blur", static_cast<int>(ProcessingModuleKind::GaussianBlur));
        m_moduleTypeCombo->addItem("FFT", static_cast<int>(ProcessingModuleKind::FFT));
        m_moduleTypeCombo->addItem("Differential Rolling",
                                   static_cast<int>(ProcessingModuleKind::DifferentialRolling));
        m_moduleTypeCombo->addItem("Background Calibration",
                                   static_cast<int>(ProcessingModuleKind::BackgroundCalibration));
        controlsLayout->addWidget(m_moduleTypeCombo);

        m_addModuleButton = new QPushButton("Add", group);
        connect(m_addModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onAddModuleClicked);
        controlsLayout->addWidget(m_addModuleButton);

        m_removeModuleButton = new QPushButton("Remove", group);
        connect(m_removeModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onRemoveModuleClicked);
        controlsLayout->addWidget(m_removeModuleButton);

        layout->addLayout(controlsLayout);
    }

    // Builds the module configuration stack
    void ImageProcessingWidget::setupModuleConfig()
    {
        auto* group = new QGroupBox("Module Configuration", this);
        auto* layout = new QVBoxLayout(group);

        m_configStack = new QStackedWidget(group);
        m_emptyConfigWidget = new QWidget(m_configStack);
        auto* emptyLayout = new QVBoxLayout(m_emptyConfigWidget);
        emptyLayout->addWidget(new QLabel("Select a module to configure", m_emptyConfigWidget));
        emptyLayout->addStretch();

        m_configStack->addWidget(m_emptyConfigWidget);
        layout->addWidget(m_configStack);
    }

    // Rebuilds the visible module list from core state
    void ImageProcessingWidget::updateModuleList()
    {
        const int currentRow = m_moduleList->currentRow();
        const QList<ProcessingModuleInfo> modules = m_scopeonecore->processingModules();

        const QSignalBlocker moduleListBlocker(m_moduleList);
        m_moduleList->clear();
        for (const ProcessingModuleInfo& info : modules)
        {
            m_moduleList->addItem(info.name());
        }
        if (!modules.isEmpty())
        {
            const int nextRow = qBound(0, currentRow, m_moduleList->count() - 1);
            m_moduleList->setCurrentRow(nextRow);
        }
    }

    // Rebuilds the editor for the selected module
    void ImageProcessingWidget::updateConfigWidget()
    {
        while (m_configStack->count() > 1)
        {
            QWidget* widget = m_configStack->widget(1);
            m_configStack->removeWidget(widget);
            widget->deleteLater();
        }
        m_configStack->setCurrentWidget(m_emptyConfigWidget);

        const int currentRow = m_moduleList->currentRow();
        const QList<ProcessingModuleInfo> modules = m_scopeonecore->processingModules();
        if (currentRow < 0 || currentRow >= modules.size())
        {
            return;
        }

        QWidget* configWidget = createConfigWidget(m_scopeonecore, currentRow, modules.at(currentRow), m_configStack);
        m_configStack->addWidget(configWidget);
        m_configStack->setCurrentWidget(configWidget);
    }

    // Updates processing controls from running state
    void ImageProcessingWidget::updateRunButtons()
    {
        const bool running = m_scopeonecore->isRealTimeProcessingEnabled();
        const bool hasModules = !m_scopeonecore->processingModules().isEmpty();
        m_startButton->setEnabled(!running && hasModules);
        m_stopButton->setEnabled(running);
        m_processingBitDepthCombo->setEnabled(!running);
        m_moduleList->parentWidget()->setEnabled(!running);
        m_configStack->parentWidget()->setEnabled(!running);
    }

    // Emits user facing processing state changes from the shared core state
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

    // Syncs processing settings from core state
    void ImageProcessingWidget::updateProcessingSettings()
    {
        const auto currentBitDepth = static_cast<int>(m_scopeonecore->processingBitDepth());
        const int index = m_processingBitDepthCombo->findData(currentBitDepth);
        if (index != m_processingBitDepthCombo->currentIndex())
        {
            const QSignalBlocker bitDepthBlocker(m_processingBitDepthCombo);
            m_processingBitDepthCombo->setCurrentIndex(index);
        }
    }

    // Adds the selected processing module
    void ImageProcessingWidget::onAddModuleClicked()
    {
        const auto kind = static_cast<ProcessingModuleKind>(m_moduleTypeCombo->currentData().toInt());
        if (!m_scopeonecore->addProcessingModule(kind))
        {
            QMessageBox::warning(this, "Warning", "Failed to add processing module");
            return;
        }

        updateModuleList();
        m_moduleList->setCurrentRow(m_moduleList->count() - 1);
        updateRunButtons();
    }

    // Removes the selected processing module
    void ImageProcessingWidget::onRemoveModuleClicked()
    {
        const int currentRow = m_moduleList->currentRow();
        if (currentRow < 0)
        {
            QMessageBox::information(this, "Information", "Please select a module to remove");
            return;
        }

        if (!m_scopeonecore->removeProcessingModule(currentRow))
        {
            QMessageBox::warning(this, "Warning", "Failed to remove processing module");
            return;
        }

        updateModuleList();
        updateConfigWidget();
        updateRunButtons();
    }

    // Updates configuration when module selection changes
    void ImageProcessingWidget::onModuleSelectionChanged()
    {
        updateConfigWidget();
    }

    // Applies the selected processing bit depth
    void ImageProcessingWidget::onProcessingBitDepthChanged()
    {
        const auto bitDepth = static_cast<scopeone::core::ScopeOneCore::ProcessingBitDepth>(
            m_processingBitDepthCombo->currentData().toInt());
        if (!m_scopeonecore->setProcessingBitDepth(bitDepth))
        {
            QMessageBox::warning(this, "Warning", "Failed to update processing bit depth");
            updateProcessingSettings();
        }
    }

    // Starts real time image processing
    void ImageProcessingWidget::onStartProcessing()
    {
        if (!m_scopeonecore->setRealTimeProcessingEnabled(true))
        {
            QMessageBox::information(this, "Information", "Please add a processing module first");
            updateRunButtons();
            return;
        }
        updateRunButtons();
        qInfo().noquote() << "Processing started";
    }

    // Stops real time image processing
    void ImageProcessingWidget::onStopProcessing()
    {
        if (!m_scopeonecore->setRealTimeProcessingEnabled(false))
        {
            QMessageBox::warning(this, "Warning", "Failed to stop image processing");
            return;
        }
        updateRunButtons();
        qInfo().noquote() << "Processing stopped";
    }
} // namespace scopeone::ui
