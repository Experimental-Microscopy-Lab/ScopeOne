#include "DhmReconstruction.h"

#include "scopeone/ToolFrameStream.h"
#include "scopeone/ToolPlugin.h"
#include "scopeone/ToolTask.h"
#include "scopeone/ScopeOneCore.h"
#include "scopeone/ImageSceneModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

namespace
{
    using scopeone::core::ImageFrame;
    using scopeone::dhm::DhmOutputMode;
    using scopeone::dhm::DhmParameters;
    using scopeone::dhm::DhmRoiMode;

    class SpectrumView final : public QWidget
    {
    public:
        explicit SpectrumView(QWidget* parent = nullptr)
            : QWidget(parent)
        {
            setMinimumSize(320, 320);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            setMouseTracking(true);
        }

        void setFrame(const ImageFrame& frame)
        {
            m_frame = frame;
            if (!frame.isValid())
            {
                m_image = {};
                update();
                return;
            }

            const cv::Mat source(frame.height,
                                 frame.width,
                                 CV_16UC1,
                                 const_cast<uchar*>(reinterpret_cast<const uchar*>(
                                     frame.bytes.constData())),
                                 static_cast<size_t>(frame.stride));
            cv::Mat eightBit;
            source.convertTo(eightBit, CV_8U, 1.0 / 256.0);
            cv::Mat colored;
            cv::applyColorMap(eightBit, colored, cv::COLORMAP_JET);
            cv::cvtColor(colored, colored, cv::COLOR_BGR2RGB);
            m_image = QImage(colored.data,
                             colored.cols,
                             colored.rows,
                             static_cast<int>(colored.step),
                             QImage::Format_RGB888)
                          .copy();
            update();
        }

        void setSelection(const QPoint& offset, int radius)
        {
            m_selection = offset;
            m_radius = radius;
            update();
        }

        void setSelectionChanged(std::function<void(const QPoint&)> callback)
        {
            m_selectionChanged = std::move(callback);
        }

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), QColor(QStringLiteral("#171b20")));
            if (m_image.isNull())
            {
                painter.setPen(QColor(QStringLiteral("#8d98a4")));
                painter.drawText(rect(), Qt::AlignCenter, tr("Spectrum unavailable"));
                return;
            }

            const QRectF imageRect = fittedImageRect();
            painter.drawImage(imageRect, m_image);

            const QPoint center(m_frame.width / 2, m_frame.height / 2);
            const QPoint selected(center.x() + m_selection.x(),
                                  center.y() + m_selection.y());
            const QPointF selectedPoint = imagePoint(selected, imageRect);
            const QPointF centerPoint = imagePoint(center, imageRect);
            const double scaleX = imageRect.width() / m_frame.width;
            const double scaleY = imageRect.height() / m_frame.height;
            const double radius = m_radius * (scaleX + scaleY) / 2.0;

            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 1.5));
            painter.drawLine(selectedPoint.x() - 8,
                             selectedPoint.y(),
                             selectedPoint.x() + 8,
                             selectedPoint.y());
            painter.drawLine(selectedPoint.x(),
                             selectedPoint.y() - 8,
                             selectedPoint.x(),
                             selectedPoint.y() + 8);
            painter.setPen(QPen(QColor(QStringLiteral("#ffd166")), 2.0));
            painter.drawEllipse(selectedPoint, radius, radius);
            painter.setPen(QPen(QColor(QStringLiteral("#ffffff")), 1.0, Qt::DashLine));
            painter.drawEllipse(centerPoint, 4.0, 4.0);
        }

        void mousePressEvent(QMouseEvent* event) override
        {
            if (event->button() != Qt::LeftButton || m_image.isNull())
            {
                return;
            }

            const QRectF imageRect = fittedImageRect();
            if (!imageRect.contains(event->position()))
            {
                return;
            }
            const int x = std::clamp(
                static_cast<int>((event->position().x() - imageRect.left())
                                 * m_frame.width / imageRect.width()),
                0,
                m_frame.width - 1);
            const int y = std::clamp(
                static_cast<int>((event->position().y() - imageRect.top())
                                 * m_frame.height / imageRect.height()),
                0,
                m_frame.height - 1);
            const QPoint center(m_frame.width / 2, m_frame.height / 2);
            m_selection = QPoint(x - center.x(), y - center.y());
            update();
            m_selectionChanged(m_selection);
        }

    private:
        QRectF fittedImageRect() const
        {
            const QSizeF imageSize = m_image.size();
            const QSizeF available = size();
            const double scale = std::min(available.width() / imageSize.width(),
                                          available.height() / imageSize.height());
            const QSizeF scaled = imageSize * scale;
            return QRectF((available.width() - scaled.width()) / 2.0,
                          (available.height() - scaled.height()) / 2.0,
                          scaled.width(),
                          scaled.height());
        }

        QPointF imagePoint(const QPoint& point, const QRectF& imageRect) const
        {
            return {imageRect.left() + imageRect.width() * point.x() / m_frame.width,
                    imageRect.top() + imageRect.height() * point.y() / m_frame.height};
        }

        ImageFrame m_frame;
        QImage m_image;
        QPoint m_selection;
        int m_radius{24};
        std::function<void(const QPoint&)> m_selectionChanged;
    };

    class DhmTool final : public QWidget
    {
    public:
        DhmTool(scopeone::ui::ScopeOneToolContext& context, QWidget* parent)
            : QWidget(parent)
              , m_context(context)
        {
            setWindowFlag(Qt::Window, true);
            setWindowTitle(QStringLiteral("DHM Reconstruction"));
            resize(1180, 820);

            auto* layout = new QVBoxLayout(this);
            auto* previews = new QHBoxLayout();
            m_inputPreview = createPreview(QStringLiteral("Input hologram"));
            m_spectrumView = new SpectrumView(this);
            m_resultPreview = createPreview(QStringLiteral("Reconstructed result"));
            previews->addWidget(m_inputPreview, 1);
            previews->addWidget(m_spectrumView, 1);
            previews->addWidget(m_resultPreview, 1);
            layout->addLayout(previews, 1);

            auto* controls = new QHBoxLayout();
            controls->addLayout(createSidebandForm(), 1);
            controls->addLayout(createOpticsForm(), 1);
            controls->addLayout(createOutputForm(), 1);
            layout->addLayout(controls);

            auto* actions = new QHBoxLayout();
            auto* layerLabel = new QLabel(QStringLiteral("Input:"), this);
            m_layerComboBox = new QComboBox(this);
            m_layerComboBox->setMinimumWidth(140);
            m_autoDetectButton = new QPushButton(QStringLiteral("Auto detect +1"), this);
            m_reconstructButton = new QPushButton(QStringLiteral("Reconstruct once"), this);
            m_cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
            m_cancelButton->setEnabled(false);
            actions->addWidget(layerLabel);
            actions->addWidget(m_layerComboBox);
            actions->addWidget(m_autoDetectButton);
            actions->addWidget(m_reconstructButton);
            actions->addWidget(m_cancelButton);
            actions->addStretch();
            m_liveCheckBox = new QCheckBox(QStringLiteral("Live reconstruction"), this);
            m_liveCheckBox->setChecked(true);
            actions->addWidget(m_liveCheckBox);
            layout->addLayout(actions);

            auto* statusLayout = new QHBoxLayout();
            m_progress = new QProgressBar(this);
            m_progress->setRange(0, 100);
            m_progress->setValue(0);
            m_status = new QLabel(QStringLiteral("Ready"), this);
            statusLayout->addWidget(m_progress, 1);
            statusLayout->addWidget(m_status);
            layout->addLayout(statusLayout);

            m_stream = new scopeone::ui::ScopeOneToolFrameStream(m_context.core(), this);

            refreshLayerComboBox();
            connect(m_context.core().imageSceneModel(),
                    &scopeone::core::ImageSceneModel::layersChanged,
                    this,
                    [this]() { refreshLayerComboBox(); });
            connect(m_layerComboBox, &QComboBox::currentIndexChanged, this,
                    [this]()
                    {
                        const QString layerKey = m_layerComboBox->currentData().toString();
                        m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
                        m_stream->setSourceId(m_sourceId);
                        if (!m_liveCheckBox->isChecked())
                        {
                            startReconstruction();
                        }
                    });

            m_spectrumView->setSelectionChanged([this](const QPoint& offset)
            {
                setManualSideband(offset);
            });
            connect(m_autoDetectButton, &QPushButton::clicked, this,
                    [this]()
                    {
                        m_autoDetectCheckBox->setChecked(true);
                        startReconstruction();
                    });
            connect(m_reconstructButton, &QPushButton::clicked, this,
                    [this]() { startReconstruction(); });
            connect(m_cancelButton, &QPushButton::clicked, this,
                    [this]()
                    {
                        m_liveCheckBox->setChecked(false);
                        m_task->cancel();
                    });
            connect(m_liveCheckBox, &QCheckBox::toggled, this,
                    [this](bool enabled)
                    {
                        m_stream->setEnabled(enabled);
                        if (!enabled)
                        {
                            m_status->setText(m_task ? QStringLiteral("Finishing current frame")
                                                     : QStringLiteral("Ready"));
                            return;
                        }
                        const QString layerKey = m_layerComboBox->currentData().toString();
                        const ImageFrame frame = !layerKey.isEmpty()
                                                     ? m_context.core().graphFrame(layerKey)
                                                     : m_context.currentFrame();
                        if (frame.isValid())
                        {
                            m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
                            m_stream->setSourceId(m_sourceId);
                            queueFrame(frame);
                        }
                    });
            connect(m_stream, &scopeone::ui::ScopeOneToolFrameStream::frameReady, this,
                    [this](const ImageFrame& frame)
                    {
                        if (m_liveCheckBox->isChecked())
                        {
                            m_sourceId = frame.cameraId;
                            m_stream->setSourceId(m_sourceId);
                            queueFrame(frame);
                        }
                    });

            const ImageFrame frame = m_context.currentFrame();
            if (frame.isValid())
            {
                m_sourceId = frame.cameraId;
                m_stream->setSourceId(m_sourceId);
                queueFrame(frame);
            }
        }

    private:
        QFormLayout* createSidebandForm()
        {
            auto* form = new QFormLayout();
            m_autoDetectCheckBox = new QCheckBox(QStringLiteral("Auto detect sideband"), this);
            m_autoDetectCheckBox->setChecked(true);
            m_sidebandX = createSpinBox(-4096, 4096, 48);
            m_sidebandY = createSpinBox(-4096, 4096, -32);
            m_radius = createSpinBox(1, 4096, 24);
            m_softEdgeCheckBox = new QCheckBox(QStringLiteral("Soft circular edge"), this);
            m_softEdgeCheckBox->setChecked(true);
            m_softEdgeSigma = createDoubleSpinBox(0.1, 30.0, 2.0, 1);
            form->addRow(m_autoDetectCheckBox);
            form->addRow(QStringLiteral("Sideband X"), m_sidebandX);
            form->addRow(QStringLiteral("Sideband Y"), m_sidebandY);
            form->addRow(QStringLiteral("Filter radius"), m_radius);
            form->addRow(m_softEdgeCheckBox);
            form->addRow(QStringLiteral("Soft edge sigma"), m_softEdgeSigma);
            connect(m_sidebandX, &QSpinBox::valueChanged, this,
                    [this]() { m_autoDetectCheckBox->setChecked(false); });
            connect(m_sidebandY, &QSpinBox::valueChanged, this,
                    [this]() { m_autoDetectCheckBox->setChecked(false); });
            connect(m_radius, &QSpinBox::valueChanged, this,
                    [this](int radius) { m_spectrumView->setSelection(currentOffset(), radius); });
            return form;
        }

        QFormLayout* createOpticsForm()
        {
            auto* form = new QFormLayout();
            m_roiMode = new QComboBox(this);
            m_roiMode->addItem(QStringLiteral("Full frame"),
                               static_cast<int>(DhmRoiMode::FullFrame));
            m_roiMode->addItem(QStringLiteral("Center crop 512"),
                               static_cast<int>(DhmRoiMode::CenterCrop));
            m_roiMode->addItem(QStringLiteral("Center crop 1024"),
                               static_cast<int>(DhmRoiMode::CenterCrop));
            m_roiSize = createSpinBox(32, 4096, 512);
            m_wavelength = createDoubleSpinBox(100.0, 2000.0, 632.8, 1);
            m_pixelSize = createDoubleSpinBox(0.01, 100.0, 5.5, 3);
            m_z = createDoubleSpinBox(-100.0, 100.0, 0.0, 4);
            m_wavelength->setSuffix(QStringLiteral(" nm"));
            m_pixelSize->setSuffix(QStringLiteral(" um"));
            m_z->setSuffix(QStringLiteral(" mm"));
            form->addRow(QStringLiteral("ROI"), m_roiMode);
            form->addRow(QStringLiteral("Crop size"), m_roiSize);
            form->addRow(QStringLiteral("Wavelength"), m_wavelength);
            form->addRow(QStringLiteral("Pixel size"), m_pixelSize);
            form->addRow(QStringLiteral("Propagation z"), m_z);
            connect(m_roiMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
                    [this](int index)
                    {
                        m_roiSize->setEnabled(index != 0);
                        if (index == 1)
                        {
                            m_roiSize->setValue(512);
                        }
                        if (index == 2)
                        {
                            m_roiSize->setValue(1024);
                        }
                    });
            return form;
        }

        QFormLayout* createOutputForm()
        {
            auto* form = new QFormLayout();
            m_unwrapCheckBox = new QCheckBox(QStringLiteral("Quality-guided unwrap"), this);
            m_tiltCheckBox = new QCheckBox(QStringLiteral("Remove tilt plane"), this);
            m_outputMode = new QComboBox(this);
            m_outputMode->addItem(QStringLiteral("Quantitative phase"),
                                  static_cast<int>(DhmOutputMode::QuantitativePhase));
            m_outputMode->addItem(QStringLiteral("Wrapped phase"),
                                  static_cast<int>(DhmOutputMode::WrappedPhase));
            m_outputMode->addItem(QStringLiteral("Amplitude"),
                                  static_cast<int>(DhmOutputMode::Amplitude));
            m_outputMode->addItem(QStringLiteral("Spectrum"),
                                  static_cast<int>(DhmOutputMode::Spectrum));
            form->addRow(m_unwrapCheckBox);
            form->addRow(m_tiltCheckBox);
            form->addRow(QStringLiteral("Output"), m_outputMode);
            return form;
        }

        static QSpinBox* createSpinBox(int minimum, int maximum, int value)
        {
            auto* spinBox = new QSpinBox();
            spinBox->setRange(minimum, maximum);
            spinBox->setValue(value);
            return spinBox;
        }

        static QDoubleSpinBox* createDoubleSpinBox(double minimum,
                                                    double maximum,
                                                    double value,
                                                    int decimals)
        {
            auto* spinBox = new QDoubleSpinBox();
            spinBox->setRange(minimum, maximum);
            spinBox->setDecimals(decimals);
            spinBox->setValue(value);
            return spinBox;
        }

        static QLabel* createPreview(const QString& title)
        {
            auto* preview = new QLabel(title);
            preview->setAlignment(Qt::AlignCenter);
            preview->setMinimumSize(300, 300);
            preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            preview->setFrameShape(QFrame::Box);
            preview->setStyleSheet(QStringLiteral("background: #202020; color: #aaaaaa;"));
            return preview;
        }

        static void showPreview(QLabel* preview, const ImageFrame& frame)
        {
            const QImage image(reinterpret_cast<const uchar*>(frame.bytes.constData()),
                               frame.width,
                               frame.height,
                               frame.stride,
                               QImage::Format_Grayscale16);
            preview->setPixmap(QPixmap::fromImage(image).scaled(
                preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        }

        QPoint currentOffset() const
        {
            return {m_sidebandX->value(), m_sidebandY->value()};
        }

        void setManualSideband(const QPoint& offset)
        {
            QSignalBlocker blockX(m_sidebandX);
            QSignalBlocker blockY(m_sidebandY);
            m_sidebandX->setValue(offset.x());
            m_sidebandY->setValue(offset.y());
            m_autoDetectCheckBox->setChecked(false);
            m_spectrumView->setSelection(offset, m_radius->value());
        }

        void refreshLayerComboBox()
        {
            const QString currentSelection = m_layerComboBox->currentData().toString();
            const QString activeKey = m_context.currentLayerKey();
            const QSignalBlocker blocker(m_layerComboBox);
            m_layerComboBox->clear();
            const QStringList layerIds = m_context.core().imageSceneModel()->layerIds();
            for (const QString& layerKey : layerIds)
            {
                QString name = layerKey;
                scopeone::core::DocumentLayer layer;
                if (m_context.core().imageSceneModel()->findLayer(layerKey, layer) && !layer.name.isEmpty())
                {
                    name = layer.name;
                }
                m_layerComboBox->addItem(name, layerKey);
            }
            int index = m_layerComboBox->findData(currentSelection);
            if (index < 0 && !activeKey.isEmpty())
            {
                index = m_layerComboBox->findData(activeKey);
            }
            if (index >= 0)
            {
                m_layerComboBox->setCurrentIndex(index);
            }
        }

        void startReconstruction()
        {
            const QString layerKey = m_layerComboBox->currentData().toString();
            const ImageFrame input = !layerKey.isEmpty()
                                          ? m_context.core().graphFrame(layerKey)
                                          : m_context.currentFrame();
            if (!input.isValid())
            {
                m_status->setText(QStringLiteral("No hologram layer available"));
                return;
            }
            m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
            m_stream->setSourceId(m_sourceId);
            queueFrame(input);
        }

        DhmParameters parameters() const
        {
            DhmParameters params;
            params.autoDetectSideband = m_autoDetectCheckBox->isChecked();
            params.sidebandX = m_sidebandX->value();
            params.sidebandY = m_sidebandY->value();
            params.radius = m_radius->value();
            params.softEdge = m_softEdgeCheckBox->isChecked();
            params.softEdgeSigma = m_softEdgeSigma->value();
            params.wavelength = m_wavelength->value() * 1.0e-9;
            params.pixelSize = m_pixelSize->value() * 1.0e-6;
            params.z = m_z->value() * 1.0e-3;
            params.unwrapPhase = m_unwrapCheckBox->isChecked();
            params.removeTilt = m_tiltCheckBox->isChecked();
            params.roiMode = static_cast<DhmRoiMode>(m_roiMode->currentData().toInt());
            params.roiSize = m_roiSize->value();
            params.outputMode = static_cast<DhmOutputMode>(m_outputMode->currentData().toInt());
            return params;
        }

        void queueFrame(const ImageFrame& input)
        {
            showPreview(m_inputPreview, input);
            m_stream->setProcessing(true);
            const DhmParameters params = parameters();
            auto result = std::make_shared<scopeone::dhm::DhmResult>();
            m_task = new scopeone::ui::ScopeOneToolTask(
                [input, params, result](const std::atomic_bool& cancel,
                                        const std::function<void(int)>& progress)
                {
                    *result = scopeone::dhm::reconstruct(input, params, cancel, progress);
                },
                this);
            connect(m_task, &scopeone::ui::ScopeOneToolTask::progressChanged, this,
                    [this](int percent)
                    {
                        m_progress->setValue(percent);
                        m_status->setText(QStringLiteral("Reconstructing %1%%").arg(percent));
                    });
            connect(m_task, &scopeone::ui::ScopeOneToolTask::finished, this,
                    [this, result]() { publishResult(*result); });
            connect(m_task, &scopeone::ui::ScopeOneToolTask::canceled, this,
                    [this]() { finishTask(QStringLiteral("Canceled")); });
            connect(m_task, &scopeone::ui::ScopeOneToolTask::failed, this,
                    [this](const QString& message) { finishTask(message); });

            m_reconstructButton->setEnabled(false);
            m_autoDetectButton->setEnabled(false);
            m_layerComboBox->setEnabled(false);
            m_cancelButton->setEnabled(true);
            m_progress->setValue(0);
            m_status->setText(QStringLiteral("Reconstructing 0%"));
            m_task->start();
        }

        void publishResult(const scopeone::dhm::DhmResult& result)
        {
            if (!result.outputFrame.isValid())
            {
                finishTask(QStringLiteral("Reconstruction produced no output"));
                return;
            }

            m_spectrumView->setFrame(result.spectrumFrame);
            if (m_autoDetectCheckBox->isChecked())
            {
                QSignalBlocker blockX(m_sidebandX);
                QSignalBlocker blockY(m_sidebandY);
                m_sidebandX->setValue(result.detectedSidebandX);
                m_sidebandY->setValue(result.detectedSidebandY);
            }
            m_spectrumView->setSelection(currentOffset(), m_radius->value());
            showPreview(m_resultPreview, result.outputFrame);

            const QString sourceId = result.outputFrame.cameraId;
            const QString displayName = outputName();
            const ImageFrame stored = m_context.publishToolStreamFrame(
                sourceId,
                result.outputFrame,
                displayName);
            if (!stored.isValid())
            {
                finishTask(QStringLiteral("Failed to publish DHM output"));
                return;
            }

            QStringList layers;
            const QString rawLayer = scopeone::core::ScopeOneCore::rawLayerKey(m_sourceId);
            if (m_context.core().graphFrame(rawLayer).isValid())
            {
                layers.append(rawLayer);
            }
            layers.append(scopeone::core::ScopeOneCore::toolLayerKey(stored.cameraId));
            m_context.showLayers(layers, layers.size() > 1);
            finishTask(QStringLiteral("DHM output ready"));
        }

        QString outputName() const
        {
            switch (static_cast<DhmOutputMode>(m_outputMode->currentData().toInt()))
            {
            case DhmOutputMode::QuantitativePhase:
                return QStringLiteral("DHM Quantitative Phase");
            case DhmOutputMode::WrappedPhase:
                return QStringLiteral("DHM Wrapped Phase");
            case DhmOutputMode::Amplitude:
                return QStringLiteral("DHM Amplitude");
            case DhmOutputMode::Spectrum:
                return QStringLiteral("DHM Spectrum");
            }
            return QStringLiteral("DHM Output");
        }

        void finishTask(const QString& status)
        {
            m_progress->setValue(status == QStringLiteral("DHM output ready") ? 100 : m_progress->value());
            m_status->setText(status);
            m_reconstructButton->setEnabled(true);
            m_autoDetectButton->setEnabled(true);
            m_layerComboBox->setEnabled(true);
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
        QLabel* m_inputPreview{nullptr};
        SpectrumView* m_spectrumView{nullptr};
        QLabel* m_resultPreview{nullptr};
        QCheckBox* m_autoDetectCheckBox{nullptr};
        QSpinBox* m_sidebandX{nullptr};
        QSpinBox* m_sidebandY{nullptr};
        QSpinBox* m_radius{nullptr};
        QCheckBox* m_softEdgeCheckBox{nullptr};
        QDoubleSpinBox* m_softEdgeSigma{nullptr};
        QComboBox* m_roiMode{nullptr};
        QSpinBox* m_roiSize{nullptr};
        QDoubleSpinBox* m_wavelength{nullptr};
        QDoubleSpinBox* m_pixelSize{nullptr};
        QDoubleSpinBox* m_z{nullptr};
        QCheckBox* m_unwrapCheckBox{nullptr};
        QCheckBox* m_tiltCheckBox{nullptr};
        QComboBox* m_outputMode{nullptr};
        QComboBox* m_layerComboBox{nullptr};
        QPushButton* m_autoDetectButton{nullptr};
        QPushButton* m_reconstructButton{nullptr};
        QPushButton* m_cancelButton{nullptr};
        QCheckBox* m_liveCheckBox{nullptr};
        QProgressBar* m_progress{nullptr};
        QLabel* m_status{nullptr};
        scopeone::ui::ScopeOneToolTask* m_task{nullptr};
        scopeone::ui::ScopeOneToolFrameStream* m_stream{nullptr};
        QString m_sourceId;
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
