#include "ImageToolsDialog.h"

#include "PreviewWidget.h"
#include "scopeone/ScopeOneCore.h"

#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtGlobal>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace scopeone::ui
{
    namespace
    {
        using scopeone::core::ImageFrame;
        using scopeone::core::ImagePixelFormat;
        using scopeone::core::RecordingFormat;
        using scopeone::core::ScopeOneCore;

        // Exposes an ImageFrame as a single channel OpenCV view
        cv::Mat frameMatView(const ImageFrame& frame)
        {
            if (!frame.isValid() || (!frame.isMono8() && !frame.isMono16()))
            {
                return {};
            }

            const int bytesPerPixel = frame.bytesPerPixel();
            const qint64 rowBytes = static_cast<qint64>(frame.width) * bytesPerPixel;
            if (rowBytes <= 0 || frame.stride < rowBytes)
            {
                return {};
            }

            const int type = frame.isMono16() ? CV_16UC1 : CV_8UC1;
            return cv::Mat(frame.height,
                           frame.width,
                           type,
                           const_cast<char*>(frame.bytes.constData()),
                           static_cast<size_t>(frame.stride));
        }

        // Copies a single channel OpenCV image into a frame derived from a reference frame
        ImageFrame frameFromMat(const cv::Mat& mat, const ImageFrame& reference)
        {
            if (mat.empty()
                || mat.channels() != 1
                || (mat.depth() != CV_8U && mat.depth() != CV_16U))
            {
                return {};
            }

            const qint64 rowBytes = static_cast<qint64>(mat.cols) * static_cast<qint64>(mat.elemSize());
            const qint64 byteCount = rowBytes * static_cast<qint64>(mat.rows);
            if (rowBytes <= 0
                || rowBytes > (std::numeric_limits<int>::max)()
                || byteCount <= 0
                || byteCount > (std::numeric_limits<qsizetype>::max)())
            {
                return {};
            }

            QByteArray bytes;
            bytes.resize(static_cast<qsizetype>(byteCount));
            char* dst = bytes.data();
            for (int y = 0; y < mat.rows; ++y)
            {
                std::memcpy(dst + static_cast<qint64>(y) * rowBytes,
                            mat.ptr(y),
                            static_cast<size_t>(rowBytes));
            }

            ImageFrame frame;
            frame.cameraId = reference.cameraId.trimmed();
            frame.width = mat.cols;
            frame.height = mat.rows;
            frame.stride = static_cast<int>(rowBytes);
            frame.pixelFormat = mat.depth() == CV_16U ? ImagePixelFormat::Mono16 : ImagePixelFormat::Mono8;
            const int referenceBits = reference.pixelFormat == frame.pixelFormat
                                          ? reference.bitsPerSample
                                          : (mat.depth() == CV_16U ? 16 : 8);
            frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(frame.pixelFormat, referenceBits);
            frame.frameIndex = reference.frameIndex;
            frame.timestampNs = reference.timestampNs;
            if (reference.hasSourceRoi() && frame.width == reference.width && frame.height == reference.height)
            {
                frame.sourceRoiX = reference.sourceRoiX;
                frame.sourceRoiY = reference.sourceRoiY;
                frame.sourceRoiWidth = reference.sourceRoiWidth;
                frame.sourceRoiHeight = reference.sourceRoiHeight;
            }
            else
            {
                frame.sourceRoiX = 0;
                frame.sourceRoiY = 0;
                frame.sourceRoiWidth = frame.width;
                frame.sourceRoiHeight = frame.height;
            }
            frame.bytes = std::move(bytes);
            return frame.isValid() ? frame : ImageFrame{};
        }

        // Returns the save directory shared with the recording panel
        QString defaultGallerySaveDirectory()
        {
            QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
            const QString saveDir = settings.value(QStringLiteral("LastSaveDirectory"),
                                                   QDir::homePath()).toString();
            return QDir(saveDir).exists() ? saveDir : QDir::homePath();
        }

        // Builds a gallery session from a validated mosaic frame
        std::shared_ptr<ScopeOneCore::RecordingSessionData> mosaicSessionFromFrame(
            ScopeOneCore& core,
            const ImageFrame& imageFrame)
        {
            scopeone::core::ExperimentPlan plan;
            plan.cameraIds = {imageFrame.cameraId};
            plan.streamToDisk = false;
            plan.format = RecordingFormat::Tiff;
            plan.saveDir = defaultGallerySaveDirectory();
            plan.baseName = QStringLiteral("stage_mosaic_")
                + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
            plan.metadataFileName = plan.baseName + QStringLiteral("_metadata.json");
            QList<ImageFrame> frames;
            frames.append(imageFrame);
            return core.createFrameSession(frames, plan);
        }

        // Adds one tile into an accumulation mosaic
        bool accumulateTileAt(cv::Mat& sum, cv::Mat& count, const ImageFrame& frame, int x, int y)
        {
            const cv::Mat tile = frameMatView(frame);
            if (sum.empty()
                || count.empty()
                || tile.empty()
                || tile.cols <= 0
                || tile.rows <= 0)
            {
                return false;
            }

            if (x < 0 || y < 0 || x + tile.cols > sum.cols || y + tile.rows > sum.rows)
            {
                return false;
            }

            const cv::Rect roi(x, y, tile.cols, tile.rows);
            cv::Mat tileFloat;
            tile.convertTo(tileFloat, CV_32F);
            sum(roi) += tileFloat;
            count(roi) += cv::Scalar(1.0);
            return true;
        }

        // Converts an accumulated mosaic into the original preview bit depth
        cv::Mat accumulatedPreviewMat(const cv::Mat& sum, const cv::Mat& count, int outputType)
        {
            if (sum.empty() || count.empty())
            {
                return {};
            }

            cv::Mat safeCount;
            cv::max(count, cv::Scalar(1.0), safeCount);

            cv::Mat average;
            cv::divide(sum, safeCount, average);

            cv::Mat output;
            average.convertTo(output, outputType);
            output.setTo(cv::Scalar(0), count == 0);
            return output;
        }

        // Limit mosaic allocation so invalid stage settings cannot exhaust memory
        constexpr qsizetype kMaxMosaicPixels = 10000ll * 10000ll;
    }

    class StageMosaicDialog::MosaicStorage
    {
    public:
        cv::Mat sum;
        cv::Mat count;
        int tileWidth{0};
        int tileHeight{0};
        int outputType{CV_8UC1};
        double minOffsetXUm{0.0};
        double minOffsetYUm{0.0};
        double pixelSizeUm{1.0};
    };

    // Create a stage driven mosaic tool
    StageMosaicDialog::StageMosaicDialog(scopeone::core::ScopeOneCore* core,
                                         PreviewWidget* previewWidget,
                                         QWidget* parent)
        : QDialog(parent)
          , m_core(core)
          , m_previewWidget(previewWidget)
          , m_mosaic(std::make_unique<MosaicStorage>())
    {
        if (!core || !previewWidget)
        {
            qFatal("StageMosaicDialog requires ScopeOneCore and PreviewWidget");
        }

        setWindowTitle(tr("Stage Mosaic"));
        setupUI();
        refreshDevices();
        connect(m_core,
                &scopeone::core::ScopeOneCore::newRawFrameReady,
                this,
                [this](const ImageFrame& frame)
                {
                    onRawFrameReady(frame);
                });
    }

    StageMosaicDialog::~StageMosaicDialog() = default;

    // Stop active capture before closing the dialog
    void StageMosaicDialog::reject()
    {
        if (m_running)
        {
            m_running = false;
            m_waitingForTileFrame = false;
            if (m_returnToStartCheckBox->isChecked() && m_originValid && !m_activeStageId.isEmpty())
            {
                m_core->moveXYTo(m_activeStageId, m_originX, m_originY);
            }
            m_originValid = false;
        }
        QDialog::reject();
    }

    // Build the mosaic control form
    void StageMosaicDialog::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);

        auto* captureGroup = new QGroupBox(tr("Capture"), this);
        auto* captureLayout = new QFormLayout(captureGroup);

        m_cameraCombo = new QComboBox(captureGroup);
        m_stageCombo = new QComboBox(captureGroup);
        m_rowsSpinBox = new QSpinBox(captureGroup);
        m_rowsSpinBox->setRange(1, 100);
        m_rowsSpinBox->setValue(3);
        m_columnsSpinBox = new QSpinBox(captureGroup);
        m_columnsSpinBox->setRange(1, 100);
        m_columnsSpinBox->setValue(3);

        m_pixelSizeSpinBox = new QDoubleSpinBox(captureGroup);
        m_pixelSizeSpinBox->setRange(0.001, 1000000.0);
        m_pixelSizeSpinBox->setDecimals(4);
        m_pixelSizeSpinBox->setValue(1.0);
        m_pixelSizeSpinBox->setSuffix(QStringLiteral(" um/px"));
        m_pixelSizeSpinBox->setKeyboardTracking(false);

        m_stepXSpinBox = new QDoubleSpinBox(captureGroup);
        m_stepXSpinBox->setRange(-1000000.0, 1000000.0);
        m_stepXSpinBox->setDecimals(3);
        m_stepXSpinBox->setValue(100.0);
        m_stepXSpinBox->setSuffix(QStringLiteral(" um"));
        m_stepXSpinBox->setKeyboardTracking(false);

        m_stepYSpinBox = new QDoubleSpinBox(captureGroup);
        m_stepYSpinBox->setRange(-1000000.0, 1000000.0);
        m_stepYSpinBox->setDecimals(3);
        m_stepYSpinBox->setValue(100.0);
        m_stepYSpinBox->setSuffix(QStringLiteral(" um"));
        m_stepYSpinBox->setKeyboardTracking(false);

        m_settleMsSpinBox = new QSpinBox(captureGroup);
        m_settleMsSpinBox->setRange(0, 10000);
        m_settleMsSpinBox->setValue(150);
        m_settleMsSpinBox->setSuffix(QStringLiteral(" ms"));
        m_settleMsSpinBox->setKeyboardTracking(false);

        m_returnToStartCheckBox = new QCheckBox(tr("Return to start"), captureGroup);
        m_returnToStartCheckBox->setChecked(true);

        captureLayout->addRow(tr("Camera"), m_cameraCombo);
        captureLayout->addRow(tr("XY Stage"), m_stageCombo);
        captureLayout->addRow(tr("Rows"), m_rowsSpinBox);
        captureLayout->addRow(tr("Columns"), m_columnsSpinBox);
        captureLayout->addRow(tr("Pixel Size"), m_pixelSizeSpinBox);
        captureLayout->addRow(tr("Step X"), m_stepXSpinBox);
        captureLayout->addRow(tr("Step Y"), m_stepYSpinBox);
        captureLayout->addRow(tr("Settle"), m_settleMsSpinBox);
        captureLayout->addRow(QString(), m_returnToStartCheckBox);
        mainLayout->addWidget(captureGroup);

        auto* buttonLayout = new QGridLayout();
        m_startButton = new QPushButton(tr("Start"), this);
        m_stopButton = new QPushButton(tr("Stop"), this);
        m_stopButton->setEnabled(false);
        buttonLayout->addWidget(m_startButton, 0, 0);
        buttonLayout->addWidget(m_stopButton, 0, 1);
        mainLayout->addLayout(buttonLayout);

        m_statusLabel = new QLabel(tr("Ready"), this);
        m_statusLabel->setWordWrap(true);
        mainLayout->addWidget(m_statusLabel);

        auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(closeButtons, &QDialogButtonBox::rejected, this, &StageMosaicDialog::reject);
        mainLayout->addWidget(closeButtons);

        connect(m_startButton, &QPushButton::clicked, this, &StageMosaicDialog::startMosaic);
        connect(m_stopButton, &QPushButton::clicked, this, &StageMosaicDialog::stopMosaic);
    }

    // Refresh cameras and XY stages from the core
    void StageMosaicDialog::refreshDevices()
    {
        m_cameraCombo->clear();
        m_stageCombo->clear();

        m_cameraCombo->addItems(m_core->cameraIds());
        m_stageCombo->addItems(m_core->xyStageDevices());

        const QString currentStage = m_core->currentXYStageDevice();
        const int stageIndex = m_stageCombo->findText(currentStage);
        if (stageIndex >= 0)
        {
            m_stageCombo->setCurrentIndex(stageIndex);
        }
        const bool ready = m_cameraCombo->count() > 0 && m_stageCombo->count() > 0;
        m_startButton->setEnabled(ready);
        if (!ready)
        {
            m_statusLabel->setText(tr("Select a camera and XY stage"));
        }
    }

    QString StageMosaicDialog::selectedCameraId() const
    {
        return m_cameraCombo->currentText().trimmed();
    }

    QString StageMosaicDialog::selectedStageId() const
    {
        return m_stageCombo->currentText().trimmed();
    }

    // Start a grid mosaic from the current stage position
    void StageMosaicDialog::startMosaic()
    {
        m_activeCameraId.clear();
        m_activeStageId.clear();
        m_originValid = false;
        m_activeCameraId = selectedCameraId();
        m_activeStageId = selectedStageId();
        if (m_activeCameraId.isEmpty() || m_activeStageId.isEmpty())
        {
            m_statusLabel->setText(tr("Select a camera and XY stage"));
            return;
        }

        if (!m_core->readXYPosition(m_activeStageId, m_originX, m_originY))
        {
            m_statusLabel->setText(tr("Failed to read current stage position"));
            return;
        }

        m_originValid = true;
        m_core->startPreview(m_activeCameraId);
        m_currentTile = 0;
        m_mosaicInitialized = false;
        m_waitingForTileFrame = false;
        m_mosaic->sum.release();
        m_mosaic->count.release();
        m_mosaicError.clear();
        m_mosaicReferenceFrame = ImageFrame{};
        m_gallerySessionPublished = false;
        m_running = true;
        m_startButton->setEnabled(false);
        m_stopButton->setEnabled(true);
        m_cameraCombo->setEnabled(false);
        m_stageCombo->setEnabled(false);
        m_rowsSpinBox->setEnabled(false);
        m_columnsSpinBox->setEnabled(false);
        m_pixelSizeSpinBox->setEnabled(false);
        m_stepXSpinBox->setEnabled(false);
        m_stepYSpinBox->setEnabled(false);
        m_settleMsSpinBox->setEnabled(false);
        m_returnToStartCheckBox->setEnabled(false);
        m_statusLabel->setText(tr("Starting mosaic capture"));
        m_previewWidget->setVisibleLayerKeys({scopeone::core::ScopeOneCore::rawLayerKey(m_activeCameraId)});
        m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
        QTimer::singleShot(qMax(50, m_settleMsSpinBox->value()), this, &StageMosaicDialog::captureNextTile);
    }

    // Request cancellation after the current stage move
    void StageMosaicDialog::stopMosaic()
    {
        if (m_waitingForTileFrame)
        {
            finishMosaic(tr("Mosaic stopped"));
            return;
        }
        m_running = false;
        m_statusLabel->setText(tr("Stopping after current tile"));
    }

    // Move to the next tile and wait for camera settle
    void StageMosaicDialog::captureNextTile()
    {
        if (!m_running)
        {
            finishMosaic(tr("Mosaic stopped"));
            return;
        }

        const int rows = m_rowsSpinBox->value();
        const int columns = m_columnsSpinBox->value();
        const int totalTiles = rows * columns;
        if (m_currentTile >= totalTiles)
        {
            finishMosaic(tr("Mosaic complete with averaged overlap"), true);
            return;
        }

        const int row = m_currentTile / columns;
        const int column = m_currentTile % columns;
        const double x = m_originX + column * m_stepXSpinBox->value();
        const double y = m_originY + row * m_stepYSpinBox->value();

        m_statusLabel->setText(tr("Moving to tile %1 of %2").arg(m_currentTile + 1).arg(totalTiles));
        m_statusLabel->repaint();
        if (!m_core->moveXYTo(m_activeStageId, x, y))
        {
            finishMosaic(tr("Stage move failed"));
            return;
        }

        QTimer::singleShot(m_settleMsSpinBox->value(), this, &StageMosaicDialog::captureSettledTile);
    }

    // Capture the current settled frame into the mosaic
    void StageMosaicDialog::captureSettledTile()
    {
        if (!m_running)
        {
            finishMosaic(tr("Mosaic stopped"));
            return;
        }

        m_waitingForTileFrame = true;
        const int waitSerial = ++m_frameWaitSerial;
        m_statusLabel->setText(tr("Waiting for camera frame"));
        QTimer::singleShot(2000, this, [this, waitSerial]()
        {
            if (m_running && m_waitingForTileFrame && m_frameWaitSerial == waitSerial)
            {
                finishMosaic(tr("Timed out waiting for camera frame"));
            }
        });
    }

    // Use the first fresh frame after stage settle for the current tile
    void StageMosaicDialog::onRawFrameReady(const ImageFrame& frame)
    {
        if (!m_running || !m_waitingForTileFrame || frame.cameraId != m_activeCameraId)
        {
            return;
        }

        m_waitingForTileFrame = false;
        const int columns = m_columnsSpinBox->value();
        const int totalTiles = m_rowsSpinBox->value() * columns;
        const int row = m_currentTile / columns;
        const int column = m_currentTile % columns;

        if (!m_mosaicInitialized && !initializeMosaicFrame(frame))
        {
            finishMosaic(m_mosaicError.isEmpty()
                             ? tr("Failed to initialize mosaic")
                             : m_mosaicError);
            return;
        }

        if (!appendTileFrame(frame, row, column))
        {
            finishMosaic(tr("Tile frame did not match the mosaic format"));
            return;
        }

        if (!updatePreviewMosaic())
        {
            finishMosaic(tr("Failed to display mosaic preview"));
            return;
        }

        ++m_currentTile;
        m_statusLabel->setText(tr("Captured tile %1 of %2").arg(m_currentTile).arg(totalTiles));
        QTimer::singleShot(0, this, &StageMosaicDialog::captureNextTile);
    }

    // Restore state after mosaic capture ends
    void StageMosaicDialog::finishMosaic(const QString& message, bool addToGallery)
    {
        m_running = false;
        m_waitingForTileFrame = false;
        m_startButton->setEnabled(m_cameraCombo->count() > 0 && m_stageCombo->count() > 0);
        m_stopButton->setEnabled(false);
        m_cameraCombo->setEnabled(true);
        m_stageCombo->setEnabled(true);
        m_rowsSpinBox->setEnabled(true);
        m_columnsSpinBox->setEnabled(true);
        m_pixelSizeSpinBox->setEnabled(true);
        m_stepXSpinBox->setEnabled(true);
        m_stepYSpinBox->setEnabled(true);
        m_settleMsSpinBox->setEnabled(true);
        m_returnToStartCheckBox->setEnabled(true);
        m_statusLabel->setText(message);
        if (addToGallery)
        {
            publishMosaicToGallery();
        }

        const bool shouldReturnToStart = m_returnToStartCheckBox->isChecked()
                                         && m_originValid
                                         && !m_activeStageId.isEmpty();
        if (shouldReturnToStart)
        {
            m_core->moveXYTo(m_activeStageId, m_originX, m_originY);
        }
        m_originValid = false;
    }

    // Allocate the mosaic image from the first live frame
    bool StageMosaicDialog::initializeMosaicFrame(const ImageFrame& frame)
    {
        const cv::Mat tile = frameMatView(frame);
        if (tile.empty())
        {
            m_mosaicError = tr("No live frame available for mosaic");
            return false;
        }

        m_mosaic->tileWidth = tile.cols;
        m_mosaic->tileHeight = tile.rows;

        const double stepXUm = m_stepXSpinBox->value();
        const double stepYUm = m_stepYSpinBox->value();
        const double lastOffsetXUm = (m_columnsSpinBox->value() - 1) * stepXUm;
        const double lastOffsetYUm = (m_rowsSpinBox->value() - 1) * stepYUm;
        m_mosaic->minOffsetXUm = qMin(0.0, lastOffsetXUm);
        m_mosaic->minOffsetYUm = qMin(0.0, lastOffsetYUm);
        m_mosaic->pixelSizeUm = m_pixelSizeSpinBox->value();

        const double maxOffsetXPxValue = qAbs(lastOffsetXUm) / m_mosaic->pixelSizeUm;
        const double maxOffsetYPxValue = qAbs(lastOffsetYUm) / m_mosaic->pixelSizeUm;
        const double mosaicWidthValue = tile.cols + maxOffsetXPxValue;
        const double mosaicHeightValue = tile.rows + maxOffsetYPxValue;
        if (!std::isfinite(mosaicWidthValue)
            || !std::isfinite(mosaicHeightValue)
            || mosaicWidthValue > (std::numeric_limits<int>::max)()
            || mosaicHeightValue > (std::numeric_limits<int>::max)()
            || mosaicWidthValue * mosaicHeightValue > kMaxMosaicPixels)
        {
            m_mosaicError = tr("Mosaic output is too large");
            return false;
        }

        const int maxOffsetXPx = qRound(maxOffsetXPxValue);
        const int maxOffsetYPx = qRound(maxOffsetYPxValue);
        const int mosaicWidth = tile.cols + maxOffsetXPx;
        const int mosaicHeight = tile.rows + maxOffsetYPx;
        if (static_cast<qsizetype>(mosaicWidth) * mosaicHeight > kMaxMosaicPixels)
        {
            m_mosaicError = tr("Mosaic output is too large");
            return false;
        }

        m_mosaic->outputType = tile.type();
        m_mosaic->sum = cv::Mat::zeros(mosaicHeight,
                                       mosaicWidth,
                                       CV_32FC1);
        m_mosaic->count = cv::Mat::zeros(mosaicHeight,
                                         mosaicWidth,
                                         CV_32FC1);
        m_mosaicInitialized = true;
        return true;
    }

    // Add the latest camera frame to one grid slot
    bool StageMosaicDialog::appendTileFrame(const ImageFrame& frame, int row, int column)
    {
        if (frame.width != m_mosaic->tileWidth || frame.height != m_mosaic->tileHeight)
        {
            return false;
        }

        const int x = qRound((column * m_stepXSpinBox->value() - m_mosaic->minOffsetXUm)
                             / m_mosaic->pixelSizeUm);
        const int y = qRound((row * m_stepYSpinBox->value() - m_mosaic->minOffsetYUm)
                             / m_mosaic->pixelSizeUm);
        if (!accumulateTileAt(m_mosaic->sum, m_mosaic->count, frame, x, y))
        {
            return false;
        }
        m_mosaicReferenceFrame = frame;
        return true;
    }

    // Publish the mosaic image as a static graph layer
    bool StageMosaicDialog::updatePreviewMosaic()
    {
        const cv::Mat mosaicPreview = accumulatedPreviewMat(m_mosaic->sum,
                                                            m_mosaic->count,
                                                            m_mosaic->outputType);
        const ImageFrame mosaicFrame = frameFromMat(mosaicPreview, m_mosaicReferenceFrame);
        const QString layerId = QStringLiteral("stage_mosaic");
        const ImageFrame previewFrame = m_core->publishStaticFrame(
            layerId,
            mosaicFrame,
            tr("Stage Mosaic %1").arg(m_activeCameraId));
        if (!previewFrame.isValid())
        {
            return false;
        }
        const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(layerId);

        m_previewWidget->setLayerColormap(layerKey, QStringLiteral("Gray"));
        m_previewWidget->setLayerBlending(layerKey, QStringLiteral("Opaque"));
        m_previewWidget->setVisibleLayerKeys({layerKey});
        m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
        return true;
    }

    // Adds the completed mosaic image to the persistent gallery
    void StageMosaicDialog::publishMosaicToGallery()
    {
        const QString layerKey = scopeone::core::ScopeOneCore::staticLayerKey(QStringLiteral("stage_mosaic"));
        const ImageFrame mosaicFrame = m_core->graphFrame(layerKey);
        if (m_gallerySessionPublished || !mosaicFrame.isValid())
        {
            return;
        }

        auto session = mosaicSessionFromFrame(*m_core, mosaicFrame);
        m_gallerySessionPublished = true;
        emit gallerySessionCreated(session, tr("Stage Mosaic %1").arg(m_activeCameraId));
        m_statusLabel->setText(tr("Mosaic complete and added to Gallery"));
    }

    // Create a particle detection tool
    ParticleDetectionDialog::ParticleDetectionDialog(scopeone::core::ScopeOneCore* core,
                                                     PreviewWidget* previewWidget,
                                                     QWidget* parent)
        : QDialog(parent)
          , m_core(core)
          , m_previewWidget(previewWidget)
    {
        if (!core || !previewWidget)
        {
            qFatal("ParticleDetectionDialog requires ScopeOneCore and PreviewWidget");
        }

        setWindowTitle(tr("Particle Detection"));
        setupUI();
        refreshCameras();
    }

    // Stop automatic analysis before closing
    void ParticleDetectionDialog::reject()
    {
        m_autoUpdateTimer->stop();
        QDialog::reject();
    }

    // Build controls for threshold based particle detection
    void ParticleDetectionDialog::setupUI()
    {
        auto* mainLayout = new QVBoxLayout(this);
        auto* formLayout = new QFormLayout();

        m_cameraCombo = new QComboBox(this);
        m_thresholdSpinBox = new QSpinBox(this);
        m_thresholdSpinBox->setRange(0, 65535);
        m_thresholdSpinBox->setValue(128);
        m_thresholdSpinBox->setKeyboardTracking(false);

        m_minAreaSpinBox = new QSpinBox(this);
        m_minAreaSpinBox->setRange(1, 100000000);
        m_minAreaSpinBox->setValue(5);
        m_minAreaSpinBox->setKeyboardTracking(false);

        m_maxAreaSpinBox = new QSpinBox(this);
        m_maxAreaSpinBox->setRange(1, 100000000);
        m_maxAreaSpinBox->setValue(100000000);
        m_maxAreaSpinBox->setKeyboardTracking(false);

        formLayout->addRow(tr("Camera"), m_cameraCombo);
        formLayout->addRow(tr("Threshold"), m_thresholdSpinBox);
        formLayout->addRow(tr("Min Area"), m_minAreaSpinBox);
        formLayout->addRow(tr("Max Area"), m_maxAreaSpinBox);
        mainLayout->addLayout(formLayout);

        m_analyzeButton = new QPushButton(tr("Analyze Latest Frame"), this);
        mainLayout->addWidget(m_analyzeButton);

        m_autoUpdateCheckBox = new QCheckBox(tr("Auto Update"), this);
        mainLayout->addWidget(m_autoUpdateCheckBox);

        m_statusLabel = new QLabel(tr("Ready"), this);
        m_statusLabel->setWordWrap(true);
        mainLayout->addWidget(m_statusLabel);

        auto* closeButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
        connect(closeButtons, &QDialogButtonBox::rejected, this, &ParticleDetectionDialog::reject);
        mainLayout->addWidget(closeButtons);

        m_autoUpdateTimer = new QTimer(this);
        m_autoUpdateTimer->setInterval(500);

        connect(m_analyzeButton, &QPushButton::clicked, this, &ParticleDetectionDialog::analyzeCurrentFrame);
        connect(m_autoUpdateCheckBox, &QCheckBox::toggled, this, [this](bool enabled)
        {
            if (enabled)
            {
                m_autoUpdateTimer->start();
                analyzeCurrentFrame();
                return;
            }
            m_autoUpdateTimer->stop();
        });
        connect(m_autoUpdateTimer, &QTimer::timeout, this, &ParticleDetectionDialog::analyzeCurrentFrame);
    }

    // Refresh available cameras
    void ParticleDetectionDialog::refreshCameras()
    {
        m_cameraCombo->clear();
        m_cameraCombo->addItems(m_core->cameraIds());
        m_analyzeButton->setEnabled(m_cameraCombo->count() > 0);
        m_autoUpdateCheckBox->setEnabled(m_cameraCombo->count() > 0);
        if (m_cameraCombo->count() == 0)
        {
            m_statusLabel->setText(tr("Select a camera"));
        }
    }

    QString ParticleDetectionDialog::selectedCameraId() const
    {
        return m_cameraCombo->currentText().trimmed();
    }

    // Request preview once when the tool needs a fresh camera frame
    void ParticleDetectionDialog::ensurePreviewRunning(const QString& cameraId)
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty() || m_requestedPreviewCameraId == trimmedCameraId)
        {
            return;
        }

        m_core->startPreview(trimmedCameraId);
        m_requestedPreviewCameraId = trimmedCameraId;
    }

    // Detect connected bright particles in the latest frame
    void ParticleDetectionDialog::analyzeCurrentFrame()
    {
        const QString cameraId = selectedCameraId();
        if (cameraId.isEmpty())
        {
            m_statusLabel->setText(tr("Select a camera"));
            return;
        }
        ensurePreviewRunning(cameraId);

        const ImageFrame frame = m_core->graphFrame(ScopeOneCore::rawLayerKey(cameraId));
        if (!frame.isValid())
        {
            m_statusLabel->setText(tr("Waiting for live frame"));
            return;
        }
        const int maxPixelValue = frame.maxValue();
        if (m_thresholdSpinBox->maximum() != maxPixelValue)
        {
            QSignalBlocker blocker(m_thresholdSpinBox);
            m_thresholdSpinBox->setMaximum(maxPixelValue);
        }
        const qint64 framePixelCount = static_cast<qint64>(frame.width) * frame.height;
        const int maxParticleArea = static_cast<int>(
            qBound<qint64>(1,
                           framePixelCount,
                           static_cast<qint64>((std::numeric_limits<int>::max)())));
        if (m_minAreaSpinBox->maximum() != maxParticleArea)
        {
            QSignalBlocker minBlocker(m_minAreaSpinBox);
            QSignalBlocker maxBlocker(m_maxAreaSpinBox);
            m_minAreaSpinBox->setMaximum(maxParticleArea);
            m_maxAreaSpinBox->setMaximum(maxParticleArea);
        }

        const cv::Mat image = frameMatView(frame);
        if (image.empty())
        {
            m_statusLabel->setText(tr("Unsupported image format"));
            return;
        }

        cv::Mat mask;
        const int threshold = qBound(0, m_thresholdSpinBox->value(), maxPixelValue);
        cv::compare(image, cv::Scalar(threshold), mask, cv::CMP_GE);

        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        const int componentCount = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8);
        cv::Mat filtered = cv::Mat::zeros(mask.size(), CV_8UC1);

        const int minArea = m_minAreaSpinBox->value();
        if (m_maxAreaSpinBox->value() < minArea)
        {
            QSignalBlocker blocker(m_maxAreaSpinBox);
            m_maxAreaSpinBox->setValue(minArea);
        }
        const int maxArea = m_maxAreaSpinBox->value();
        int acceptedCount = 0;
        for (int label = 1; label < componentCount; ++label)
        {
            const int area = stats.at<int>(label, cv::CC_STAT_AREA);
            if (area < minArea || area > maxArea)
            {
                continue;
            }

            filtered.setTo(255, labels == label);
            ++acceptedCount;
        }

        ImageFrame maskFrame = frameFromMat(filtered, frame);
        const QString maskLayerId = QStringLiteral("particle_mask");
        const ImageFrame previewMaskFrame = m_core->publishStaticFrame(
            maskLayerId,
            maskFrame,
            tr("Particles %1").arg(cameraId));
        if (!previewMaskFrame.isValid())
        {
            m_statusLabel->setText(tr("Failed to display particle mask"));
            return;
        }
        const QString maskLayer = scopeone::core::ScopeOneCore::staticLayerKey(maskLayerId);

        m_previewWidget->setLayerColormap(maskLayer, QStringLiteral("Magenta"));
        m_previewWidget->setLayerOpacityPercent(maskLayer, 70);
        m_previewWidget->setLayerBlending(maskLayer, QStringLiteral("Additive"));
        m_previewWidget->setLayerVisible(scopeone::core::ScopeOneCore::rawLayerKey(cameraId), true);
        m_previewWidget->setVisibleLayerKeys({scopeone::core::ScopeOneCore::rawLayerKey(cameraId), maskLayer});
        m_previewWidget->setLayerLayoutMode(PreviewWidget::LayerLayoutMode::Overlay);
        m_statusLabel->setText(tr("Detected %1 particle(s)").arg(acceptedCount));
    }
}
