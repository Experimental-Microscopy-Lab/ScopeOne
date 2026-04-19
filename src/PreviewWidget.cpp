#include "PreviewWidget.h"
#include <QSurfaceFormat>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <algorithm>
#include <vector>

namespace scopeone::ui
{
    using scopeone::core::ImageFrame;

    namespace
    {
        QString streamSelectionKey(const QString& cameraId, bool processed)
        {
            return processed
                       ? QStringLiteral("proc:%1").arg(cameraId)
                       : QStringLiteral("raw:%1").arg(cameraId);
        }

        QSet<QString> validStreamSelectionKeys(const QStringList& cameraIds)
        {
            QSet<QString> keys;
            for (const QString& cameraId : cameraIds)
            {
                keys.insert(streamSelectionKey(cameraId, false));
                keys.insert(streamSelectionKey(cameraId, true));
            }
            return keys;
        }

        bool sampleFrameValue(const ImageFrame& frame, const QPoint& imagePos, int& outValue)
        {
            if (!frame.isValid()
                || imagePos.x() < 0 || imagePos.y() < 0
                || imagePos.x() >= frame.width || imagePos.y() >= frame.height)
            {
                return false;
            }

            const char* rowData = frame.bytes.constData() + frame.stride * imagePos.y();
            if (frame.isMono8())
            {
                const uchar* row = reinterpret_cast<const uchar*>(rowData);
                outValue = static_cast<int>(row[imagePos.x()]);
                return true;
            }
            if (frame.isMono16())
            {
                const quint16* row = reinterpret_cast<const quint16*>(rowData);
                outValue = static_cast<int>(row[imagePos.x()]);
                return true;
            }
            return false;
        }
    } // namespace

    PreviewWidget::PreviewWidget(QWidget* parent)
        : QOpenGLWidget(parent)
    {
        QSurfaceFormat requestedFormat;
        requestedFormat.setVersion(4, 6);
        requestedFormat.setProfile(QSurfaceFormat::CoreProfile);
        requestedFormat.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
        requestedFormat.setDepthBufferSize(0);
        setFormat(requestedFormat);

        setMinimumSize(256, 256);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    PreviewWidget::~PreviewWidget()
    {
        cleanupTextureCache();
    }

    void PreviewWidget::setProcessedFrame(const QString& cameraId, const ImageFrame& frame)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.processedFrame = frame;
        lock.unlock();

        if (frame.isValid() && registerAvailableCamera(cameraId))
        {
            return;
        }
        updateImageDisplay();
    }

    void PreviewWidget::setRawFrame(const ImageFrame& frame)
    {
        {
            QMutexLocker lock(&m_mutex);
            CameraFrameState& frameState = m_cameraFrames[frame.cameraId];
            frameState.rawFrame = frame;
        }
        if (frame.isValid())
        {
            updateFpsOnFrame();

            CameraInfo& info = m_cameraInfos[frame.cameraId];
            info.cameraId = frame.cameraId;
            info.width = frame.width;
            info.height = frame.height;
            info.fps = m_lastFps;
            updateCameraInfoDisplay();
        }
        if (registerAvailableCamera(frame.cameraId))
        {
            return;
        }
        update();
    }

    void PreviewWidget::setStreamLayoutMode(StreamLayoutMode mode)
    {
        if (m_streamLayoutMode == mode)
        {
            return;
        }
        m_streamLayoutMode = mode;
        updateImageDisplay();
        emit streamLayoutModeChanged(m_streamLayoutMode);
    }

    void PreviewWidget::setOverlayAlphaPercent(int percent)
    {
        percent = std::clamp(percent, 0, 100);
        if (m_overlayAlphaPercent == percent)
        {
            return;
        }
        m_overlayAlphaPercent = percent;
        updateImageDisplay();
    }

    int PreviewWidget::overlayAlphaPercent() const
    {
        return m_overlayAlphaPercent;
    }

    PreviewWidget::StreamLayoutMode PreviewWidget::streamLayoutMode() const
    {
        return m_streamLayoutMode;
    }

    void PreviewWidget::setAvailableCameraIds(const QStringList& cameraIds)
    {
        const QStringList previousSelection = selectedStreams();
        m_availableCameraIds = cameraIds;
        emit availableCameraIdsChanged(m_availableCameraIds);

        const QSet<QString> validKeys = validStreamSelectionKeys(m_availableCameraIds);
        QSet<QString> nextSelection;
        for (const QString& streamKey : previousSelection)
        {
            if (validKeys.contains(streamKey))
            {
                nextSelection.insert(streamKey);
            }
        }
        m_selectedStreams = std::move(nextSelection);
        emit selectedStreamsChanged(selectedStreams());
        updateImageDisplay();
    }

    void PreviewWidget::setSelectedStreams(const QStringList& streamKeys)
    {
        const QSet<QString> validKeys = validStreamSelectionKeys(m_availableCameraIds);
        QSet<QString> nextSelection;
        for (const QString& streamKey : streamKeys)
        {
            if (validKeys.contains(streamKey))
            {
                nextSelection.insert(streamKey);
            }
        }
        m_selectedStreams = std::move(nextSelection);
        emit selectedStreamsChanged(selectedStreams());
        updateImageDisplay();
    }

    QStringList PreviewWidget::availableCameraIds() const
    {
        return m_availableCameraIds;
    }

    QStringList PreviewWidget::selectedStreams() const
    {
        QStringList orderedSelection;
        orderedSelection.reserve(m_selectedStreams.size());
        for (const QString& cameraId : m_availableCameraIds)
        {
            const QString rawKey = streamSelectionKey(cameraId, false);
            if (m_selectedStreams.contains(rawKey))
            {
                orderedSelection.append(rawKey);
            }
            const QString procKey = streamSelectionKey(cameraId, true);
            if (m_selectedStreams.contains(procKey))
            {
                orderedSelection.append(procKey);
            }
        }
        for (const QString& streamKey : m_selectedStreams)
        {
            if (!orderedSelection.contains(streamKey))
            {
                orderedSelection.append(streamKey);
            }
        }
        return orderedSelection;
    }

    QString PreviewWidget::cameraInfoText() const
    {
        return m_cameraInfoText;
    }

    void PreviewWidget::clearCameraFrames(const QString& cameraId)
    {
        m_cameraInfos.remove(cameraId);
        updateCameraInfoDisplay();

        QMutexLocker lock(&m_mutex);
        if (m_cameraFrames.contains(cameraId))
        {
            m_cameraFrames.remove(cameraId);
            update();
        }
        lock.unlock();

        makeCurrent();
        const QString rawKey = streamSelectionKey(cameraId, false);
        const QString procKey = streamSelectionKey(cameraId, true);
        if (m_textureCache.contains(rawKey))
        {
            glDeleteTextures(1, &m_textureCache[rawKey].texId);
            m_textureCache.remove(rawKey);
        }
        if (m_textureCache.contains(procKey))
        {
            glDeleteTextures(1, &m_textureCache[procKey].texId);
            m_textureCache.remove(procKey);
        }
        doneCurrent();
    }

    bool PreviewWidget::registerAvailableCamera(const QString& cameraId)
    {
        if (cameraId.isEmpty() || m_availableCameraIds.contains(cameraId))
        {
            return false;
        }

        m_availableCameraIds.append(cameraId);
        setAvailableCameraIds(m_availableCameraIds);
        return true;
    }

    void PreviewWidget::setZoomPercent(int percent)
    {
        const int nextPercent = qBound(10, percent, 500);
        if (m_zoomPercent == nextPercent)
        {
            return;
        }
        m_zoomPercent = nextPercent;
        emit zoomLevelChanged(m_zoomPercent);
        update();
    }

    int PreviewWidget::zoomPercent() const
    {
        return m_zoomPercent;
    }

    void PreviewWidget::setFitToWindow(bool enabled)
    {
        if (m_fitToWindow == enabled)
        {
            return;
        }
        m_fitToWindow = enabled;
        if (m_fitToWindow)
        {
            m_viewOffset = QPoint();
        }
        emit fitToWindowChanged(m_fitToWindow);
        update();
    }

    bool PreviewWidget::isFitToWindow() const
    {
        return m_fitToWindow;
    }

    void PreviewWidget::setCameraOffset(const QString& cameraId, int offsetX, int offsetY)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.offsetX = offsetX;
        frameState.offsetY = offsetY;
        update();
    }

    void PreviewWidget::setCameraFlip(const QString& cameraId, bool flipX, bool flipY)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.flipX = flipX;
        frameState.flipY = flipY;
        update();
    }

    void PreviewWidget::setCameraZoomPercent(const QString& cameraId, int percent)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.zoomPercent = qBound(10, percent, 500);
        update();
    }

    void PreviewWidget::updateImageDisplay()
    {
        // Refresh placeholder text from current stream state
        bool hasDisplayableFrame = false;
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_cameraFrames.constBegin(); it != m_cameraFrames.constEnd(); ++it)
            {
                const CameraFrameState& frameState = it.value();
                if (hasRawFrame(frameState) || frameState.processedFrame.isValid())
                {
                    hasDisplayableFrame = true;
                    break;
                }
            }
        }

        if (hasDisplayableFrame)
        {
            m_placeholderText = m_selectedStreams.isEmpty()
                                    ? QStringLiteral("No stream selected")
                                    : QStringLiteral("No image loaded\nClick 'Start Preview' to view the camera feed");
            update();
            return;
        }

        m_placeholderText = QStringLiteral("No image loaded\nClick 'Start Preview' to view the camera feed");
        update();
    }

    void PreviewWidget::updateFpsOnFrame()
    {
        if (!m_fpsTimer.isValid())
        {
            m_fpsTimer.start();
            m_fpsFrameCounter = 0;
            m_lastFps = 0.0;
        }

        ++m_fpsFrameCounter;
        const qint64 elapsedMs = m_fpsTimer.elapsed();
        if (elapsedMs >= 3000)
        {
            m_lastFps = (m_fpsFrameCounter * 1000.0) / elapsedMs;
            m_fpsFrameCounter = 0;
            m_fpsTimer.restart();
        }
    }

    bool PreviewWidget::hasRawFrame(const CameraFrameState& frameState) const
    {
        return frameState.rawFrame.isValid();
    }

    QMap<QString, PreviewWidget::CameraFrameState> PreviewWidget::snapshotCameraFrames() const
    {
        QMutexLocker lock(&m_mutex);
        return m_cameraFrames;
    }

    std::vector<PreviewWidget::CameraRenderInfo> PreviewWidget::buildCameraRenderInfos(
        const QMap<QString, CameraFrameState>& cameraFrames) const
    {
        std::vector<CameraRenderInfo> cameraRenderInfos;
        for (auto it = cameraFrames.constBegin(); it != cameraFrames.constEnd(); ++it)
        {
            const QString& cameraId = it.key();
            const CameraFrameState& frameState = it.value();
            const bool hasProcessedFrame = frameState.processedFrame.isValid();
            const bool hasRawFrameNow = hasRawFrame(frameState);
            if (hasProcessedFrame || hasRawFrameNow)
            {
                cameraRenderInfos.push_back({cameraId, &frameState, hasProcessedFrame, hasRawFrameNow});
            }
        }
        return cameraRenderInfos;
    }

    void PreviewWidget::buildRenderSnapshot(QMap<QString, CameraFrameState>& cameraFrames,
                                            std::vector<CameraRenderInfo>& cameraRenderInfos,
                                            std::vector<RenderItem>& renderItems) const
    {
        cameraFrames = snapshotCameraFrames();
        cameraRenderInfos = buildCameraRenderInfos(cameraFrames);
        renderItems = buildRenderItems(cameraRenderInfos);
    }

    bool PreviewWidget::resolveDisplayGeometry(const CameraFrameState& frameState,
                                               bool processed,
                                               const QRect& area,
                                               QRect& displayRect,
                                               QSize& imageSize) const
    {
        if (processed)
        {
            if (!frameState.processedFrame.isValid())
            {
                return false;
            }
            imageSize = frameState.processedFrame.size();
            displayRect = targetRectForImageSize(imageSize, frameState, area);
        }
        else
        {
            if (!hasRawFrame(frameState))
            {
                return false;
            }
            imageSize = frameState.rawFrame.size();
            displayRect = targetRectForImageSize(imageSize, frameState, area);
        }

        return imageSize.width() > 0
            && imageSize.height() > 0
            && displayRect.width() > 0
            && displayRect.height() > 0;
    }

    bool PreviewWidget::mapWidgetPositionToImage(const CameraFrameState& frameState,
                                                 bool processed,
                                                 const QRect& area,
                                                 const QPoint& widgetPos,
                                                 QPoint& imagePos) const
    {
        QRect displayRect;
        QSize imageSize;
        if (!resolveDisplayGeometry(frameState, processed, area, displayRect, imageSize)
            || !displayRect.contains(widgetPos))
        {
            return false;
        }

        const int imgW = imageSize.width();
        const int imgH = imageSize.height();
        const double scaleX = imgW / static_cast<double>(displayRect.width());
        const double scaleY = imgH / static_cast<double>(displayRect.height());
        int x = static_cast<int>((widgetPos.x() - displayRect.x()) * scaleX);
        int y = static_cast<int>((widgetPos.y() - displayRect.y()) * scaleY);
        x = qBound(0, x, imgW - 1);
        y = qBound(0, y, imgH - 1);
        if (frameState.flipX)
        {
            x = (imgW - 1) - x;
        }
        if (frameState.flipY)
        {
            y = (imgH - 1) - y;
        }
        imagePos = QPoint(x, y);
        return true;
    }

    void PreviewWidget::paintPlaceholder(const QString& text)
    {
        QPainter painter(this);
        painter.setOpacity(1.0);
        painter.setPen(QColor(136, 136, 136));
        QFont placeholderFont = font();
        placeholderFont.setPointSizeF(placeholderFont.pointSizeF() + 1);
        painter.setFont(placeholderFont);
        painter.drawText(rect(), Qt::AlignCenter,
                         text.isEmpty() ? QStringLiteral("No image loaded") : text);
    }

    void PreviewWidget::drawRenderItem(const RenderItem& item)
    {
        if (!item.info || !item.info->frameState)
        {
            return;
        }

        const CameraFrameState& frameState = *item.info->frameState;
        QRect displayRect;
        QSize imageSize;
        if (!resolveDisplayGeometry(frameState, item.processed, item.area, displayRect, imageSize))
        {
            return;
        }

        if (item.processed)
        {
            drawFrameInRect(QStringLiteral("proc:%1").arg(item.info->cameraId),
                            frameState.processedFrame,
                            displayRect,
                            item.alpha,
                            frameState.flipX,
                            frameState.flipY,
                            frameState.processedLevelMin,
                            frameState.processedLevelMax,
                            frameState.processedLevelDomainMax);
            return;
        }

        if (frameState.rawFrame.isValid())
        {
            drawFrameInRect(QStringLiteral("raw:%1").arg(item.info->cameraId),
                            frameState.rawFrame,
                            displayRect,
                            item.alpha,
                            frameState.flipX,
                            frameState.flipY,
                            frameState.rawLevelMin,
                            frameState.rawLevelMax,
                            frameState.rawLevelDomainMax);
            return;
        }
    }

    void PreviewWidget::updateCameraInfoDisplay()
    {
        if (m_cameraInfos.isEmpty())
        {
            m_cameraInfoText = QStringLiteral("No image loaded");
            emit cameraInfoTextChanged(m_cameraInfoText);
            return;
        }

        QStringList lines;
        for (auto it = m_cameraInfos.constBegin(); it != m_cameraInfos.constEnd(); ++it)
        {
            const CameraInfo& info = it.value();
            lines.append(QString("%1: %2×%3 @ %4 FPS")
                         .arg(info.cameraId)
                         .arg(info.width)
                         .arg(info.height)
                         .arg(info.fps, 0, 'f', 1));
        }

        m_cameraInfoText = lines.join(QStringLiteral("\n"));
        emit cameraInfoTextChanged(m_cameraInfoText);
    }

    bool PreviewWidget::widgetToImageCoords(const QPoint& widgetPos,
                                            QString& outCameraId,
                                            QPoint& outImagePos,
                                            bool& outProcessed) const
    {
        // Resolve one widget point against the active render items
        QMap<QString, CameraFrameState> cameraFrames;
        std::vector<CameraRenderInfo> cameraRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(cameraFrames, cameraRenderInfos, renderItems);
        if (cameraRenderInfos.empty())
        {
            return false;
        }

        for (const auto& item : renderItems)
        {
            if (!item.info || !item.info->frameState)
            {
                continue;
            }
            if (!item.area.contains(widgetPos))
            {
                continue;
            }

            const CameraFrameState& frameState = *item.info->frameState;
            QPoint imagePos;
            if (!mapWidgetPositionToImage(frameState, item.processed, item.area, widgetPos, imagePos))
            {
                continue;
            }

            outCameraId = item.info->cameraId;
            outImagePos = imagePos;
            outProcessed = item.processed;
            return true;
        }

        return false;
    }

    bool PreviewWidget::locateRenderTarget(const QPoint& widgetPos,
                                           QString& cameraId,
                                           bool& processed,
                                           QRect& itemArea,
                                           QPointF& relativePos) const
    {
        QMap<QString, CameraFrameState> cameraFrames;
        std::vector<CameraRenderInfo> cameraRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(cameraFrames, cameraRenderInfos, renderItems);
        if (cameraRenderInfos.empty())
        {
            return false;
        }

        for (const auto& item : renderItems)
        {
            if (!item.info || !item.info->frameState || !item.area.contains(widgetPos))
            {
                continue;
            }

            const CameraFrameState& frameState = *item.info->frameState;
            QRect rect;
            QSize imageSize;
            if (!resolveDisplayGeometry(frameState, item.processed, item.area, rect, imageSize)
                || !rect.contains(widgetPos))
            {
                continue;
            }

            cameraId = item.info->cameraId;
            processed = item.processed;
            itemArea = item.area;
            relativePos = QPointF(
                static_cast<double>(widgetPos.x() - rect.x()) / static_cast<double>(rect.width()),
                static_cast<double>(widgetPos.y() - rect.y()) / static_cast<double>(rect.height()));
            return true;
        }

        return false;
    }

    bool PreviewWidget::widgetToImageCoordsForCamera(const QString& cameraId,
                                                     const QPoint& widgetPos,
                                                     QPoint& outImagePos,
                                                     bool& outProcessed) const
    {
        if (cameraId.isEmpty())
        {
            return false;
        }

        QMap<QString, CameraFrameState> cameraFrames;
        std::vector<CameraRenderInfo> cameraRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(cameraFrames, cameraRenderInfos, renderItems);
        if (cameraRenderInfos.empty())
        {
            return false;
        }

        for (const auto& item : renderItems)
        {
            if (!item.info || !item.info->frameState || item.info->cameraId != cameraId)
            {
                continue;
            }
            if (!item.area.contains(widgetPos))
            {
                continue;
            }

            const CameraFrameState& frameState = *item.info->frameState;
            QPoint imagePos;
            if (!mapWidgetPositionToImage(frameState, item.processed, item.area, widgetPos, imagePos))
            {
                continue;
            }

            outImagePos = imagePos;
            outProcessed = item.processed;
            return true;
        }

        return false;
    }

    bool PreviewWidget::getPixelValue(const QString& cameraId,
                                      const QPoint& imagePos,
                                      bool processed,
                                      int& outValue) const
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_cameraFrames.find(cameraId);
        if (it == m_cameraFrames.end())
        {
            return false;
        }

        const CameraFrameState& frameState = it.value();

        if (processed)
        {
            return sampleFrameValue(frameState.processedFrame, imagePos, outValue);
        }

        if (frameState.rawFrame.isValid())
        {
            return sampleFrameValue(frameState.rawFrame, imagePos, outValue);
        }

        return false;
    }

    void PreviewWidget::initializeGL()
    {
        initializeOpenGLFunctions();

        glDisable(GL_DEPTH_TEST);
        ensureGlPipeline();
    }

    void PreviewWidget::resizeGL(int w, int h)
    {
        Q_UNUSED(w);
        Q_UNUSED(h);
        applyViewportForRect(rect());
    }

    std::vector<QRect> PreviewWidget::computeLayout(int count) const
    {
        std::vector<QRect> areas;
        if (count <= 0)
        {
            return areas;
        }

        const int w = width();
        const int h = height();

        if (count == 1)
        {
            areas.push_back(QRect(0, 0, w, h));
            return areas;
        }

        if (count == 2)
        {
            areas.push_back(QRect(0, 0, w / 2, h));
            areas.push_back(QRect(w / 2, 0, w - w / 2, h));
            return areas;
        }

        const int cellW = w / 2;
        const int cellH = h / 2;
        areas.push_back(QRect(0, 0, cellW, cellH));
        areas.push_back(QRect(cellW, 0, w - cellW, cellH));
        areas.push_back(QRect(0, cellH, cellW, h - cellH));
        areas.push_back(QRect(cellW, cellH, w - cellW, h - cellH));

        if (count < static_cast<int>(areas.size()))
        {
            areas.resize(static_cast<size_t>(count));
        }
        return areas;
    }

    std::vector<PreviewWidget::RenderItem> PreviewWidget::buildRenderItems(
        const std::vector<CameraRenderInfo>& cameraRenderInfos) const
    {
        // Build the streams that will be drawn this frame
        std::vector<RenderItem> items;
        if (cameraRenderInfos.empty())
        {
            return items;
        }

        const QRect full(0, 0, width(), height());

        auto addItem = [&](const CameraRenderInfo* info, bool processed, const QRect& area, float alpha)
        {
            if (!info || !info->frameState)
            {
                return;
            }
            if (processed && !info->hasProcessedFrame)
            {
                return;
            }
            if (!processed && !info->hasRawFrame)
            {
                return;
            }
            items.push_back({info, processed, area, alpha});
        };

        auto isSelected = [&](const QString& cameraId, bool processed)
        {
            return m_selectedStreams.contains(streamSelectionKey(cameraId, processed));
        };

        auto buildSelectedStreams = [&](size_t maxCount)
        {
            std::vector<StreamItem> out;
            out.reserve(maxCount);

            // Raw streams get priority in the list
            for (const auto& info : cameraRenderInfos)
            {
                if (info.hasRawFrame && isSelected(info.cameraId, false))
                {
                    out.push_back({&info, false});
                }
                if (out.size() >= maxCount)
                {
                    break;
                }
            }
            for (const auto& info : cameraRenderInfos)
            {
                if (info.hasProcessedFrame && isSelected(info.cameraId, true))
                {
                    out.push_back({&info, true});
                }
                if (out.size() >= maxCount)
                {
                    break;
                }
            }
            return out;
        };

        if (cameraRenderInfos.size() > 1)
        {
            if (m_streamLayoutMode == StreamLayoutMode::SideBySide)
            {
                QMap<QString, const CameraRenderInfo*> renderInfoByCameraId;
                for (const auto& info : cameraRenderInfos)
                {
                    renderInfoByCameraId.insert(info.cameraId, &info);
                }

                std::vector<StreamItem> streamItems;
                streamItems.reserve(4);
                QSet<QString> addedKeys;

                auto tryAdd = [&](const QString& cameraId, bool processed)
                {
                    const QString key = streamSelectionKey(cameraId, processed);
                    if (!m_selectedStreams.contains(key) || addedKeys.contains(key))
                    {
                        return;
                    }

                    // Skip ids that have no data now
                    const auto it = renderInfoByCameraId.constFind(cameraId);
                    if (it == renderInfoByCameraId.constEnd() || it.value() == nullptr)
                    {
                        return;
                    }

                    const CameraRenderInfo* info = it.value();
                    if (processed && !info->hasProcessedFrame)
                    {
                        return;
                    }
                    if (!processed && !info->hasRawFrame)
                    {
                        return;
                    }

                    streamItems.push_back({info, processed});
                    addedKeys.insert(key);
                };

                for (const QString& cameraId : m_availableCameraIds)
                {
                    tryAdd(cameraId, false);
                }
                // Show raw streams before processed ones
                for (const QString& cameraId : m_availableCameraIds)
                {
                    tryAdd(cameraId, true);
                }

                for (const auto& info : cameraRenderInfos)
                {
                    if (!m_availableCameraIds.contains(info.cameraId))
                    {
                        tryAdd(info.cameraId, false);
                    }
                }
                for (const auto& info : cameraRenderInfos)
                {
                    if (!m_availableCameraIds.contains(info.cameraId))
                    {
                        tryAdd(info.cameraId, true);
                    }
                }

                const int count = static_cast<int>(std::min<size_t>(4, streamItems.size()));
                const auto areas = computeLayout(count);
                const size_t limit = std::min<size_t>(streamItems.size(), areas.size());
                // Draw at most four streams in the grid
                for (size_t i = 0; i < limit; ++i)
                {
                    addItem(streamItems[i].info, streamItems[i].processed, areas[i], 1.0f);
                }
                return items;
            }

            const auto streams = buildSelectedStreams(2);
            if (streams.empty())
            {
                return items;
            }
            // Overlay mode uses the same full area
            addItem(streams[0].info, streams[0].processed, full, 1.0f);
            if (streams.size() > 1)
            {
                addItem(streams[1].info, streams[1].processed, full,
                        qBound(0.0f, static_cast<float>(m_overlayAlphaPercent) / 100.0f, 1.0f));
            }
            return items;
        }

        const CameraRenderInfo& info = cameraRenderInfos.front();
        if (info.hasRawFrame && info.hasProcessedFrame)
        {
            if (m_streamLayoutMode == StreamLayoutMode::SideBySide)
            {
                std::vector<StreamItem> streamItems;
                if (isSelected(info.cameraId, false))
                {
                    streamItems.push_back({&info, false});
                }
                if (isSelected(info.cameraId, true))
                {
                    streamItems.push_back({&info, true});
                }

                const auto areas = computeLayout(static_cast<int>(streamItems.size()));
                const size_t limit = std::min<size_t>(streamItems.size(), areas.size());
                for (size_t i = 0; i < limit; ++i)
                {
                    addItem(streamItems[i].info, streamItems[i].processed, areas[i], 1.0f);
                }
            }
            else
            {
                if (isSelected(info.cameraId, false))
                {
                    addItem(&info, false, full, 1.0f);
                }
                if (isSelected(info.cameraId, true))
                {
                    addItem(&info, true, full,
                            qBound(0.0f, static_cast<float>(m_overlayAlphaPercent) / 100.0f, 1.0f));
                }
            }
        }
        else if (info.hasRawFrame && isSelected(info.cameraId, false))
        {
            addItem(&info, false, full, 1.0f);
        }
        else if (info.hasProcessedFrame && isSelected(info.cameraId, true))
        {
            addItem(&info, true, full, 1.0f);
        }
        return items;
    }

    void PreviewWidget::paintGL()
    {
        // Draw all visible streams from a stable snapshot
        if (!context() || !context()->isValid())
        {
            return;
        }
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        applyViewportForRect(rect());
        glClear(GL_COLOR_BUFFER_BIT);

        bool canGpu = m_glInited && m_prog.isLinked();
        QMap<QString, CameraFrameState> cameraFrames;
        std::vector<CameraRenderInfo> cameraRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(cameraFrames, cameraRenderInfos, renderItems);

        if (cameraRenderInfos.empty())
        {
            paintPlaceholder(m_placeholderText);
            return;
        }

        if (canGpu)
        {
            // Draw everything with GL when the pipeline is ready
            if (renderItems.empty())
            {
                paintPlaceholder(m_placeholderText);
                return;
            }

            for (const auto& item : renderItems)
            {
                drawRenderItem(item);
            }

            if (m_roiDrawingMode && m_roiDragging)
            {
                QPainter p(this);
                QPen pen(QColor(0, 180, 255));
                pen.setWidth(1);
                pen.setStyle(Qt::DashLine);
                p.setPen(pen);
                p.setBrush(Qt::NoBrush);
                const QRect roiRect = QRect(m_roiStart, m_roiEnd).normalized();
                p.drawRect(roiRect);
            }
            if (m_lineVisible || (m_lineDrawingMode && m_lineDragging))
            {
                QPainter p(this);
                QPen pen(QColor(255, 200, 0));
                pen.setWidth(2);
                if (m_lineProcessed)
                {
                    pen.setStyle(Qt::DashLine);
                }
                p.setPen(pen);
                p.drawLine(m_lineStart, m_lineEnd);
            }
            return;
        }

        static bool warned = false;
        if (!warned)
        {
            // Keep one warning so the log stays readable
            qWarning() << "PreviewWidget: GPU rendering unavailable; no CPU fallback path is enabled";
            warned = true;
        }
        paintPlaceholder(QStringLiteral(
            "Preview unavailable\nOpenGL initialization failed on this system"));
        return;
    }

    void PreviewWidget::setStreamDisplayLevels(const QString& cameraId,
                                               bool processed,
                                               int minLevel,
                                               int maxLevel,
                                               int maxPossible)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        int& levelDomainMax = processed ? frameState.processedLevelDomainMax : frameState.rawLevelDomainMax;
        int& levelMinRef = processed ? frameState.processedLevelMin : frameState.rawLevelMin;
        int& levelMaxRef = processed ? frameState.processedLevelMax : frameState.rawLevelMax;
        levelDomainMax = qMax(1, maxPossible);
        levelMinRef = qBound(0, minLevel, levelDomainMax);
        levelMaxRef = qBound(levelMinRef + 1, maxLevel, levelDomainMax);

        lock.unlock();
        update();
    }

    void PreviewWidget::ensureGlPipeline()
    {
        if (m_glInited) return;
        const float verts[] = {
            -1.f, -1.f, 0.f, 0.f,
            1.f, -1.f, 1.f, 0.f,
            -1.f, 1.f, 0.f, 1.f,
            1.f, 1.f, 1.f, 1.f,
        };
        m_vao.create();
        glGenBuffers(1, &m_vbo);
        m_vao.bind();
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

        const char* vs = R"(
        #version 330 core
        layout (location = 0) in vec2 aPos;
        layout (location = 1) in vec2 aUV;
        out vec2 vUV;
        uniform vec2 uUvScale;
        uniform vec2 uUvOffset;
        void main(){ vUV = aUV * uUvScale + uUvOffset; gl_Position = vec4(aPos, 0.0, 1.0); }
    )";
        const char* fs = R"(
        #version 330 core
        in vec2 vUV; out vec4 FragColor;
        uniform sampler2D uTex;
        uniform float uMinNorm;
        uniform float uMaxNorm;
        uniform float uTexNormScale;
        uniform float uAlpha;
        void main(){
            vec4 s = texture(uTex, vUV);
            float t0 = s.r * uTexNormScale;
            float t = clamp((t0 - uMinNorm) / max(uMaxNorm - uMinNorm, 1e-6), 0.0, 1.0);
            FragColor = vec4(t, t, t, 1.0) * uAlpha;
        }
    )";
        if (!m_prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vs))
        {
            qCritical() << "PreviewWidget: vertex shader compile FAILED - GPU rendering disabled" << m_prog.log();
            update();
            return;
        }
        if (!m_prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fs))
        {
            qCritical() << "PreviewWidget: fragment shader compile FAILED - GPU rendering disabled" << m_prog.log();
            update();
            return;
        }
        if (!m_prog.link())
        {
            qCritical() << "PreviewWidget: shader link FAILED - GPU rendering disabled" << m_prog.log();
            update();
            return;
        }
        m_prog.bind();
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        m_uTex = m_prog.uniformLocation("uTex");
        m_uMinNorm = m_prog.uniformLocation("uMinNorm");
        m_uMaxNorm = m_prog.uniformLocation("uMaxNorm");
        m_uTexNormScale = m_prog.uniformLocation("uTexNormScale");
        m_uAlpha = m_prog.uniformLocation("uAlpha");
        m_uUvScale = m_prog.uniformLocation("uUvScale");
        m_uUvOffset = m_prog.uniformLocation("uUvOffset");
        m_prog.release();
        m_vao.release();
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_glInited = true;
    }

    void PreviewWidget::setUvTransform(bool flipX, bool flipY)
    {
        const float sx = flipX ? -1.0f : 1.0f;
        const float sy = flipY ? -1.0f : 1.0f;
        const float ox = flipX ? 1.0f : 0.0f;
        const float oy = flipY ? 1.0f : 0.0f;

        if (m_uUvScale >= 0) m_prog.setUniformValue(m_uUvScale, sx, sy);
        if (m_uUvOffset >= 0) m_prog.setUniformValue(m_uUvOffset, ox, oy);
    }

    void PreviewWidget::drawFrameInRect(const QString& textureKey,
                                        const ImageFrame& frame,
                                        const QRect& r,
                                        float alpha,
                                        bool flipX,
                                        bool flipY,
                                        int levelMin,
                                        int levelMax,
                                        int levelDomainMax)
    {
        if (!frame.isValid() || r.width() <= 0 || r.height() <= 0) return;

        ensureGlPipeline();

        GLenum uploadFormat = GL_RED;
        GLenum uploadType = GL_UNSIGNED_BYTE;
        GLint internalFormat = GL_R8;
        int unpackAlign = 1;

        if (frame.isMono16())
        {
            uploadFormat = GL_RED;
            uploadType = GL_UNSIGNED_SHORT;
            internalFormat = GL_R16;
            unpackAlign = 2;
        }
        else if (!frame.isMono8())
        {
            return;
        }

        GLuint texId = getOrCreateTexture(textureKey, frame.width, frame.height, internalFormat);

        glBindTexture(GL_TEXTURE_2D, texId);

        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
        const int bytesPerPixel = (uploadType == GL_UNSIGNED_SHORT) ? 2 : 1;
        if (bytesPerPixel > 0 && frame.stride > 0)
        {
            const int rowPixels = frame.stride / bytesPerPixel;
            if (rowPixels != frame.width)
            {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowPixels);
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame.width, frame.height,
                        uploadFormat, uploadType, frame.bytes.constData());
        if (bytesPerPixel > 0)
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        }

        m_prog.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);
        m_prog.setUniformValue(m_uTex, 0);
        const int levelDomain = qMax(1, levelDomainMax);
        m_prog.setUniformValue(m_uMinNorm, static_cast<float>(levelMin) / static_cast<float>(levelDomain));
        m_prog.setUniformValue(m_uMaxNorm, static_cast<float>(levelMax) / static_cast<float>(levelDomain));
        const int bitDepth = qBound(8, frame.bitsPerSample, 16);
        const float bitMax = static_cast<float>((1u << bitDepth) - 1u);
        const float sampleMax = (internalFormat == GL_R16) ? 65535.0f : 255.0f;
        const float texNormScale = sampleMax / bitMax;
        m_prog.setUniformValue(m_uTexNormScale, texNormScale);
        m_prog.setUniformValue(m_uAlpha, alpha);
        setUvTransform(flipX, flipY);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        applyViewportForRect(r);

        m_vao.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_vao.release();
        glDisable(GL_BLEND);
        m_prog.release();
    }


    QRect PreviewWidget::targetRectForImageSize(const QSize& imageSize,
                                                const CameraFrameState& frameState,
                                                const QRect& avail) const
    {
        if (imageSize.width() <= 0 || imageSize.height() <= 0 || avail.width() <= 0 || avail.height() <= 0) return
            avail;

        QSize s = imageSize;
        if (m_fitToWindow)
        {
            s.scale(avail.size(), Qt::KeepAspectRatio);
            s = s * (frameState.zoomPercent / 100.0);
        }
        else
        {
            const double z = (m_zoomPercent / 100.0) * (frameState.zoomPercent / 100.0);
            s = s * z;
        }

        int x = avail.x() + (avail.width() - s.width()) / 2 + frameState.offsetX;
        int y = avail.y() + (avail.height() - s.height()) / 2 + frameState.offsetY;
        if (!m_fitToWindow)
        {
            x += m_viewOffset.x();
            y += m_viewOffset.y();
        }
        return QRect(QPoint(x, y), s);
    }

    void PreviewWidget::applyViewportForRect(const QRect& logicalRect)
    {
        if (logicalRect.width() <= 0 || logicalRect.height() <= 0)
        {
            return;
        }

        const qreal dpr = devicePixelRatioF();
        const int totalHeightPx = qMax(1, qRound(height() * dpr));
        const int xPx = qRound(logicalRect.x() * dpr);
        const int yPx = qRound(logicalRect.y() * dpr);
        const int wPx = qMax(1, qRound(logicalRect.width() * dpr));
        const int hPx = qMax(1, qRound(logicalRect.height() * dpr));
        const int glY = totalHeightPx - hPx - yPx;
        glViewport(xPx, glY, wPx, hPx);
    }

    GLuint PreviewWidget::getOrCreateTexture(const QString& key, int width, int height, GLenum internalFormat)
    {
        auto it = m_textureCache.find(key);
        if (it != m_textureCache.end())
        {
            CachedTexture& cached = it.value();
            if (cached.width == width && cached.height == height && cached.internalFormat == internalFormat)
            {
                return cached.texId;
            }
            glDeleteTextures(1, &cached.texId);
            m_textureCache.erase(it);
        }

        GLuint texId = 0;
        glGenTextures(1, &texId);
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const GLenum uploadFormat = GL_RED;
        const GLenum uploadType = (internalFormat == GL_R16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, uploadFormat, uploadType, nullptr);

        CachedTexture cached;
        cached.texId = texId;
        cached.width = width;
        cached.height = height;
        cached.internalFormat = internalFormat;
        m_textureCache[key] = cached;

        return texId;
    }

    void PreviewWidget::cleanupTextureCache()
    {
        makeCurrent();
        for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it)
        {
            glDeleteTextures(1, &it.value().texId);
        }
        m_textureCache.clear();
        doneCurrent();
    }

    void PreviewWidget::startROIDrawing(const QString& cameraId)
    {
        if (m_lineDrawingMode)
        {
            cancelLineDrawing();
        }
        m_roiDrawingMode = true;
        m_roiTargetCameraId = cameraId;
        m_roiDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    void PreviewWidget::cancelROIDrawing()
    {
        if (!m_roiDrawingMode)
        {
            return;
        }
        m_roiDrawingMode = false;
        m_roiDragging = false;
        m_roiTargetCameraId.clear();
        unsetCursor();
        update();
    }

    void PreviewWidget::startLineDrawing(const QString& cameraId)
    {
        if (m_roiDrawingMode)
        {
            cancelROIDrawing();
        }
        m_lineDrawingMode = true;
        m_lineTargetCameraId = cameraId;
        m_lineDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    void PreviewWidget::cancelLineDrawing()
    {
        if (!m_lineDrawingMode)
        {
            return;
        }
        m_lineDrawingMode = false;
        m_lineDragging = false;
        m_lineTargetCameraId.clear();
        unsetCursor();
        update();
    }

    void PreviewWidget::clearLine()
    {
        m_lineVisible = false;
        m_lineDragging = false;
        m_lineDrawingMode = false;
        m_lineProcessed = false;
        m_lineTargetCameraId.clear();
        unsetCursor();
        update();
    }

    void PreviewWidget::mousePressEvent(QMouseEvent* event)
    {
        emit mousePositionChanged(event->pos());
        if (m_lineDrawingMode && event->button() == Qt::LeftButton)
        {
            QString cameraId = m_lineTargetCameraId;
            QPoint imagePos;
            bool processed = false;
            const bool ok = cameraId.isEmpty()
                                ? widgetToImageCoords(event->pos(), cameraId, imagePos, processed)
                                : widgetToImageCoordsForCamera(cameraId, event->pos(), imagePos, processed);
            if (!ok)
            {
                return;
            }

            m_lineTargetCameraId = cameraId;
            m_lineStart = event->pos();
            m_lineEnd = event->pos();
            m_lineProcessed = processed;
            m_lineDragging = true;
            update();
            return;
        }

        if (m_roiDrawingMode && event->button() == Qt::LeftButton)
        {
            m_roiStart = event->pos();
            m_roiEnd = event->pos();
            m_roiDragging = true;
            update();
            return;
        }

        QOpenGLWidget::mousePressEvent(event);
    }

    void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
    {
        emit mousePositionChanged(event->pos());
        if (m_lineDrawingMode && m_lineDragging)
        {
            m_lineEnd = event->pos();
            update();
            return;
        }

        if (m_roiDrawingMode && m_roiDragging)
        {
            m_roiEnd = event->pos();
            update();
            return;
        }

        QOpenGLWidget::mouseMoveEvent(event);
    }

    void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
    {
        // Finish ROI and line drawing in image coordinates
        emit mousePositionChanged(event->pos());
        if (m_lineDrawingMode && event->button() == Qt::LeftButton && m_lineDragging)
        {
            m_lineDragging = false;
            m_lineEnd = event->pos();

            QPoint imgStart;
            QPoint imgEnd;
            bool procStart = false;
            bool procEnd = false;
            const bool okStart = widgetToImageCoordsForCamera(m_lineTargetCameraId, m_lineStart, imgStart, procStart);
            const bool okEnd = widgetToImageCoordsForCamera(m_lineTargetCameraId, m_lineEnd, imgEnd, procEnd);
            if (!okStart || !okEnd || procStart != procEnd)
            {
                cancelLineDrawing();
                return;
            }

            m_lineProcessed = procStart;
            m_lineVisible = true;
            m_lineDrawingMode = false;
            m_lineDragging = false;
            unsetCursor();
            emit lineDrawn(m_lineTargetCameraId,
                           imgStart.x(), imgStart.y(),
                           imgEnd.x(), imgEnd.y(),
                           procStart);
            update();
            return;
        }

        if (m_roiDrawingMode && event->button() == Qt::LeftButton && m_roiDragging)
        {
            m_roiDragging = false;

            QRect rect = QRect(m_roiStart, m_roiEnd).normalized();
            if (rect.width() < 10 || rect.height() < 10)
            {
                cancelROIDrawing();
                return;
            }

            QPoint imgStart;
            QPoint imgEnd;
            bool procStart = false;
            bool procEnd = false;
            const bool okStart = widgetToImageCoordsForCamera(m_roiTargetCameraId, m_roiStart, imgStart, procStart);
            const bool okEnd = widgetToImageCoordsForCamera(m_roiTargetCameraId, m_roiEnd, imgEnd, procEnd);
            if (!okStart || !okEnd || procStart != procEnd)
            {
                cancelROIDrawing();
                return;
            }

            const int imgX = qMin(imgStart.x(), imgEnd.x());
            const int imgY = qMin(imgStart.y(), imgEnd.y());
            const int imgW = qAbs(imgEnd.x() - imgStart.x());
            const int imgH = qAbs(imgEnd.y() - imgStart.y());
            if (imgW > 0 && imgH > 0)
            {
                emit roiDrawn(m_roiTargetCameraId, imgX, imgY, imgW, imgH);
            }

            cancelROIDrawing();
            return;
        }

        QOpenGLWidget::mouseReleaseEvent(event);
    }

    void PreviewWidget::wheelEvent(QWheelEvent* event)
    {
        if (!(event->modifiers() & Qt::ControlModifier))
        {
            QOpenGLWidget::wheelEvent(event);
            return;
        }

        const int deltaY = event->angleDelta().y();
        if (deltaY == 0)
        {
            event->accept();
            return;
        }

        const int steps = (deltaY / 120 != 0) ? (deltaY / 120) : ((deltaY > 0) ? 1 : -1);
        QString cameraId;
        bool processed = false;
        QRect itemArea;
        QPointF relativePos;
        const bool hasAnchor = locateRenderTarget(event->position().toPoint(),
                                                  cameraId,
                                                  processed,
                                                  itemArea,
                                                  relativePos);
        if (m_fitToWindow)
        {
            setFitToWindow(false);
        }
        setZoomPercent(m_zoomPercent + steps * 10);

        if (hasAnchor && !cameraId.isEmpty())
        {
            CameraFrameState frameState;
            bool hasFrameState = false;
            {
                QMutexLocker lock(&m_mutex);
                const auto it = m_cameraFrames.constFind(cameraId);
                if (it != m_cameraFrames.constEnd())
                {
                    frameState = it.value();
                    hasFrameState = true;
                }
            }

            if (hasFrameState)
            {
                QRect newRect;
                QSize imageSize;
                if (!resolveDisplayGeometry(frameState, processed, itemArea, newRect, imageSize))
                {
                    event->accept();
                    return;
                }
                const int desiredX = qRound(event->position().x() - relativePos.x() * newRect.width());
                const int desiredY = qRound(event->position().y() - relativePos.y() * newRect.height());
                m_viewOffset += QPoint(desiredX - newRect.x(), desiredY - newRect.y());
                update();
            }
        }
        event->accept();
    }

    void PreviewWidget::keyPressEvent(QKeyEvent* event)
    {
        if (m_lineDrawingMode && event->key() == Qt::Key_Escape)
        {
            cancelLineDrawing();
            event->accept();
            return;
        }

        if (m_roiDrawingMode && event->key() == Qt::Key_Escape)
        {
            cancelROIDrawing();
            event->accept();
            return;
        }

        QOpenGLWidget::keyPressEvent(event);
    }
} // namespace scopeone::ui
