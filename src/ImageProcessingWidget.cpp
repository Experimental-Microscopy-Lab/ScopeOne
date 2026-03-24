#include "ImageProcessingWidget.h"

#include "scopeone/ScopeOneCore.h"

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
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace scopeone::ui {

namespace {

using ProcessingModuleInfo = scopeone::core::ScopeOneCore::ProcessingModuleInfo;
using ProcessingModuleKind = scopeone::core::ScopeOneCore::ProcessingModuleKind;

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
        if (!m_scopeonecore) {
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
        auto* group = new QGroupBox("FFT Bandpass Settings", this);
        auto* groupLayout = new QGridLayout(group);

        groupLayout->addWidget(new QLabel("Min width:", group), 0, 0);
        m_minWidthSpin = new QDoubleSpinBox(group);
        m_minWidthSpin->setRange(0.0, 1000.0);
        m_minWidthSpin->setDecimals(2);
        groupLayout->addWidget(m_minWidthSpin, 0, 1);

        groupLayout->addWidget(new QLabel("Max width:", group), 1, 0);
        m_maxWidthSpin = new QDoubleSpinBox(group);
        m_maxWidthSpin->setRange(0.0, 1000.0);
        m_maxWidthSpin->setDecimals(2);
        groupLayout->addWidget(m_maxWidthSpin, 1, 1);

        groupLayout->addWidget(new QLabel("Filter kind:", group), 2, 0);
        m_filterKindCombo = new QComboBox(group);
        m_filterKindCombo->addItem("Smooth", 0);
        m_filterKindCombo->addItem("Hard", 1);
        groupLayout->addWidget(m_filterKindCombo, 2, 1);

        layout->addWidget(group);
        layout->addStretch();

        const QVariantMap params = info.parameters();
        m_minWidthSpin->setValue(params.value("min_width", 2.0).toDouble());
        m_maxWidthSpin->setValue(params.value("max_width", 10.0).toDouble());
        const int filterIndex = m_filterKindCombo->findData(params.value("filter_kind", 0).toInt());
        if (filterIndex >= 0) {
            m_filterKindCombo->setCurrentIndex(filterIndex);
        }

        connect(m_minWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this]() { apply(); });
        connect(m_maxWidthSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this]() { apply(); });
        connect(m_filterKindCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { apply(); });
    }

private:
    void apply()
    {
        QVariantMap params;
        params["min_width"] = m_minWidthSpin->value();
        params["max_width"] = m_maxWidthSpin->value();
        params["filter_kind"] = m_filterKindCombo->currentData().toInt();
        applyParameters(params);
    }

    QDoubleSpinBox* m_minWidthSpin{nullptr};
    QDoubleSpinBox* m_maxWidthSpin{nullptr};
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
        groupLayout->addWidget(m_spatialBinXSpin, 0, 1);

        groupLayout->addWidget(new QLabel("Spatial Y:", group), 1, 0);
        m_spatialBinYSpin = new QSpinBox(group);
        m_spatialBinYSpin->setRange(1, 64);
        groupLayout->addWidget(m_spatialBinYSpin, 1, 1);

        groupLayout->addWidget(new QLabel("Temporal:", group), 2, 0);
        m_temporalBinSpin = new QSpinBox(group);
        m_temporalBinSpin->setRange(1, 256);
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
        for (int i = 0; i < m_spatialModeCombo->count(); ++i) {
            m_temporalModeCombo->addItem(m_spatialModeCombo->itemText(i),
                                         m_spatialModeCombo->itemData(i));
        }
        groupLayout->addWidget(m_temporalModeCombo, 4, 1);

        layout->addWidget(group);
        layout->addStretch();

        const QVariantMap params = info.parameters();
        m_spatialBinXSpin->setValue(params.value("spatial_bin_x", 1).toInt());
        m_spatialBinYSpin->setValue(params.value("spatial_bin_y", 1).toInt());
        m_temporalBinSpin->setValue(params.value("temporal_bin", 1).toInt());

        const int spatialModeIndex = m_spatialModeCombo->findData(params.value("spatial_mode", 0).toInt());
        if (spatialModeIndex >= 0) {
            m_spatialModeCombo->setCurrentIndex(spatialModeIndex);
        }
        const int temporalModeIndex = m_temporalModeCombo->findData(params.value("temporal_mode", 0).toInt());
        if (temporalModeIndex >= 0) {
            m_temporalModeCombo->setCurrentIndex(temporalModeIndex);
        }

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
        groupLayout->addWidget(m_kernelSizeSpin, 0, 1);

        groupLayout->addWidget(new QLabel("Sigma:", group), 1, 0);
        m_sigmaSpin = new QDoubleSpinBox(group);
        m_sigmaSpin->setRange(0.0, 100.0);
        m_sigmaSpin->setDecimals(2);
        groupLayout->addWidget(m_sigmaSpin, 1, 1);

        layout->addWidget(group);
        layout->addStretch();

        const QVariantMap params = info.parameters();
        m_kernelSizeSpin->setValue(params.value("kernel_size", 3).toInt());
        m_sigmaSpin->setValue(params.value("sigma", 0.0).toDouble());

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

class MedianFilterModuleConfigWidget : public ProcessingModuleConfigWidgetBase
{
public:
    MedianFilterModuleConfigWidget(scopeone::core::ScopeOneCore* core,
                                   int moduleIndex,
                                   const ProcessingModuleInfo& info,
                                   QWidget* parent = nullptr)
        : ProcessingModuleConfigWidgetBase(core, moduleIndex, parent)
    {
        auto* layout = new QVBoxLayout(this);
        auto* group = new QGroupBox("Temporal Median Settings", this);
        auto* groupLayout = new QGridLayout(group);

        groupLayout->addWidget(new QLabel("Window Size:", group), 0, 0);
        m_windowSizeSpin = new QSpinBox(group);
        m_windowSizeSpin->setRange(3, 99);
        m_windowSizeSpin->setSingleStep(2);
        groupLayout->addWidget(m_windowSizeSpin, 0, 1);

        m_resetButton = new QPushButton("Reset Buffer", this);

        layout->addWidget(group);
        layout->addWidget(m_resetButton);
        layout->addStretch();

        m_windowSizeSpin->setValue(info.parameters().value("window_size", 5).toInt());

        connect(m_windowSizeSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this]() { apply(); });
        connect(m_resetButton, &QPushButton::clicked, this, [this]() {
            resetModule();
        });
    }

private:
    void apply()
    {
        QVariantMap params;
        params["window_size"] = m_windowSizeSpin->value();
        applyParameters(params);
    }

    QSpinBox* m_windowSizeSpin{nullptr};
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
        m_operationCombo->addItem("Subtract (Remove BG)", 0);
        m_operationCombo->addItem("Add (Add Pattern)", 1);
        m_operationCombo->addItem("Multiply (Modulate)", 2);
        m_operationCombo->addItem("Divide (Flat-field)", 3);
        groupLayout->addWidget(m_operationCombo, 3, 1);

        layout->addWidget(group);

        m_resetButton = new QPushButton("Reset Background", this);
        layout->addWidget(m_resetButton);
        layout->addStretch();

        const QVariantMap params = info.parameters();
        int frames = params.value("calibration_frames", 101).toInt();
        if (frames < 3) {
            frames = 3;
        }
        if ((frames % 2) == 0) {
            ++frames;
        }
        m_calibrationFramesSpin->setValue(frames);
        const int modeIndex = m_modeCombo->findData(params.value("mode", 0).toInt());
        if (modeIndex >= 0) {
            m_modeCombo->setCurrentIndex(modeIndex);
        }
        const int methodIndex = m_methodCombo->findData(params.value("method", 0).toInt());
        if (methodIndex >= 0) {
            m_methodCombo->setCurrentIndex(methodIndex);
        }
        const int operationIndex = m_operationCombo->findData(params.value("operation", 0).toInt());
        if (operationIndex >= 0) {
            m_operationCombo->setCurrentIndex(operationIndex);
        }

        connect(m_calibrationFramesSpin, QOverload<int>::of(&QSpinBox::valueChanged),
                this, [this]() { apply(); });
        connect(m_modeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { apply(); });
        connect(m_methodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { apply(); });
        connect(m_operationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [this]() { apply(); });
        connect(m_resetButton, &QPushButton::clicked, this, [this]() {
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
    switch (info.kind()) {
    case ProcessingModuleKind::FFT:
        return new FFTModuleConfigWidget(core, moduleIndex, info, parent);
    case ProcessingModuleKind::SpatiotemporalBinning:
        return new SpatiotemporalBinningModuleConfigWidget(core, moduleIndex, info, parent);
    case ProcessingModuleKind::GaussianBlur:
        return new GaussianBlurModuleConfigWidget(core, moduleIndex, info, parent);
    case ProcessingModuleKind::MedianFilter:
        return new MedianFilterModuleConfigWidget(core, moduleIndex, info, parent);
    case ProcessingModuleKind::BackgroundCalibration:
        return new BackgroundCalibrationModuleConfigWidget(core, moduleIndex, info, parent);
    case ProcessingModuleKind::Unknown:
        break;
    }
    return nullptr;
}

} // namespace

ImageProcessingWidget::ImageProcessingWidget(scopeone::core::ScopeOneCore* core, QWidget* parent)
    : QWidget(parent)
    , m_scopeonecore(core)
{
    if (!m_scopeonecore) {
        qFatal("ImageProcessingWidget requires ScopeOneCore");
    }

    connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processingModulesChanged,
            this, [this]() {
        updateModuleList();
        updateConfigWidget();
    });
    connect(m_scopeonecore, &scopeone::core::ScopeOneCore::processingError,
            this, &ImageProcessingWidget::onProcessingError);

    setupUI();
    updateModuleList();
    updateConfigWidget();
    updateRunButtons();
    qInfo().noquote() << "Image Processing Widget initialized";
}

void ImageProcessingWidget::setupUI()
{
    auto* mainLayout = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Vertical, this);

    setupRunControls();
    setupModuleList();
    setupModuleConfig();

    auto* topWidget = new QWidget(this);
    auto* topLayout = new QVBoxLayout(topWidget);
    if (m_runControlsWidget) {
        topLayout->addWidget(m_runControlsWidget);
    }
    topLayout->addWidget(m_moduleList->parentWidget());

    splitter->addWidget(topWidget);
    splitter->addWidget(m_configStack->parentWidget());
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    mainLayout->addWidget(splitter);
}

void ImageProcessingWidget::setupRunControls()
{
    m_runControlsWidget = new QWidget(this);
    auto* layout = new QHBoxLayout(m_runControlsWidget);
    m_startButton = new QPushButton("Start Processing", m_runControlsWidget);
    m_stopButton = new QPushButton("Stop Processing", m_runControlsWidget);
    connect(m_startButton, &QPushButton::clicked, this, &ImageProcessingWidget::onStartProcessing);
    connect(m_stopButton, &QPushButton::clicked, this, &ImageProcessingWidget::onStopProcessing);

    layout->addWidget(m_startButton);
    layout->addWidget(m_stopButton);
    layout->addStretch();
}

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
    m_moduleTypeCombo->addItem("Spatiotemporal Binning", static_cast<int>(ProcessingModuleKind::SpatiotemporalBinning));
    m_moduleTypeCombo->addItem("Gaussian Blur", static_cast<int>(ProcessingModuleKind::GaussianBlur));
    m_moduleTypeCombo->addItem("FFT Bandpass", static_cast<int>(ProcessingModuleKind::FFT));
    m_moduleTypeCombo->addItem("Temporal Median", static_cast<int>(ProcessingModuleKind::MedianFilter));
    m_moduleTypeCombo->addItem("Background Calibration", static_cast<int>(ProcessingModuleKind::BackgroundCalibration));
    controlsLayout->addWidget(m_moduleTypeCombo);

    m_addModuleButton = new QPushButton("Add", group);
    connect(m_addModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onAddModuleClicked);
    controlsLayout->addWidget(m_addModuleButton);

    m_removeModuleButton = new QPushButton("Remove", group);
    connect(m_removeModuleButton, &QPushButton::clicked, this, &ImageProcessingWidget::onRemoveModuleClicked);
    controlsLayout->addWidget(m_removeModuleButton);

    layout->addLayout(controlsLayout);
}

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

void ImageProcessingWidget::updateModuleList()
{
    if (!m_moduleList) {
        return;
    }

    const int currentRow = m_moduleList->currentRow();
    const QList<ProcessingModuleInfo> modules = m_scopeonecore->processingModules();

    m_moduleList->blockSignals(true);
    m_moduleList->clear();
    for (const ProcessingModuleInfo& info : modules) {
        m_moduleList->addItem(info.name());
    }
    if (!modules.isEmpty()) {
        const int nextRow = qBound(0, currentRow, m_moduleList->count() - 1);
        m_moduleList->setCurrentRow(nextRow);
    }
    m_moduleList->blockSignals(false);
}

void ImageProcessingWidget::updateConfigWidget()
{
    if (!m_configStack) {
        return;
    }

    while (m_configStack->count() > 1) {
        QWidget* widget = m_configStack->widget(1);
        m_configStack->removeWidget(widget);
        widget->deleteLater();
    }
    m_configStack->setCurrentWidget(m_emptyConfigWidget);

    const int currentRow = m_moduleList ? m_moduleList->currentRow() : -1;
    const QList<ProcessingModuleInfo> modules = m_scopeonecore->processingModules();
    if (currentRow < 0 || currentRow >= modules.size()) {
        return;
    }

    QWidget* configWidget = createConfigWidget(m_scopeonecore, currentRow, modules.at(currentRow), m_configStack);
    if (!configWidget) {
        return;
    }
    m_configStack->addWidget(configWidget);
    m_configStack->setCurrentWidget(configWidget);
}

void ImageProcessingWidget::updateRunButtons()
{
    const bool running = m_scopeonecore->isRealTimeProcessingEnabled();
    // Lock edits while processing runs
    if (m_startButton) {
        m_startButton->setEnabled(!running);
    }
    if (m_stopButton) {
        m_stopButton->setEnabled(running);
    }
    if (m_moduleList && m_moduleList->parentWidget()) {
        m_moduleList->parentWidget()->setEnabled(!running);
    }
    if (m_configStack && m_configStack->parentWidget()) {
        m_configStack->parentWidget()->setEnabled(!running);
    }
}

void ImageProcessingWidget::onProcessingError(const QString& error)
{
    qWarning().noquote() << QString("Processing error: %1").arg(error);
}

void ImageProcessingWidget::onAddModuleClicked()
{
    const auto kind = static_cast<ProcessingModuleKind>(m_moduleTypeCombo->currentData().toInt());
    if (!m_scopeonecore->addProcessingModule(kind)) {
        QMessageBox::warning(this, "Warning", "Failed to add processing module");
        return;
    }

    updateModuleList();
    if (m_moduleList && m_moduleList->count() > 0) {
        m_moduleList->setCurrentRow(m_moduleList->count() - 1);
    }
}

void ImageProcessingWidget::onRemoveModuleClicked()
{
    if (!m_moduleList) {
        return;
    }

    const int currentRow = m_moduleList->currentRow();
    if (currentRow < 0) {
        QMessageBox::information(this, "Information", "Please select a module to remove");
        return;
    }

    if (!m_scopeonecore->removeProcessingModule(currentRow)) {
        QMessageBox::warning(this, "Warning", "Failed to remove processing module");
        return;
    }

    updateModuleList();
    updateConfigWidget();
}

void ImageProcessingWidget::onModuleSelectionChanged()
{
    updateConfigWidget();
}

void ImageProcessingWidget::onStartProcessing()
{
    m_scopeonecore->setRealTimeProcessingEnabled(true);
    updateRunButtons();
    emit processingStarted();
    qInfo().noquote() << "Processing started";
}

void ImageProcessingWidget::onStopProcessing()
{
    m_scopeonecore->setRealTimeProcessingEnabled(false);
    updateRunButtons();
    qInfo().noquote() << "Processing stopped";
}

} // namespace scopeone::ui
