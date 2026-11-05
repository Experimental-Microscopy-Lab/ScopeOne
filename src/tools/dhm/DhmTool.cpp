#include "scopeone/DhmReconstruction.h"

#include "tools/ToolContext.h"
#include "scopeone/ScopeOneCore.h"
#include "scopeone/ImageSceneModel.h"

#include <QFile>
#include <QFutureWatcher>
#include <QPromise>
#include <QtConcurrent>

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


#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <utility>

namespace
{
    using scopeone::core::ImageFrame;
    using scopeone::core::DhmOutputMode;
    using scopeone::core::DhmParameters;
    using scopeone::core::DhmRoiMode;

    // The planar 256-entry ImageJ table the preview also uses
    QByteArray jetLookupTable()
    {
        QFile file(QStringLiteral(":/luts/Jet.lut"));
        QByteArray table = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        table.resize(3 * 256);
        return table;
    }

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

            // The spectrum is shown in the Jet lookup table of the preview, the top 8 bits of each sample
            static const QByteArray lut = jetLookupTable();
            m_image = QImage(frame.width, frame.height, QImage::Format_RGB888);
            for (int y = 0; y < frame.height; ++y)
            {
                const auto* source = reinterpret_cast<const quint16*>(frame.bytes.constData() + y * frame.stride);
                uchar* target = m_image.scanLine(y);
                for (int x = 0; x < frame.width; ++x)
                {
                    const int index = source[x] >> 8;
                    target[3 * x] = static_cast<uchar>(lut[index]);
                    target[3 * x + 1] = static_cast<uchar>(lut[256 + index]);
                    target[3 * x + 2] = static_cast<uchar>(lut[512 + index]);
                }
            }
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

    // What a reconstruction on the thread pool hands back to the window
    struct ReconstructionOutcome
    {
        scopeone::core::DhmResult result;
        QString error;
        bool canceled{false};
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

            connect(&m_watcher, &QFutureWatcher<ReconstructionOutcome>::progressValueChanged, this,
                    [this](int percent)
                    {
                        if (m_showProgress)
                        {
                            m_progress->setValue(percent);
                            m_status->setText(QStringLiteral("Reconstructing %1%").arg(percent));
                        }
                    });
            connect(&m_watcher, &QFutureWatcher<ReconstructionOutcome>::finished, this,
                    [this]() { reconstructionFinished(); });
            connect(&m_context.core(), &scopeone::core::ScopeOneCore::previewRawFrameReady, this,
                    [this](const ImageFrame& frame) { acceptLiveFrame(frame); });

            refreshLayerComboBox();
            connect(m_context.core().imageSceneModel(),
                    &scopeone::core::ImageSceneModel::layersChanged,
                    this,
                    [this]()
                    {
                        const QString previousSelection = m_layerComboBox->currentData().toString();
                        refreshLayerComboBox();
                        if (previousSelection != m_layerComboBox->currentData().toString())
                        {
                            startReconstruction();
                        }
                    });
            connect(m_layerComboBox, &QComboBox::currentIndexChanged, this,
                    [this]()
                    {
                        const QString layerKey = m_layerComboBox->currentData().toString();
                        m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
                        startReconstruction();
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
                        m_pendingFrame = {};
                        if (m_cancel)
                        {
                            m_cancel->store(true);
                        }
                    });
            connect(m_liveCheckBox, &QCheckBox::toggled, this,
                    [this](bool enabled)
                    {
                        if (!enabled)
                        {
                            m_pendingFrame = {};
                            m_status->setText(m_running ? QStringLiteral("Finishing current frame")
                                                        : QStringLiteral("Ready"));
                            return;
                        }
                        const QString layerKey = m_layerComboBox->currentData().toString();
                        const ImageFrame frame = !layerKey.isEmpty()
                                                     ? m_context.core().graphFrame(layerKey)
                                                     : m_context.core().graphFrame(
                                                           m_context.currentLayerKey());
                        if (frame.isValid())
                        {
                            m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
                            queueFrame(frame);
                        }
                    });

            const ImageFrame frame = m_context.core().graphFrame(m_context.currentLayerKey());
            if (frame.isValid())
            {
                m_sourceId = frame.cameraId;
                queueFrame(frame);
            }
        }

        // A reconstruction still running is canceled and awaited, so no result arrives after the window is gone
        ~DhmTool() override
        {
            if (m_cancel)
            {
                m_cancel->store(true);
            }
            m_watcher.waitForFinished();
        }

    private:
        // Live frames of the shown source. While a frame is reconstructed only the newest one waits, so the
        // reconstruction keeps up with the camera by dropping the frames in between.
        void acceptLiveFrame(const ImageFrame& frame)
        {
            if (!m_liveCheckBox->isChecked() || !frame.isValid()
                || (!m_sourceId.isEmpty() && frame.cameraId != m_sourceId))
            {
                return;
            }
            m_sourceId = frame.cameraId;
            queueFrame(frame);
        }

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
                               frame.isMono16() ? QImage::Format_Grayscale16
                                                : QImage::Format_Grayscale8);
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
                                          : m_context.core().graphFrame(
                                                m_context.currentLayerKey());
            if (!input.isValid())
            {
                m_status->setText(QStringLiteral("No hologram layer available"));
                return;
            }
            m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey);
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
            // Starting a new task while one ran replaced the watched future, so the old task ran on unseen and
            // could no longer be canceled; a frame that arrives meanwhile waits until the current one finishes
            if (m_processing)
            {
                m_pendingFrame = input;
                return;
            }
            showPreview(m_inputPreview, input);
            m_processing = true;
            // While the reconstruction follows the stream the controls stay as they are, and the progress of
            // each frame is not shown, as both would flicker at the rate of the frames
            const bool live = m_liveCheckBox->isChecked();
            const DhmParameters params = parameters();
            m_cancel = std::make_shared<std::atomic_bool>(false);
            m_showProgress = !live;
            m_running = true;
            const auto cancel = m_cancel;
            m_watcher.setFuture(QtConcurrent::run(
                [input, params, cancel](QPromise<ReconstructionOutcome>& promise)
                {
                    promise.setProgressRange(0, 100);
                    ReconstructionOutcome outcome;
                    try
                    {
                        outcome.result = scopeone::core::reconstructDhm(
                            input, params, *cancel,
                            [&promise](int percent) { promise.setProgressValue(std::clamp(percent, 0, 100)); });
                    }
                    catch (const std::exception& exception)
                    {
                        outcome.error = QString::fromLocal8Bit(exception.what());
                    }
                    catch (...)
                    {
                        outcome.error = QStringLiteral("Reconstruction failed");
                    }
                    outcome.canceled = cancel->load();
                    promise.addResult(std::move(outcome));
                }));

            if (!live)
            {
                m_reconstructButton->setEnabled(false);
                m_autoDetectButton->setEnabled(false);
                m_layerComboBox->setEnabled(false);
                m_progress->setValue(0);
                m_status->setText(QStringLiteral("Reconstructing 0%"));
            }
            m_cancelButton->setEnabled(true);
        }

        void reconstructionFinished()
        {
            m_running = false;
            const QFuture<ReconstructionOutcome> future = m_watcher.future();
            const ReconstructionOutcome outcome = future.resultCount() > 0 ? future.result() : ReconstructionOutcome{};
            if (!outcome.error.isEmpty())
            {
                finishTask(outcome.error);
            }
            else if (outcome.canceled)
            {
                finishTask(QStringLiteral("Canceled"));
            }
            else
            {
                publishResult(outcome.result);
            }
        }

        void publishResult(const scopeone::core::DhmResult& result)
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
            const ImageFrame stored = m_context.core().publishToolStreamFrame(
                sourceId,
                result.outputFrame,
                displayName);
            if (!stored.isValid())
            {
                finishTask(QStringLiteral("Failed to publish DHM output"));
                return;
            }

            const QString outputLayer = scopeone::core::ScopeOneCore::toolLayerKey(stored.cameraId);
            if (m_shownInputSourceId != m_sourceId || m_shownOutputLayerKey != outputLayer)
            {
                QStringList layers;
                const QString rawLayer = scopeone::core::ScopeOneCore::rawLayerKey(m_sourceId);
                if (m_context.core().graphFrame(rawLayer).isValid())
                {
                    layers.append(rawLayer);
                }
                layers.append(outputLayer);
                m_context.showLayers(layers, layers.size() > 1);
                m_shownInputSourceId = m_sourceId;
                m_shownOutputLayerKey = outputLayer;
            }
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
            const bool live = m_liveCheckBox->isChecked();
            m_progress->setValue(status == QStringLiteral("DHM output ready") ? 100 : m_progress->value());
            m_status->setText(live && status == QStringLiteral("DHM output ready") ? QStringLiteral("Live") : status);
            m_reconstructButton->setEnabled(true);
            m_autoDetectButton->setEnabled(true);
            m_layerComboBox->setEnabled(true);
            m_cancelButton->setEnabled(false);
            m_processing = false;
            if (m_pendingFrame.isValid())
            {
                const ImageFrame next = m_pendingFrame;
                m_pendingFrame = {};
                queueFrame(next);
            }
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
        QFutureWatcher<ReconstructionOutcome> m_watcher;
        std::shared_ptr<std::atomic_bool> m_cancel;
        bool m_running{false};
        bool m_showProgress{false};
        bool m_processing{false};
        ImageFrame m_pendingFrame;
        QString m_sourceId;
        QString m_shownInputSourceId;
        QString m_shownOutputLayerKey;
    };
}

namespace scopeone::ui
{
    SCOPEONE_REGISTER_TOOL(
        {QStringLiteral("scopeone.dhm_reconstruction"), QString::fromUtf8(QT_TRANSLATE_NOOP("Tools", "DHM Reconstruction")),
         {}, ToolWindowMode::ModelessSingleton},
        [](ScopeOneToolContext& context, QWidget* parent)
        {
            return new DhmTool(context, parent);
        })
}
