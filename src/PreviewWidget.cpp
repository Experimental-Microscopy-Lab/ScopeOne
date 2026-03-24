#include "PreviewWidget.h"
#include <QSurfaceFormat>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <algorithm>
#include <vector>

namespace scopeone::ui {

using scopeone::core::ImageFrame;
using scopeone::core::ImagePixelFormat;

namespace {

QString channelSelectionKey(const QString& cameraId, bool processed)
{
    return processed
        ? QStringLiteral("proc:%1").arg(cameraId)
        : QStringLiteral("raw:%1").arg(cameraId);
}

QSet<QString> validSelectionKeys(const QStringList& cameraIds)
{
    QSet<QString> keys;
    for (const QString& cameraId : cameraIds) {
        keys.insert(channelSelectionKey(cameraId, false));
        keys.insert(channelSelectionKey(cameraId, true));
    }
    return keys;
}

bool sampleFrameValue(const ImageFrame& frame, const QPoint& imagePos, int& outValue)
{
    if (!frame.isValid()
        || imagePos.x() < 0 || imagePos.y() < 0
        || imagePos.x() >= frame.width || imagePos.y() >= frame.height) {
        return false;
    }

    const char* rowData = frame.bytes.constData() + frame.stride * imagePos.y();
    if (frame.isMono8()) {
        const uchar* row = reinterpret_cast<const uchar*>(rowData);
        outValue = static_cast<int>(row[imagePos.x()]);
        return true;
    }
    if (frame.isMono16()) {
        const quint16* row = reinterpret_cast<const quint16*>(rowData);
        outValue = static_cast<int>(row[imagePos.x()]);
        return true;
    }
    return false;
}

QString gpuUnavailableText()
{
    return QStringLiteral("Preview unavailable\nOpenGL initialization failed on this system");
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

void PreviewWidget::setProcessedChannelFrame(const QString& channelId, const ImageFrame& frame)
{
    QMutexLocker lock(&m_mutex);
    ChannelTex& ch = m_channels[channelId];
    ch.processedFrame = frame;
    lock.unlock();

    if (frame.isValid() && !m_availableChannels.contains(channelId)) {
        m_availableChannels.append(channelId);
        setAvailableChannels(m_availableChannels);
        return;
    }
    updateImageDisplay();
}

void PreviewWidget::setChannelRaw(const ImageFrame& frame)
{
    {
        QMutexLocker lock(&m_mutex);
        ChannelTex& ch = m_channels[frame.cameraId];
        ch.rawFrame = frame;
    }
    if (frame.isValid()) {
        updateFpsOnFrame();

        CameraInfo& info = m_cameraInfos[frame.cameraId];
        info.cameraId = frame.cameraId;
        info.width = frame.width;
        info.height = frame.height;
        info.fps = m_lastFps;
        updateCameraInfoDisplay();
    }
    if (!m_availableChannels.contains(frame.cameraId)) {
        m_availableChannels.append(frame.cameraId);
        setAvailableChannels(m_availableChannels);
        return;
    }
    update();
}

void PreviewWidget::setChannelMode(ChannelMode mode)
{
    if (m_channelMode == mode) {
        return;
    }
    m_channelMode = mode;
    updateImageDisplay();
    emit channelModeChanged(m_channelMode);
}

void PreviewWidget::setOverlayAlphaPercent(int percent)
{
    percent = std::clamp(percent, 0, 100);
    if (m_overlayAlphaPercent == percent) {
        return;
    }
    m_overlayAlphaPercent = percent;
    updateImageDisplay();
}

int PreviewWidget::overlayAlphaPercent() const
{
    return m_overlayAlphaPercent;
}

PreviewWidget::ChannelMode PreviewWidget::channelMode() const
{
    return m_channelMode;
}

void PreviewWidget::setAvailableChannels(const QStringList& channelIds)
{
    const QStringList previousSelection = selectedChannels();
    m_availableChannels = channelIds;
    emit availableChannelsChanged(m_availableChannels);

    const QSet<QString> validKeys = validSelectionKeys(m_availableChannels);
    QSet<QString> nextSelection;
    for (const QString& channelId : previousSelection) {
        if (validKeys.contains(channelId)) {
            nextSelection.insert(channelId);
        }
    }
    if (nextSelection.isEmpty()) {
        for (const QString& id : m_availableChannels) {
            nextSelection.insert(channelSelectionKey(id, false));
        }
    }
    m_selectedChannels = std::move(nextSelection);
    emit selectedChannelsChanged(selectedChannels());
    updateImageDisplay();
}

void PreviewWidget::setSelectedChannels(const QStringList& channelIds)
{
    const QSet<QString> validKeys = validSelectionKeys(m_availableChannels);
    QSet<QString> nextSelection;
    for (const QString& channelId : channelIds) {
        if (validKeys.contains(channelId)) {
            nextSelection.insert(channelId);
        }
    }
    m_selectedChannels = std::move(nextSelection);
    emit selectedChannelsChanged(selectedChannels());
    updateImageDisplay();
}

QStringList PreviewWidget::availableChannels() const
{
    return m_availableChannels;
}

QStringList PreviewWidget::selectedChannels() const
{
    QStringList orderedSelection;
    orderedSelection.reserve(m_selectedChannels.size());
    for (const QString& cameraId : m_availableChannels) {
        const QString rawKey = channelSelectionKey(cameraId, false);
        if (m_selectedChannels.contains(rawKey)) {
            orderedSelection.append(rawKey);
        }
        const QString procKey = channelSelectionKey(cameraId, true);
        if (m_selectedChannels.contains(procKey)) {
            orderedSelection.append(procKey);
        }
    }
    for (const QString& channelId : m_selectedChannels) {
        if (!orderedSelection.contains(channelId)) {
            orderedSelection.append(channelId);
        }
    }
    return orderedSelection;
}

QString PreviewWidget::cameraInfoText() const
{
    return m_cameraInfoText;
}

void PreviewWidget::clearChannel(const QString& cameraId)
{
    m_cameraInfos.remove(cameraId);
    updateCameraInfoDisplay();

    QMutexLocker lock(&m_mutex);
    if (m_channels.contains(cameraId)) {
        m_channels.remove(cameraId);
        update();
    }
    lock.unlock();

    makeCurrent();
    QString rawKey = QString("raw:%1").arg(cameraId);
    QString procKey = QString("proc:%1").arg(cameraId);
    if (m_textureCache.contains(rawKey)) {
        glDeleteTextures(1, &m_textureCache[rawKey].texId);
        m_textureCache.remove(rawKey);
    }
    if (m_textureCache.contains(procKey)) {
        glDeleteTextures(1, &m_textureCache[procKey].texId);
        m_textureCache.remove(procKey);
    }
    doneCurrent();
}

void PreviewWidget::setZoomPercent(int percent)
{
    const int nextPercent = qBound(10, percent, 500);
    if (m_zoomPercent == nextPercent) {
        return;
    }
    m_zoomPercent = nextPercent;
    emit zoomLevelChanged(m_zoomPercent);
    update();
}

void PreviewWidget::setZoomLevel(int zoomPercent)
{
    setZoomPercent(zoomPercent);
}

int PreviewWidget::getZoomLevel() const
{
    return m_zoomPercent;
}

void PreviewWidget::setFitToWindow(bool enabled)
{
    if (m_fitToWindow == enabled) {
        return;
    }
    m_fitToWindow = enabled;
    if (m_fitToWindow) {
        m_viewOffset = QPoint();
    }
    emit fitToWindowChanged(m_fitToWindow);
    update();
}

bool PreviewWidget::isFitToWindow() const
{
    return m_fitToWindow;
}

void PreviewWidget::setPlaceholderText(const QString& text)
{
    m_placeholderText = text;
    update();
}

void PreviewWidget::setChannelOffset(const QString& cameraId, int offsetX, int offsetY)
{
    QMutexLocker lock(&m_mutex);
    ChannelTex& ch = m_channels[cameraId];
    ch.offsetX = offsetX;
    ch.offsetY = offsetY;
    update();
}

void PreviewWidget::setChannelFlip(const QString& cameraId, bool flipX, bool flipY)
{
    QMutexLocker lock(&m_mutex);
    ChannelTex& ch = m_channels[cameraId];
    ch.flipX = flipX;
    ch.flipY = flipY;
    update();
}

void PreviewWidget::setChannelZoomPercent(const QString& cameraId, int percent)
{
    QMutexLocker lock(&m_mutex);
    ChannelTex& ch = m_channels[cameraId];
    ch.zoomPercent = qBound(10, percent, 500);
    update();
}

void PreviewWidget::updateImageDisplay()
{
    // Refresh placeholder text from current stream state
    bool hasDisplayableChannel = false;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_channels.constBegin(); it != m_channels.constEnd(); ++it) {
            const ChannelTex& ch = it.value();
            if (hasRawDisplay(ch) || ch.processedFrame.isValid()) {
                hasDisplayableChannel = true;
                break;
            }
        }
    }

    if (hasDisplayableChannel) {
        m_placeholderText = m_selectedChannels.isEmpty()
            ? QStringLiteral("No channel selected")
            : QStringLiteral("No image loaded\nClick 'Start Preview' to view the camera feed");
        update();
        return;
    }

    m_placeholderText = QStringLiteral("No image loaded\nClick 'Start Preview' to view the camera feed");
    update();
}

void PreviewWidget::updateFpsOnFrame()
{
    if (!m_fpsTimer.isValid()) {
        m_fpsTimer.start();
        m_fpsFrameCounter = 0;
        m_lastFps = 0.0;
    }

    ++m_fpsFrameCounter;
    const qint64 elapsedMs = m_fpsTimer.elapsed();
    if (elapsedMs >= 3000) {
        m_lastFps = (m_fpsFrameCounter * 1000.0) / elapsedMs;
        m_fpsFrameCounter = 0;
        m_fpsTimer.restart();
    }
}

bool PreviewWidget::hasRawDisplay(const ChannelTex& ch) const
{
    return ch.rawFrame.isValid();
}

QSize PreviewWidget::rawDisplaySize(const ChannelTex& ch) const
{
    return ch.rawFrame.size();
}

QMap<QString, PreviewWidget::ChannelTex> PreviewWidget::snapshotChannels() const
{
    QMutexLocker lock(&m_mutex);
    return m_channels;
}

std::vector<PreviewWidget::ChannelInfo> PreviewWidget::buildChannelInfos(
    const QMap<QString, ChannelTex>& channels) const
{
    std::vector<ChannelInfo> allChannels;
    for (auto it = channels.constBegin(); it != channels.constEnd(); ++it) {
        const QString& id = it.key();
        const ChannelTex& ch = it.value();
        const bool hasImg = ch.processedFrame.isValid();
        const bool hasRaw = hasRawDisplay(ch);
        if (hasImg || hasRaw) {
            allChannels.push_back({id, &ch, hasImg, hasRaw});
        }
    }
    return allChannels;
}

bool PreviewWidget::resolveDisplayGeometry(const ChannelTex& ch,
                                             bool processed,
                                             const QRect& area,
                                             QRect& displayRect,
                                             QSize& imageSize) const
{
    if (processed) {
        if (!ch.processedFrame.isValid()) {
            return false;
        }
        displayRect = targetRectForFrame(ch.processedFrame, ch, area);
        imageSize = ch.processedFrame.size();
    } else {
        if (!hasRawDisplay(ch)) {
            return false;
        }
        displayRect = targetRectForRaw(ch, area);
        imageSize = rawDisplaySize(ch);
    }

    return imageSize.width() > 0
        && imageSize.height() > 0
        && displayRect.width() > 0
        && displayRect.height() > 0;
}

bool PreviewWidget::mapWidgetPositionToImage(const ChannelTex& ch,
                                               bool processed,
                                               const QRect& area,
                                               const QPoint& widgetPos,
                                               QPoint& imagePos) const
{
    QRect displayRect;
    QSize imageSize;
    if (!resolveDisplayGeometry(ch, processed, area, displayRect, imageSize)
        || !displayRect.contains(widgetPos)) {
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
    if (ch.flipX) {
        x = (imgW - 1) - x;
    }
    if (ch.flipY) {
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
    if (!item.info || !item.info->ch) {
        return;
    }

    const ChannelTex& ch = *item.info->ch;
    QRect displayRect;
    QSize imageSize;
    if (!resolveDisplayGeometry(ch, item.processed, item.area, displayRect, imageSize)) {
        return;
    }

    if (item.processed) {
        drawFrameInRect(QStringLiteral("proc:%1").arg(item.info->id),
                         ch.processedFrame,
                         displayRect,
                         item.alpha,
                         ch.flipX,
                         ch.flipY,
                         ch.processedLevelMin,
                         ch.processedLevelMax,
                         ch.processedLevelDomainMax);
        return;
    }

    if (ch.rawFrame.isValid()) {
        drawRawInRect(item.info->id, ch, displayRect, item.alpha);
        return;
    }
}

void PreviewWidget::updateCameraInfoDisplay()
{
    if (m_cameraInfos.isEmpty()) {
        m_cameraInfoText = QStringLiteral("No image loaded");
        emit cameraInfoTextChanged(m_cameraInfoText);
        return;
    }

    QStringList lines;
    for (auto it = m_cameraInfos.constBegin(); it != m_cameraInfos.constEnd(); ++it) {
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
    const QMap<QString, ChannelTex> channelsCopy = snapshotChannels();
    const std::vector<ChannelInfo> allChannels = buildChannelInfos(channelsCopy);
    if (allChannels.empty()) {
        return false;
    }

    const auto items = buildRenderItems(allChannels);
    for (const auto& item : items) {
        if (!item.info || !item.info->ch) {
            continue;
        }
        if (!item.area.contains(widgetPos)) {
            continue;
        }

        const ChannelTex& ch = *item.info->ch;
        QPoint imagePos;
        if (!mapWidgetPositionToImage(ch, item.processed, item.area, widgetPos, imagePos)) {
            continue;
        }

        outCameraId = item.info->id;
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
    const QMap<QString, ChannelTex> channelsCopy = snapshotChannels();
    const std::vector<ChannelInfo> allChannels = buildChannelInfos(channelsCopy);
    if (allChannels.empty()) {
        return false;
    }

    const auto items = buildRenderItems(allChannels);
    for (const auto& item : items) {
        if (!item.info || !item.info->ch || !item.area.contains(widgetPos)) {
            continue;
        }

        const ChannelTex& ch = *item.info->ch;
        QRect rect;
        QSize imageSize;
        if (!resolveDisplayGeometry(ch, item.processed, item.area, rect, imageSize)
            || !rect.contains(widgetPos)) {
            continue;
        }

        cameraId = item.info->id;
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
    if (cameraId.isEmpty()) {
        return false;
    }

    const QMap<QString, ChannelTex> channelsCopy = snapshotChannels();
    const std::vector<ChannelInfo> allChannels = buildChannelInfos(channelsCopy);
    if (allChannels.empty()) {
        return false;
    }

    const auto items = buildRenderItems(allChannels);
    for (const auto& item : items) {
        if (!item.info || !item.info->ch || item.info->id != cameraId) {
            continue;
        }
        if (!item.area.contains(widgetPos)) {
            continue;
        }

        const ChannelTex& ch = *item.info->ch;
        QPoint imagePos;
        if (!mapWidgetPositionToImage(ch, item.processed, item.area, widgetPos, imagePos)) {
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
    auto it = m_channels.find(cameraId);
    if (it == m_channels.end()) {
        return false;
    }

    const ChannelTex& ch = it.value();

    if (processed) {
        return sampleFrameValue(ch.processedFrame, imagePos, outValue);
    }

    if (ch.rawFrame.isValid()) {
        return sampleFrameValue(ch.rawFrame, imagePos, outValue);
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
    if (count <= 0) {
        return areas;
    }

    const int w = width();
    const int h = height();

    if (count == 1) {
        areas.push_back(QRect(0, 0, w, h));
        return areas;
    }

    if (count == 2) {
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

    if (count < static_cast<int>(areas.size())) {
        areas.resize(static_cast<size_t>(count));
    }
    return areas;
}

std::vector<PreviewWidget::RenderItem> PreviewWidget::buildRenderItems(const std::vector<ChannelInfo>& allChannels) const
{
    // Build the streams that will be drawn this frame
    std::vector<RenderItem> items;
    if (allChannels.empty()) {
        return items;
    }

    const QRect full(0, 0, width(), height());

    auto addItem = [&](const ChannelInfo* info, bool processed, const QRect& area, float alpha) {
        if (!info || !info->ch) {
            return;
        }
        if (processed && !info->hasImage) {
            return;
        }
        if (!processed && !info->hasRaw) {
            return;
        }
        items.push_back({info, processed, area, alpha});
    };

    auto isSelected = [&](const QString& cameraId, bool processed) {
        return m_selectedChannels.contains(channelSelectionKey(cameraId, processed));
    };

    auto buildSelectedStreams = [&](size_t maxCount) {
        std::vector<StreamItem> out;
        out.reserve(maxCount);

        // Raw streams get priority in the list
        for (const auto& info : allChannels) {
            if (info.hasRaw && isSelected(info.id, false)) {
                out.push_back({&info, false});
            }
            if (out.size() >= maxCount) {
                break;
            }
        }
        for (const auto& info : allChannels) {
            if (info.hasImage && isSelected(info.id, true)) {
                out.push_back({&info, true});
            }
            if (out.size() >= maxCount) {
                break;
            }
        }
        return out;
    };

    if (allChannels.size() > 1) {
        if (m_channelMode == ChannelMode::SideBySide) {
            QMap<QString, const ChannelInfo*> channelById;
            for (const auto& info : allChannels) {
                channelById.insert(info.id, &info);
            }

            std::vector<StreamItem> streamItems;
            streamItems.reserve(4);
            QSet<QString> addedKeys;

            auto tryAdd = [&](const QString& cameraId, bool processed) {
                const QString key = channelSelectionKey(cameraId, processed);
                if (!m_selectedChannels.contains(key) || addedKeys.contains(key)) {
                    return;
                }

                // Skip ids that have no data now
                const auto it = channelById.constFind(cameraId);
                if (it == channelById.constEnd() || it.value() == nullptr) {
                    return;
                }

                const ChannelInfo* info = it.value();
                if (processed && !info->hasImage) {
                    return;
                }
                if (!processed && !info->hasRaw) {
                    return;
                }

                streamItems.push_back({info, processed});
                addedKeys.insert(key);
            };

            for (const QString& cameraId : m_availableChannels) {
                tryAdd(cameraId, false);
            }
            // Show raw streams before processed ones
            for (const QString& cameraId : m_availableChannels) {
                tryAdd(cameraId, true);
            }

            for (const auto& info : allChannels) {
                if (!m_availableChannels.contains(info.id)) {
                    tryAdd(info.id, false);
                }
            }
            for (const auto& info : allChannels) {
                if (!m_availableChannels.contains(info.id)) {
                    tryAdd(info.id, true);
                }
            }

            const int count = static_cast<int>(std::min<size_t>(4, streamItems.size()));
            const auto areas = computeLayout(count);
            const size_t limit = std::min<size_t>(streamItems.size(), areas.size());
            // Draw at most four streams in the grid
            for (size_t i = 0; i < limit; ++i) {
                addItem(streamItems[i].info, streamItems[i].processed, areas[i], 1.0f);
            }
            return items;
        }

        const auto streams = buildSelectedStreams(2);
        if (streams.empty()) {
            return items;
        }
        // Overlay mode uses the same full area
        addItem(streams[0].info, streams[0].processed, full, 1.0f);
        if (streams.size() > 1) {
            addItem(streams[1].info, streams[1].processed, full,
                    qBound(0.0f, static_cast<float>(m_overlayAlphaPercent) / 100.0f, 1.0f));
        }
        return items;
    }

    const ChannelInfo& info = allChannels.front();
    if (info.hasRaw && info.hasImage) {
        if (m_channelMode == ChannelMode::SideBySide) {
            std::vector<StreamItem> streamItems;
            if (isSelected(info.id, false)) {
                streamItems.push_back({&info, false});
            }
            if (isSelected(info.id, true)) {
                streamItems.push_back({&info, true});
            }

            const auto areas = computeLayout(static_cast<int>(streamItems.size()));
            const size_t limit = std::min<size_t>(streamItems.size(), areas.size());
            for (size_t i = 0; i < limit; ++i) {
                addItem(streamItems[i].info, streamItems[i].processed, areas[i], 1.0f);
            }
        } else {
            if (isSelected(info.id, false)) {
                addItem(&info, false, full, 1.0f);
            }
            if (isSelected(info.id, true)) {
                addItem(&info, true, full,
                        qBound(0.0f, static_cast<float>(m_overlayAlphaPercent) / 100.0f, 1.0f));
            }
        }
    } else if (info.hasRaw && isSelected(info.id, false)) {
        addItem(&info, false, full, 1.0f);
    } else if (info.hasImage && isSelected(info.id, true)) {
        addItem(&info, true, full, 1.0f);
    }
    return items;
}

void PreviewWidget::paintGL()
{
    // Draw all visible streams from a stable snapshot
    if (!context() || !context()->isValid()) {
        return;
    }
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    applyViewportForRect(rect());
    glClear(GL_COLOR_BUFFER_BIT);

    const QMap<QString, ChannelTex> channelsCopy = snapshotChannels();

    bool canGpu = m_glInited && m_prog.isLinked();

    const std::vector<ChannelInfo> allChannels = buildChannelInfos(channelsCopy);

    if (allChannels.empty()) {
        paintPlaceholder(m_placeholderText);
        return;
    }

    if (canGpu) {
        // Draw everything with GL when the pipeline is ready
        const auto renderItems = buildRenderItems(allChannels);
        if (renderItems.empty()) {
            paintPlaceholder(m_placeholderText);
            return;
        }

        for (const auto& item : renderItems) {
            drawRenderItem(item);
        }

        if (m_roiDrawingMode && m_roiDragging) {
            QPainter p(this);
            QPen pen(QColor(0, 180, 255));
            pen.setWidth(1);
            pen.setStyle(Qt::DashLine);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            const QRect roiRect = QRect(m_roiStart, m_roiEnd).normalized();
            p.drawRect(roiRect);
        }
        if (m_lineVisible || (m_lineDrawingMode && m_lineDragging)) {
            QPainter p(this);
            QPen pen(QColor(255, 200, 0));
            pen.setWidth(2);
            if (m_lineProcessed) {
                pen.setStyle(Qt::DashLine);
            }
            p.setPen(pen);
            p.drawLine(m_lineStart, m_lineEnd);
        }
        return;
    }

    static bool warned = false;
    if (!warned) {
        // Keep one warning so the log stays readable
        qWarning() << "PreviewWidget: GPU rendering unavailable; no CPU fallback path is enabled";
        warned = true;
    }
    paintPlaceholder(gpuUnavailableText());
    return;
}

void PreviewWidget::setChannelDisplayLevels(const QString& cameraId,
                                            bool processed,
                                            int minLevel,
                                            int maxLevel,
                                            int maxPossible)
{
    QMutexLocker lock(&m_mutex);
    ChannelTex& ch = m_channels[cameraId];
    int& levelDomainMax = processed ? ch.processedLevelDomainMax : ch.rawLevelDomainMax;
    int& levelMinRef = processed ? ch.processedLevelMin : ch.rawLevelMin;
    int& levelMaxRef = processed ? ch.processedLevelMax : ch.rawLevelMax;
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
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
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
    if (!m_prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vs)) {
        qCritical() << "PreviewWidget: vertex shader compile FAILED - GPU rendering disabled" << m_prog.log();
        update();
        return;
    }
    if (!m_prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fs)) {
        qCritical() << "PreviewWidget: fragment shader compile FAILED - GPU rendering disabled" << m_prog.log();
        update();
        return;
    }
    if (!m_prog.link()) {
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

QRect PreviewWidget::targetRectForRaw(const ChannelTex& ch, const QRect& avail) const
{
    const QSize rawSize = rawDisplaySize(ch);
    const int w = rawSize.width();
    const int h = rawSize.height();
    if (w <= 0 || h <= 0 || avail.width() <= 0 || avail.height() <= 0) return avail;

    QSize s(w, h);
    if (m_fitToWindow) {
        s.scale(avail.size(), Qt::KeepAspectRatio);
        s = s * (ch.zoomPercent / 100.0);
    } else {
        const double z = (m_zoomPercent / 100.0) * (ch.zoomPercent / 100.0);
        s = s * z;
    }

    int x = avail.x() + (avail.width() - s.width())/2 + ch.offsetX;
    int y = avail.y() + (avail.height() - s.height())/2 + ch.offsetY;
    if (!m_fitToWindow) {
        x += m_viewOffset.x();
        y += m_viewOffset.y();
    }
    return QRect(QPoint(x,y), s);
}

void PreviewWidget::drawRawInRect(const QString& cameraId, const ChannelTex& ch, const QRect& r, float alpha)
{
    ensureGlPipeline();
    GLenum internal = 0, fmt = 0, type = 0;
    if (ch.rawFrame.pixelFormat == ImagePixelFormat::Mono16) {
        internal = GL_R16;
        fmt = GL_RED;
        type = GL_UNSIGNED_SHORT;
    } else if (ch.rawFrame.pixelFormat == ImagePixelFormat::Mono8) {
        internal = GL_R8;
        fmt = GL_RED;
        type = GL_UNSIGNED_BYTE;
    } else {
        return;
    }

    const int w = ch.rawFrame.width;
    const int h = ch.rawFrame.height;

    const QString cacheKey = QString("raw:%1").arg(cameraId);
    GLuint texId = getOrCreateTexture(cacheKey, w, h, internal);

    glBindTexture(GL_TEXTURE_2D, texId);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const int bpp = (type == GL_UNSIGNED_SHORT) ? 2 : 1;
    const int stride = ch.rawFrame.stride;
    if (stride > 0 && stride != w * bpp) {
        const int rowLen = stride / bpp;
        glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLen);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, type, ch.rawFrame.bytes.constData());
    if (stride > 0 && stride != w * bpp) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    m_prog.bind();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texId);
    m_prog.setUniformValue(m_uTex, 0);

    const int levelDomain = qMax(1, ch.rawLevelDomainMax);
    const float minNorm = static_cast<float>(ch.rawLevelMin) / static_cast<float>(levelDomain);
    const float maxNorm = static_cast<float>(ch.rawLevelMax) / static_cast<float>(levelDomain);
    m_prog.setUniformValue(m_uMinNorm, minNorm);
    m_prog.setUniformValue(m_uMaxNorm, maxNorm);
    const int bitDepth = qBound(8, ch.rawFrame.bitsPerSample, 16);
    const float bitMax = static_cast<float>((1u << bitDepth) - 1u);
    const float sampleMax = (internal == GL_R8) ? 255.0f : 65535.0f;
    const float texNormScale = sampleMax / bitMax;
    m_prog.setUniformValue(m_uTexNormScale, texNormScale);
    m_prog.setUniformValue(m_uAlpha, alpha);
    setUvTransform(ch.flipX, ch.flipY);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    applyViewportForRect(r);

    m_vao.bind();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao.release();
    glDisable(GL_BLEND);
    m_prog.release();
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

    if (frame.isMono16()) {
        uploadFormat = GL_RED;
        uploadType = GL_UNSIGNED_SHORT;
        internalFormat = GL_R16;
        unpackAlign = 2;
    } else if (!frame.isMono8()) {
        return;
    }

    GLuint texId = getOrCreateTexture(textureKey, frame.width, frame.height, internalFormat);

    glBindTexture(GL_TEXTURE_2D, texId);

    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlign);
    const int bytesPerPixel = (uploadType == GL_UNSIGNED_SHORT) ? 2 : 1;
    if (bytesPerPixel > 0 && frame.stride > 0) {
        const int rowPixels = frame.stride / bytesPerPixel;
        if (rowPixels != frame.width) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowPixels);
        }
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    frame.width, frame.height,
                    uploadFormat, uploadType, frame.bytes.constData());
    if (bytesPerPixel > 0) {
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


QRect PreviewWidget::targetRectForFrame(const ImageFrame& frame, const ChannelTex& ch, const QRect& avail) const
{
    if (!frame.isValid() || avail.width() <= 0 || avail.height() <= 0) return avail;

    QSize s = frame.size();
    if (m_fitToWindow) {
        s.scale(avail.size(), Qt::KeepAspectRatio);
        s = s * (ch.zoomPercent / 100.0);
    } else {
        const double z = (m_zoomPercent / 100.0) * (ch.zoomPercent / 100.0);
        s = s * z;
    }

    int x = avail.x() + (avail.width() - s.width()) / 2 + ch.offsetX;
    int y = avail.y() + (avail.height() - s.height()) / 2 + ch.offsetY;
    if (!m_fitToWindow) {
        x += m_viewOffset.x();
        y += m_viewOffset.y();
    }
    return QRect(QPoint(x, y), s);
}

void PreviewWidget::applyViewportForRect(const QRect& logicalRect)
{
    if (logicalRect.width() <= 0 || logicalRect.height() <= 0) {
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
    if (it != m_textureCache.end()) {
        CachedTexture& cached = it.value();
        if (cached.width == width && cached.height == height && cached.internalFormat == internalFormat) {
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
    for (auto it = m_textureCache.begin(); it != m_textureCache.end(); ++it) {
        glDeleteTextures(1, &it.value().texId);
    }
    m_textureCache.clear();
    doneCurrent();
}

void PreviewWidget::startROIDrawing(const QString& cameraId)
{
    if (m_lineDrawingMode) {
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
    if (!m_roiDrawingMode) {
        return;
    }
    m_roiDrawingMode = false;
    m_roiDragging = false;
    m_roiTargetCameraId.clear();
    unsetCursor();
    emit roiCancelled();
    update();
}

void PreviewWidget::startLineDrawing(const QString& cameraId)
{
    if (m_roiDrawingMode) {
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
    if (!m_lineDrawingMode) {
        return;
    }
    m_lineDrawingMode = false;
    m_lineDragging = false;
    m_lineTargetCameraId.clear();
    unsetCursor();
    emit lineDrawingCancelled();
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
    if (m_lineDrawingMode && event->button() == Qt::LeftButton) {
        QString cameraId = m_lineTargetCameraId;
        QPoint imagePos;
        bool processed = false;
        const bool ok = cameraId.isEmpty()
            ? widgetToImageCoords(event->pos(), cameraId, imagePos, processed)
            : widgetToImageCoordsForCamera(cameraId, event->pos(), imagePos, processed);
        if (!ok) {
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

    if (m_roiDrawingMode && event->button() == Qt::LeftButton) {
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
    if (m_lineDrawingMode && m_lineDragging) {
        m_lineEnd = event->pos();
        update();
        return;
    }

    if (m_roiDrawingMode && m_roiDragging) {
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
    if (m_lineDrawingMode && event->button() == Qt::LeftButton && m_lineDragging) {
        m_lineDragging = false;
        m_lineEnd = event->pos();

        QPoint imgStart;
        QPoint imgEnd;
        bool procStart = false;
        bool procEnd = false;
        const bool okStart = widgetToImageCoordsForCamera(m_lineTargetCameraId, m_lineStart, imgStart, procStart);
        const bool okEnd = widgetToImageCoordsForCamera(m_lineTargetCameraId, m_lineEnd, imgEnd, procEnd);
        if (!okStart || !okEnd || procStart != procEnd) {
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

    if (m_roiDrawingMode && event->button() == Qt::LeftButton && m_roiDragging) {
        m_roiDragging = false;

        QRect rect = QRect(m_roiStart, m_roiEnd).normalized();
        if (rect.width() < 10 || rect.height() < 10) {
            cancelROIDrawing();
            return;
        }

        QPoint imgStart;
        QPoint imgEnd;
        bool procStart = false;
        bool procEnd = false;
        const bool okStart = widgetToImageCoordsForCamera(m_roiTargetCameraId, m_roiStart, imgStart, procStart);
        const bool okEnd = widgetToImageCoordsForCamera(m_roiTargetCameraId, m_roiEnd, imgEnd, procEnd);
        if (!okStart || !okEnd || procStart != procEnd) {
            cancelROIDrawing();
            return;
        }

        const int imgX = qMin(imgStart.x(), imgEnd.x());
        const int imgY = qMin(imgStart.y(), imgEnd.y());
        const int imgW = qAbs(imgEnd.x() - imgStart.x());
        const int imgH = qAbs(imgEnd.y() - imgStart.y());
        if (imgW > 0 && imgH > 0) {
            emit roiDrawn(m_roiTargetCameraId, imgX, imgY, imgW, imgH);
        }

        cancelROIDrawing();
        return;
    }

    QOpenGLWidget::mouseReleaseEvent(event);
}

void PreviewWidget::wheelEvent(QWheelEvent* event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QOpenGLWidget::wheelEvent(event);
        return;
    }

    const int deltaY = event->angleDelta().y();
    if (deltaY == 0) {
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
    if (m_fitToWindow) {
        setFitToWindow(false);
    }
    setZoomPercent(m_zoomPercent + steps * 10);

    if (hasAnchor && !cameraId.isEmpty()) {
        ChannelTex ch;
        bool hasChannel = false;
        {
            QMutexLocker lock(&m_mutex);
            const auto it = m_channels.constFind(cameraId);
            if (it != m_channels.constEnd()) {
                ch = it.value();
                hasChannel = true;
            }
        }

        if (hasChannel) {
            QRect newRect;
            QSize imageSize;
            if (!resolveDisplayGeometry(ch, processed, itemArea, newRect, imageSize)) {
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
    if (m_lineDrawingMode && event->key() == Qt::Key_Escape) {
        cancelLineDrawing();
        event->accept();
        return;
    }

    if (m_roiDrawingMode && event->key() == Qt::Key_Escape) {
        cancelROIDrawing();
        event->accept();
        return;
    }

    QOpenGLWidget::keyPressEvent(event);
}

} // namespace scopeone::ui
