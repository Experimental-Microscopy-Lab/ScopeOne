#include "PreviewWidget.h"
#include <QSurfaceFormat>
#include <QPainter>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>
#include <vector>

namespace scopeone::ui
{
    using scopeone::core::ImageFrame;

    namespace
    {
        // Builds a stable layer key for raw or processed preview
        QString previewLayerKey(const QString& cameraId, bool processed)
        {
            return processed
                       ? QStringLiteral("proc:%1").arg(cameraId)
                       : QStringLiteral("raw:%1").arg(cameraId);
        }

        // Extracts the camera id from a layer key
        QString cameraIdFromLayerKey(const QString& key)
        {
            const int separator = key.indexOf(QLatin1Char(':'));
            return separator >= 0 ? key.mid(separator + 1) : key;
        }

        // Checks whether a layer key points to processed preview
        bool isProcessedLayerKey(const QString& key)
        {
            return key.startsWith(QStringLiteral("proc:"));
        }

        // Builds all valid layer keys for available cameras
        QSet<QString> validPreviewLayerKeys(const QStringList& cameraIds)
        {
            QSet<QString> keys;
            for (const QString& cameraId : cameraIds)
            {
                keys.insert(previewLayerKey(cameraId, false));
                keys.insert(previewLayerKey(cameraId, true));
            }
            return keys;
        }

        // Reads one mono pixel value from a frame
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

        // Sample a straight line through one frame
        bool sampleLineValues(const ImageFrame& frame,
                              const QPoint& start,
                              const QPoint& end,
                              QVector<int>& outValues)
        {
            const int dx = end.x() - start.x();
            const int dy = end.y() - start.y();
            const int steps = qMax(qAbs(dx), qAbs(dy));
            outValues.clear();
            outValues.reserve(steps + 1);
            for (int i = 0; i <= steps; ++i)
            {
                const double t = (steps == 0) ? 0.0 : static_cast<double>(i) / static_cast<double>(steps);
                const QPoint point(qRound(start.x() + dx * t), qRound(start.y() + dy * t));
                int value = 0;
                if (sampleFrameValue(frame, point, value))
                {
                    outValues.push_back(value);
                }
            }
            return !outValues.isEmpty();
        }

        // Clips one widget line to a display rectangle
        bool clipLineToRect(const QPoint& start,
                            const QPoint& end,
                            const QRect& rect,
                            QPoint& clippedStart,
                            QPoint& clippedEnd)
        {
            if (rect.isEmpty())
            {
                return false;
            }

            const double left = rect.left();
            const double right = rect.right();
            const double top = rect.top();
            const double bottom = rect.bottom();
            const double x0 = start.x();
            const double y0 = start.y();
            const double dx = end.x() - start.x();
            const double dy = end.y() - start.y();
            double u1 = 0.0;
            double u2 = 1.0;

            const auto clipBoundary = [&u1, &u2](double p, double q)
            {
                if (qFuzzyIsNull(p))
                {
                    return q >= 0.0;
                }
                const double r = q / p;
                if (p < 0.0)
                {
                    if (r > u2)
                    {
                        return false;
                    }
                    if (r > u1)
                    {
                        u1 = r;
                    }
                    return true;
                }
                if (r < u1)
                {
                    return false;
                }
                if (r < u2)
                {
                    u2 = r;
                }
                return true;
            };

            if (!clipBoundary(-dx, x0 - left)
                || !clipBoundary(dx, right - x0)
                || !clipBoundary(-dy, y0 - top)
                || !clipBoundary(dy, bottom - y0)
                || u1 > u2)
            {
                return false;
            }

            clippedStart = QPoint(qBound(rect.left(), qRound(x0 + u1 * dx), rect.right()),
                                  qBound(rect.top(), qRound(y0 + u1 * dy), rect.bottom()));
            clippedEnd = QPoint(qBound(rect.left(), qRound(x0 + u2 * dx), rect.right()),
                                qBound(rect.top(), qRound(y0 + u2 * dy), rect.bottom()));
            return clippedStart != clippedEnd;
        }
    } // namespace

    // Creates the OpenGL preview widget
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

    // Releases cached OpenGL textures
    PreviewWidget::~PreviewWidget()
    {
        cleanupTextureCache();
    }

    // Stores one processed frame and updates layer statistics
    void PreviewWidget::setProcessedFrame(const QString& cameraId, const ImageFrame& frame)
    {
        ensureLayersForCamera(cameraId);
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        const bool hadProcessedFrame = frameState.processedFrame.isValid();
        frameState.processedFrame = frame;
        lock.unlock();

        if (frame.isValid())
        {
            const QString layerKey = previewLayerKey(cameraId, true);
            const FpsUpdate fps = updateFpsOnFrame(layerKey);

            LayerInfo& info = m_layerInfos[layerKey];
            const bool sizeChanged = info.width != frame.width || info.height != frame.height;
            info.width = frame.width;
            info.height = frame.height;
            info.fps = fps.fps;
            if (sizeChanged || fps.changed)
            {
                updateLayerInfoDisplay();
            }
        }

        const bool registeredCamera = frame.isValid() && registerAvailableCamera(cameraId);
        if (frame.isValid() && !hadProcessedFrame)
        {
            emit availableLayerKeysChanged(availableLayerKeys());
        }

        if (registeredCamera)
        {
            return;
        }
        updateImageDisplay();
    }

    // Stores one raw frame and updates layer statistics
    void PreviewWidget::setRawFrame(const ImageFrame& frame)
    {
        ensureLayersForCamera(frame.cameraId);
        {
            QMutexLocker lock(&m_mutex);
            CameraFrameState& frameState = m_cameraFrames[frame.cameraId];
            frameState.rawFrame = frame;
        }
        if (frame.isValid())
        {
            const QString layerKey = previewLayerKey(frame.cameraId, false);
            const FpsUpdate fps = updateFpsOnFrame(layerKey);

            LayerInfo& info = m_layerInfos[layerKey];
            const bool sizeChanged = info.width != frame.width || info.height != frame.height;
            info.width = frame.width;
            info.height = frame.height;
            info.fps = fps.fps;
            if (sizeChanged || fps.changed)
            {
                updateLayerInfoDisplay();
            }
        }
        if (registerAvailableCamera(frame.cameraId))
        {
            return;
        }
        update();
    }

    // Changes how multiple preview layers are laid out
    void PreviewWidget::setLayerLayoutMode(LayerLayoutMode mode)
    {
        if (m_layerLayoutMode == mode)
        {
            return;
        }
        m_layerLayoutMode = mode;
        updateImageDisplay();
        emit layerLayoutModeChanged(m_layerLayoutMode);
    }

    PreviewWidget::LayerLayoutMode PreviewWidget::layerLayoutMode() const
    {
        return m_layerLayoutMode;
    }

    // Replaces the available camera list and keeps valid layer selections
    void PreviewWidget::setAvailableCameraIds(const QStringList& cameraIds)
    {
        const QStringList previousSelection = selectedLayerKeys();
        m_availableCameraIds = cameraIds;
        emit availableCameraIdsChanged(m_availableCameraIds);

        const QSet<QString> validKeys = validLayerKeys();
        removeInvalidLayers(validKeys);
        for (const QString& cameraId : m_availableCameraIds)
        {
            ensureLayersForCamera(cameraId);
        }

        QSet<QString> nextSelection;
        for (const QString& layerKey : previousSelection)
        {
            if (validKeys.contains(layerKey))
            {
                nextSelection.insert(layerKey);
            }
        }
        for (auto it = m_layers.begin(); it != m_layers.end(); ++it)
        {
            it->visible = validKeys.contains(it.key()) && nextSelection.contains(it.key());
        }
        emit selectedLayerKeysChanged(selectedLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
    }

    // Sets the visible preview layers
    void PreviewWidget::setSelectedLayerKeys(const QStringList& layerKeys)
    {
        const QSet<QString> validKeys = validLayerKeys();
        QSet<QString> nextSelection;
        for (const QString& layerKey : layerKeys)
        {
            if (validKeys.contains(layerKey))
            {
                nextSelection.insert(layerKey);
            }
        }
        removeInvalidLayers(validKeys);
        for (const QString& cameraId : m_availableCameraIds)
        {
            ensureLayersForCamera(cameraId);
        }
        for (auto it = m_layers.begin(); it != m_layers.end(); ++it)
        {
            it->visible = validKeys.contains(it.key()) && nextSelection.contains(it.key());
        }
        emit selectedLayerKeysChanged(selectedLayerKeys());
        updateImageDisplay();
    }

    // Sets visibility for one preview layer
    void PreviewWidget::setLayerVisible(const QString& layerKey, bool visible)
    {
        const QSet<QString> validKeys = validLayerKeys();
        if (!validKeys.contains(layerKey))
        {
            return;
        }

        ensureLayer(layerKey);
        LayerDisplaySettings& display = m_layers[layerKey];
        if (display.visible == visible)
        {
            return;
        }

        display.visible = visible;
        emit selectedLayerKeysChanged(selectedLayerKeys());
        updateImageDisplay();
    }

    // Sets opacity for one preview layer
    void PreviewWidget::setLayerOpacityPercent(const QString& layerKey, int percent)
    {
        if (!validLayerKeys().contains(layerKey))
        {
            return;
        }

        ensureLayer(layerKey);
        LayerDisplaySettings& display = m_layers[layerKey];
        const int nextPercent = qBound(0, percent, 100);
        if (display.opacityPercent == nextPercent)
        {
            return;
        }

        display.opacityPercent = nextPercent;
        update();
    }

    // Sets gamma for one preview layer
    void PreviewWidget::setLayerGamma(const QString& layerKey, double gamma)
    {
        if (!validLayerKeys().contains(layerKey))
        {
            return;
        }

        ensureLayer(layerKey);
        LayerDisplaySettings& display = m_layers[layerKey];
        const double nextGamma = std::clamp(gamma, 0.2, 2.0);
        if (qFuzzyCompare(display.gamma, nextGamma))
        {
            return;
        }

        display.gamma = nextGamma;
        update();
    }

    // Sets colormap for one preview layer
    void PreviewWidget::setLayerColormap(const QString& layerKey, const QString& colormap)
    {
        if (!validLayerKeys().contains(layerKey)
            || !supportedLayerColormaps().contains(colormap, Qt::CaseInsensitive))
        {
            return;
        }

        const Colormap nextColormap = colormapFromName(colormap);
        ensureLayer(layerKey);
        LayerDisplaySettings& display = m_layers[layerKey];
        if (display.colormap == nextColormap)
        {
            return;
        }

        display.colormap = nextColormap;
        update();
    }

    // Sets blending for one preview layer
    void PreviewWidget::setLayerBlending(const QString& layerKey, const QString& blending)
    {
        const QString normalized = blending.trimmed().toLower().replace(QStringLiteral("_"), QStringLiteral(" "));
        const bool supported = normalized == QStringLiteral("translucent")
                               || normalized == QStringLiteral("additive")
                               || normalized == QStringLiteral("minimum")
                               || normalized == QStringLiteral("opaque")
                               || normalized == QStringLiteral("multiplicative");
        if (!validLayerKeys().contains(layerKey) || !supported)
        {
            return;
        }

        const Blending nextBlending = blendingFromName(blending);
        ensureLayer(layerKey);
        LayerDisplaySettings& display = m_layers[layerKey];
        if (display.blending == nextBlending)
        {
            return;
        }

        display.blending = nextBlending;
        update();
    }

    // Sets display intensity levels for one preview layer
    void PreviewWidget::setLayerDisplayLevels(const QString& layerKey,
                                              int minLevel,
                                              int maxLevel,
                                              int maxPossible)
    {
        if (!validLayerKeys().contains(layerKey))
        {
            return;
        }

        ensureLayer(layerKey);
        LayerDisplaySettings& display = m_layers[layerKey];
        display.levelDomainMax = qMax(1, maxPossible);
        display.levelMin = qBound(0, minLevel, display.levelDomainMax);
        display.levelMax = qBound(display.levelMin + 1, maxLevel, display.levelDomainMax);
        update();
    }

    // Moves one layer in draw order
    void PreviewWidget::moveLayer(const QString& layerKey, int offset)
    {
        if (offset == 0 || !validLayerKeys().contains(layerKey))
        {
            return;
        }

        ensureLayer(layerKey);
        const int currentIndex = static_cast<int>(m_layerOrder.indexOf(layerKey));
        const int maxIndex = static_cast<int>(m_layerOrder.size()) - 1;
        const int nextIndex = qBound(0, currentIndex + offset, maxIndex);
        if (currentIndex == nextIndex)
        {
            return;
        }

        m_layerOrder.move(currentIndex, nextIndex);
        emit selectedLayerKeysChanged(selectedLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
    }

    // Adds a static image frame as a preview layer
    QString PreviewWidget::setStaticLayerFrame(const QString& layerId,
                                               const QString& displayName,
                                               const ImageFrame& frame)
    {
        const QString normalizedId = layerId.trimmed();
        if (normalizedId.isEmpty() || !frame.isValid())
        {
            return {};
        }

        const QString cameraId = QStringLiteral("static:%1").arg(normalizedId);
        const QString layerKey = previewLayerKey(cameraId, false);
        ImageFrame staticFrame = frame;
        staticFrame.cameraId = cameraId;
        const bool hadStaticLayer = m_staticCameraIds.contains(cameraId);
        const bool wasVisible = m_layers.value(layerKey).visible;

        m_staticCameraIds.insert(cameraId);
        m_layerNames.insert(layerKey,
                            displayName.trimmed().isEmpty() ? frame.cameraId : displayName.trimmed());
        ensureLayer(layerKey);
        m_layers[layerKey].visible = true;

        {
            QMutexLocker lock(&m_mutex);
            CameraFrameState& frameState = m_cameraFrames[cameraId];
            frameState.rawFrame = staticFrame;
        }

        LayerInfo& info = m_layerInfos[layerKey];
        info.width = staticFrame.width;
        info.height = staticFrame.height;
        info.fps = 0.0;

        updateLayerInfoDisplay();
        if (!hadStaticLayer)
        {
            emit availableLayerKeysChanged(availableLayerKeys());
        }
        if (!wasVisible)
        {
            emit selectedLayerKeysChanged(selectedLayerKeys());
        }
        emit staticLayerFrameChanged(layerKey, staticFrame);
        updateImageDisplay();
        return layerKey;
    }

    // Removes one static image layer from the preview
    bool PreviewWidget::removeStaticLayer(const QString& layerKey)
    {
        const QString cameraId = cameraIdFromLayerKey(layerKey);
        if (layerKey != previewLayerKey(cameraId, false) || !m_staticCameraIds.contains(cameraId))
        {
            return false;
        }

        removeStaticLayerData(cameraId);
        updateLayerInfoDisplay();
        emit selectedLayerKeysChanged(selectedLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
        return true;
    }

    // Removes all static image layers from the preview
    void PreviewWidget::clearStaticLayers()
    {
        if (m_staticCameraIds.isEmpty())
        {
            return;
        }

        QStringList cameraIds;
        cameraIds.reserve(m_staticCameraIds.size());
        for (const QString& cameraId : m_staticCameraIds)
        {
            cameraIds.append(cameraId);
        }
        for (const QString& cameraId : cameraIds)
        {
            removeStaticLayerData(cameraId);
        }

        updateLayerInfoDisplay();
        emit selectedLayerKeysChanged(selectedLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
    }

    QStringList PreviewWidget::availableCameraIds() const
    {
        return m_availableCameraIds;
    }

    QStringList PreviewWidget::availableLayerKeys() const
    {
        QStringList availableKeys;
        QSet<QString> availableSet;

        QMap<QString, CameraFrameState> cameraFrames = snapshotCameraFrames();
        for (const QString& cameraId : m_availableCameraIds)
        {
            const QString rawKey = previewLayerKey(cameraId, false);
            availableKeys.append(rawKey);
            availableSet.insert(rawKey);

            const auto it = cameraFrames.constFind(cameraId);
            if (it != cameraFrames.constEnd() && it.value().processedFrame.isValid())
            {
                const QString processedKey = previewLayerKey(cameraId, true);
                availableKeys.append(processedKey);
                availableSet.insert(processedKey);
            }
        }
        for (const QString& cameraId : m_staticCameraIds)
        {
            const QString layerKey = previewLayerKey(cameraId, false);
            availableKeys.append(layerKey);
            availableSet.insert(layerKey);
        }

        QStringList layerKeys;
        layerKeys.reserve(availableKeys.size());
        for (const QString& layerKey : m_layerOrder)
        {
            if (availableSet.contains(layerKey))
            {
                layerKeys.append(layerKey);
            }
        }
        for (const QString& layerKey : availableKeys)
        {
            if (!layerKeys.contains(layerKey))
            {
                layerKeys.append(layerKey);
            }
        }
        return layerKeys;
    }

    QStringList PreviewWidget::selectedLayerKeys() const
    {
        QStringList orderedSelection;
        orderedSelection.reserve(m_layers.size());
        const QSet<QString> validKeys = validLayerKeys();
        for (const QString& layerKey : m_layerOrder)
        {
            if (validKeys.contains(layerKey) && m_layers.value(layerKey).visible)
            {
                orderedSelection.append(layerKey);
            }
        }
        for (auto it = m_layers.constBegin(); it != m_layers.constEnd(); ++it)
        {
            if (validKeys.contains(it.key()) && it.value().visible && !orderedSelection.contains(it.key()))
            {
                orderedSelection.append(it.key());
            }
        }
        return orderedSelection;
    }

    int PreviewWidget::layerOpacityPercent(const QString& layerKey) const
    {
        return layerDisplaySettings(layerKey).opacityPercent;
    }

    double PreviewWidget::layerGamma(const QString& layerKey) const
    {
        return layerDisplaySettings(layerKey).gamma;
    }

    QString PreviewWidget::layerColormap(const QString& layerKey) const
    {
        return colormapName(layerDisplaySettings(layerKey).colormap);
    }

    QStringList PreviewWidget::supportedLayerColormaps() const
    {
        return {
            QStringLiteral("Gray"),
            QStringLiteral("Green"),
            QStringLiteral("Magenta"),
            QStringLiteral("Cyan"),
            QStringLiteral("Red"),
            QStringLiteral("Blue"),
            QStringLiteral("Yellow"),
            QStringLiteral("Fire"),
        };
    }

    QString PreviewWidget::layerBlending(const QString& layerKey) const
    {
        return blendingName(layerDisplaySettings(layerKey).blending);
    }

    QStringList PreviewWidget::supportedLayerBlendingModes() const
    {
        return {
            QStringLiteral("Translucent"),
            QStringLiteral("Additive"),
            QStringLiteral("Minimum"),
            QStringLiteral("Opaque"),
            QStringLiteral("Multiplicative"),
        };
    }

    QString PreviewWidget::layerName(const QString& layerKey) const
    {
        const auto nameIt = m_layerNames.constFind(layerKey);
        if (nameIt != m_layerNames.constEnd())
        {
            return nameIt.value();
        }

        const QString cameraId = cameraIdFromLayerKey(layerKey);
        return isProcessedLayerKey(layerKey)
                   ? QStringLiteral("%1 Processed").arg(cameraId)
                   : QStringLiteral("%1 Raw").arg(cameraId);
    }

    QString PreviewWidget::layerInfoText(const QString& layerKey) const
    {
        const auto it = m_layerInfos.constFind(layerKey);
        if (it == m_layerInfos.constEnd())
        {
            return {};
        }

        const LayerInfo& info = it.value();
        if (info.width <= 0 || info.height <= 0)
        {
            return {};
        }

        const QString cameraId = cameraIdFromLayerKey(layerKey);
        if (m_staticCameraIds.contains(cameraId))
        {
            return QStringLiteral("%1x%2")
                .arg(info.width)
                .arg(info.height);
        }

        return QStringLiteral("%1x%2 @ %3 FPS")
            .arg(info.width)
            .arg(info.height)
            .arg(info.fps, 0, 'f', 1);
    }

    QString PreviewWidget::layerInfoSummaryText() const
    {
        return m_layerInfoText;
    }

    // Clears preview state for one camera
    void PreviewWidget::clearCameraFrames(const QString& cameraId)
    {
        m_layerInfos.remove(previewLayerKey(cameraId, false));
        m_layerInfos.remove(previewLayerKey(cameraId, true));
        m_fpsStates.remove(previewLayerKey(cameraId, false));
        m_fpsStates.remove(previewLayerKey(cameraId, true));
        updateLayerInfoDisplay();

        QMutexLocker lock(&m_mutex);
        if (m_cameraFrames.contains(cameraId))
        {
            m_cameraFrames.remove(cameraId);
            update();
        }
        lock.unlock();

        makeCurrent();
        const QString rawKey = previewLayerKey(cameraId, false);
        const QString procKey = previewLayerKey(cameraId, true);
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
        emit availableLayerKeysChanged(availableLayerKeys());
    }

    // Clears all processed preview frames and removes their layers
    void PreviewWidget::clearProcessedFrames()
    {
        for (const QString& cameraId : m_availableCameraIds)
        {
            m_layerInfos.remove(previewLayerKey(cameraId, true));
            m_fpsStates.remove(previewLayerKey(cameraId, true));
            m_layers.remove(previewLayerKey(cameraId, true));
        }
        updateLayerInfoDisplay();

        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_cameraFrames.begin(); it != m_cameraFrames.end(); ++it)
            {
                it->processedFrame = ImageFrame();
            }
        }

        makeCurrent();
        for (const QString& cameraId : m_availableCameraIds)
        {
            const QString procKey = previewLayerKey(cameraId, true);
            if (m_textureCache.contains(procKey))
            {
                glDeleteTextures(1, &m_textureCache[procKey].texId);
                m_textureCache.remove(procKey);
            }
        }
        doneCurrent();

        emit selectedLayerKeysChanged(selectedLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
    }

    // Registers a camera when its first frame arrives
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

    // Builds default display settings for a raw or processed layer
    PreviewWidget::LayerDisplaySettings PreviewWidget::defaultLayerDisplaySettings(bool processed) const
    {
        LayerDisplaySettings settings;
        settings.visible = false;
        settings.opacityPercent = 100;
        settings.gamma = 1.0;
        settings.colormap = processed ? Colormap::Green : Colormap::Gray;
        settings.blending = Blending::Translucent;
        settings.levelMin = 0;
        settings.levelMax = 255;
        settings.levelDomainMax = 255;
        return settings;
    }

    // Returns display settings for one layer id
    PreviewWidget::LayerDisplaySettings PreviewWidget::layerDisplaySettings(const QString& layerKey) const
    {
        const auto it = m_layers.constFind(layerKey);
        if (it != m_layers.constEnd())
        {
            return it.value();
        }
        return defaultLayerDisplaySettings(isProcessedLayerKey(layerKey));
    }

    PreviewWidget::Colormap PreviewWidget::colormapFromName(const QString& name) const
    {
        const QString normalized = name.trimmed().toLower();
        if (normalized == QStringLiteral("green")) return Colormap::Green;
        if (normalized == QStringLiteral("magenta")) return Colormap::Magenta;
        if (normalized == QStringLiteral("cyan")) return Colormap::Cyan;
        if (normalized == QStringLiteral("red")) return Colormap::Red;
        if (normalized == QStringLiteral("blue")) return Colormap::Blue;
        if (normalized == QStringLiteral("yellow")) return Colormap::Yellow;
        if (normalized == QStringLiteral("fire")) return Colormap::Fire;
        return Colormap::Gray;
    }

    QString PreviewWidget::colormapName(Colormap colormap) const
    {
        switch (colormap)
        {
        case Colormap::Green:
            return QStringLiteral("Green");
        case Colormap::Magenta:
            return QStringLiteral("Magenta");
        case Colormap::Cyan:
            return QStringLiteral("Cyan");
        case Colormap::Red:
            return QStringLiteral("Red");
        case Colormap::Blue:
            return QStringLiteral("Blue");
        case Colormap::Yellow:
            return QStringLiteral("Yellow");
        case Colormap::Fire:
            return QStringLiteral("Fire");
        case Colormap::Gray:
            return QStringLiteral("Gray");
        }
        return QStringLiteral("Gray");
    }

    PreviewWidget::Blending PreviewWidget::blendingFromName(const QString& name) const
    {
        const QString normalized = name.trimmed().toLower().replace(QStringLiteral("_"), QStringLiteral(" "));
        if (normalized == QStringLiteral("additive")) return Blending::Additive;
        if (normalized == QStringLiteral("minimum")) return Blending::Minimum;
        if (normalized == QStringLiteral("opaque")) return Blending::Opaque;
        if (normalized == QStringLiteral("multiplicative")) return Blending::Multiplicative;
        return Blending::Translucent;
    }

    QString PreviewWidget::blendingName(Blending blending) const
    {
        switch (blending)
        {
        case Blending::Additive:
            return QStringLiteral("Additive");
        case Blending::Minimum:
            return QStringLiteral("Minimum");
        case Blending::Opaque:
            return QStringLiteral("Opaque");
        case Blending::Multiplicative:
            return QStringLiteral("Multiplicative");
        case Blending::Translucent:
            return QStringLiteral("Translucent");
        }
        return QStringLiteral("Translucent");
    }

    // Ensures one layer id has display settings
    void PreviewWidget::ensureLayer(const QString& layerKey)
    {
        if (layerKey.isEmpty())
        {
            return;
        }
        if (!m_layers.contains(layerKey))
        {
            m_layers.insert(layerKey, defaultLayerDisplaySettings(isProcessedLayerKey(layerKey)));
        }
        if (!m_layerOrder.contains(layerKey))
        {
            m_layerOrder.append(layerKey);
        }
    }

    // Ensures raw and processed layers exist for one camera
    void PreviewWidget::ensureLayersForCamera(const QString& cameraId)
    {
        if (cameraId.isEmpty())
        {
            return;
        }
        ensureLayer(previewLayerKey(cameraId, false));
        ensureLayer(previewLayerKey(cameraId, true));
    }

    // Removes stored state for one static image camera
    void PreviewWidget::removeStaticLayerData(const QString& cameraId)
    {
        const QString layerKey = previewLayerKey(cameraId, false);
        m_staticCameraIds.remove(cameraId);
        m_layerInfos.remove(layerKey);
        m_fpsStates.remove(layerKey);
        m_layers.remove(layerKey);
        m_layerNames.remove(layerKey);
        m_layerOrder.removeAll(layerKey);

        {
            QMutexLocker lock(&m_mutex);
            m_cameraFrames.remove(cameraId);
        }

        auto textureIt = m_textureCache.find(layerKey);
        if (textureIt != m_textureCache.end())
        {
            GLuint texId = textureIt.value().texId;
            makeCurrent();
            glDeleteTextures(1, &texId);
            doneCurrent();
            m_textureCache.erase(textureIt);
        }
    }

    // Removes layers that are no longer valid for the current camera list
    void PreviewWidget::removeInvalidLayers(const QSet<QString>& validKeys)
    {
        const QStringList layerIds = m_layers.keys();
        for (const QString& layerId : layerIds)
        {
            if (!validKeys.contains(layerId))
            {
                m_layers.remove(layerId);
            }
        }
        for (int i = static_cast<int>(m_layerOrder.size()) - 1; i >= 0; --i)
        {
            if (!validKeys.contains(m_layerOrder.at(i)))
            {
                m_layerOrder.removeAt(i);
            }
        }
    }

    QSet<QString> PreviewWidget::validLayerKeys() const
    {
        QSet<QString> keys = validPreviewLayerKeys(m_availableCameraIds);
        for (const QString& cameraId : m_staticCameraIds)
        {
            keys.insert(previewLayerKey(cameraId, false));
        }
        return keys;
    }

    // Sets the global preview zoom percentage
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

    // Enables or disables fit to window display mode
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

    // Sets manual pixel offset for one camera layer source
    void PreviewWidget::setCameraOffset(const QString& cameraId, int offsetX, int offsetY)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.offsetX = offsetX;
        frameState.offsetY = offsetY;
        update();
    }

    // Reads the display transform stored for one source camera
    bool PreviewWidget::cameraDisplayTransform(const QString& cameraId,
                                               int& offsetX,
                                               int& offsetY,
                                               int& zoomPercent,
                                               bool& flipX,
                                               bool& flipY) const
    {
        QMutexLocker lock(&m_mutex);
        auto it = m_cameraFrames.constFind(cameraId);
        if (it == m_cameraFrames.constEnd())
        {
            offsetX = 0;
            offsetY = 0;
            zoomPercent = 100;
            flipX = false;
            flipY = false;
            return false;
        }

        const CameraFrameState& frameState = it.value();
        offsetX = frameState.offsetX;
        offsetY = frameState.offsetY;
        zoomPercent = frameState.zoomPercent;
        flipX = frameState.flipX;
        flipY = frameState.flipY;
        return true;
    }

    // Sets preview flips for one camera layer source
    void PreviewWidget::setCameraFlip(const QString& cameraId, bool flipX, bool flipY)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.flipX = flipX;
        frameState.flipY = flipY;
        update();
    }

    // Sets per camera preview zoom percentage
    void PreviewWidget::setCameraZoomPercent(const QString& cameraId, int percent)
    {
        QMutexLocker lock(&m_mutex);
        CameraFrameState& frameState = m_cameraFrames[cameraId];
        frameState.zoomPercent = qBound(10, percent, 500);
        update();
    }

    // Refreshes placeholder state and schedules repaint
    void PreviewWidget::updateImageDisplay()
    {
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

        m_placeholderText = QStringLiteral("No image loaded\nClick 'Start Preview' to view the camera feed");
        if (hasDisplayableFrame && selectedLayerKeys().isEmpty())
        {
            m_placeholderText = QStringLiteral("No layer selected");
        }
        update();
    }

    // Updates rolling FPS statistics for one layer
    PreviewWidget::FpsUpdate PreviewWidget::updateFpsOnFrame(const QString& layerKey)
    {
        FpsState& state = m_fpsStates[layerKey];
        if (!state.timer.isValid())
        {
            state.timer.start();
            state.frameCounter = 0;
            state.lastFps = 0.0;
        }

        ++state.frameCounter;
        const qint64 elapsedMs = state.timer.elapsed();
        if (elapsedMs >= 3000)
        {
            state.lastFps = (state.frameCounter * 1000.0) / elapsedMs;
            state.frameCounter = 0;
            state.timer.restart();
            return {state.lastFps, true};
        }
        return {state.lastFps, false};
    }

    // Checks whether a camera state has a raw frame
    bool PreviewWidget::hasRawFrame(const CameraFrameState& frameState) const
    {
        return frameState.rawFrame.isValid();
    }

    // Copies camera frame state under the preview mutex
    QMap<QString, PreviewWidget::CameraFrameState> PreviewWidget::snapshotCameraFrames() const
    {
        QMutexLocker lock(&m_mutex);
        return m_cameraFrames;
    }

    // Builds render metadata for cameras with displayable frames
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

    // Builds a complete render snapshot for painting or hit testing
    void PreviewWidget::buildRenderSnapshot(QMap<QString, CameraFrameState>& cameraFrames,
                                            std::vector<CameraRenderInfo>& cameraRenderInfos,
                                            std::vector<RenderItem>& renderItems) const
    {
        cameraFrames = snapshotCameraFrames();
        cameraRenderInfos = buildCameraRenderInfos(cameraFrames);
        renderItems = buildRenderItems(cameraRenderInfos);
    }

    // Resolves the displayed image rectangle for one layer
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

    // Resolves the displayed geometry for one active layer
    bool PreviewWidget::resolveLayerDisplayGeometry(const QString& layerKey,
                                                    CameraFrameState& frameState,
                                                    bool& processed,
                                                    QRect& itemArea,
                                                    QRect& displayRect,
                                                    QSize& imageSize) const
    {
        QMap<QString, CameraFrameState> cameraFrames;
        std::vector<CameraRenderInfo> cameraRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(cameraFrames, cameraRenderInfos, renderItems);

        for (const RenderItem& item : renderItems)
        {
            if (item.layerKey != layerKey || !item.info || !item.info->frameState)
            {
                continue;
            }

            frameState = *item.info->frameState;
            processed = item.processed;
            itemArea = item.area;
            return resolveDisplayGeometry(frameState, processed, itemArea, displayRect, imageSize);
        }

        return false;
    }

    // Maps one widget position into image coordinates
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

    // Maps one widget rectangle into image rectangle bounds
    bool PreviewWidget::mapWidgetRectToImage(const CameraFrameState& frameState,
                                             bool processed,
                                             const QRect& area,
                                             const QRect& widgetRect,
                                             QRect& imageRect) const
    {
        QRect displayRect;
        QSize imageSize;
        if (!resolveDisplayGeometry(frameState, processed, area, displayRect, imageSize))
        {
            return false;
        }

        const QRect clippedRect = widgetRect.normalized().intersected(displayRect);
        if (clippedRect.isEmpty())
        {
            return false;
        }

        const int imgW = imageSize.width();
        const int imgH = imageSize.height();
        const double scaleX = imgW / static_cast<double>(displayRect.width());
        const double scaleY = imgH / static_cast<double>(displayRect.height());

        int x0 = qFloor((clippedRect.left() - displayRect.left()) * scaleX);
        int y0 = qFloor((clippedRect.top() - displayRect.top()) * scaleY);
        int x1 = qCeil((clippedRect.right() + 1 - displayRect.left()) * scaleX);
        int y1 = qCeil((clippedRect.bottom() + 1 - displayRect.top()) * scaleY);
        x0 = qBound(0, x0, imgW);
        y0 = qBound(0, y0, imgH);
        x1 = qBound(0, x1, imgW);
        y1 = qBound(0, y1, imgH);

        if (frameState.flipX)
        {
            const int flippedX0 = imgW - x1;
            const int flippedX1 = imgW - x0;
            x0 = flippedX0;
            x1 = flippedX1;
        }
        if (frameState.flipY)
        {
            const int flippedY0 = imgH - y1;
            const int flippedY1 = imgH - y0;
            y0 = flippedY0;
            y1 = flippedY1;
        }

        const int width = x1 - x0;
        const int height = y1 - y0;
        if (width <= 0 || height <= 0)
        {
            return false;
        }

        imageRect = QRect(x0, y0, width, height);
        return true;
    }

    // Maps one image coordinate into widget coordinates
    bool PreviewWidget::mapImagePositionToWidget(const CameraFrameState& frameState,
                                                 bool processed,
                                                 const QRect& area,
                                                 const QPoint& imagePos,
                                                 QPoint& widgetPos) const
    {
        QRect displayRect;
        QSize imageSize;
        if (!resolveDisplayGeometry(frameState, processed, area, displayRect, imageSize))
        {
            return false;
        }

        const int imgW = imageSize.width();
        const int imgH = imageSize.height();
        if (imagePos.x() < 0 || imagePos.y() < 0 || imagePos.x() >= imgW || imagePos.y() >= imgH)
        {
            return false;
        }

        int x = imagePos.x();
        int y = imagePos.y();
        if (frameState.flipX)
        {
            x = (imgW - 1) - x;
        }
        if (frameState.flipY)
        {
            y = (imgH - 1) - y;
        }

        const double scaleX = displayRect.width() / static_cast<double>(imgW);
        const double scaleY = displayRect.height() / static_cast<double>(imgH);
        widgetPos = QPoint(qRound(displayRect.x() + (x + 0.5) * scaleX),
                           qRound(displayRect.y() + (y + 0.5) * scaleY));
        return true;
    }

    // Paints the placeholder text when no frame is available
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

    // Draws one render item into its assigned area
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
            drawFrameInRect(item.layerKey,
                            frameState.processedFrame,
                            displayRect,
                            frameState.flipX,
                            frameState.flipY,
                            item.display,
                            item.firstVisibleInArea);
            return;
        }

        if (frameState.rawFrame.isValid())
        {
            drawFrameInRect(item.layerKey,
                            frameState.rawFrame,
                            displayRect,
                            frameState.flipX,
                            frameState.flipY,
                            item.display,
                            item.firstVisibleInArea);
            return;
        }
    }

    // Updates the layer info summary text
    void PreviewWidget::updateLayerInfoDisplay()
    {
        if (m_layerInfos.isEmpty())
        {
            m_layerInfoText = QStringLiteral("No image loaded");
            emit layerInfoTextChanged(m_layerInfoText);
            return;
        }

        QStringList lines;
        QSet<QString> appendedKeys;
        const auto appendInfoLine = [this, &lines, &appendedKeys](const QString& key)
        {
            if (appendedKeys.contains(key))
            {
                return;
            }
            const auto it = m_layerInfos.constFind(key);
            if (it == m_layerInfos.constEnd())
            {
                return;
            }
            appendedKeys.insert(key);
            const LayerInfo& info = it.value();
            const QString cameraId = cameraIdFromLayerKey(key);
            if (m_staticCameraIds.contains(cameraId))
            {
                lines.append(QString("%1: %2×%3")
                             .arg(layerName(key))
                             .arg(info.width)
                             .arg(info.height));
                return;
            }

            lines.append(QString("%1: %2×%3 @ %4 FPS")
                         .arg(layerName(key))
                         .arg(info.width)
                         .arg(info.height)
                         .arg(info.fps, 0, 'f', 1));
        };

        for (const QString& cameraId : m_availableCameraIds)
        {
            appendInfoLine(previewLayerKey(cameraId, false));
            appendInfoLine(previewLayerKey(cameraId, true));
        }
        for (auto it = m_layerInfos.constBegin(); it != m_layerInfos.constEnd(); ++it)
        {
            appendInfoLine(it.key());
        }

        m_layerInfoText = lines.join(QStringLiteral("\n"));
        emit layerInfoTextChanged(m_layerInfoText);
    }

    // Resolves one widget point to the topmost matching preview item
    bool PreviewWidget::resolveInteractionTarget(const QPoint& widgetPos,
                                                 PreviewInteractionTarget& outTarget,
                                                 const QString& cameraId,
                                                 bool rawOnly,
                                                 const QString& layerKey) const
    {
        QMap<QString, CameraFrameState> cameraFrames;
        std::vector<CameraRenderInfo> cameraRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(cameraFrames, cameraRenderInfos, renderItems);
        const QString cameraFilter = cameraId.trimmed();
        const QString layerFilter = layerKey.trimmed();
        if (renderItems.empty())
        {
            return false;
        }

        for (auto it = renderItems.crbegin(); it != renderItems.crend(); ++it)
        {
            const RenderItem& item = *it;
            if (!item.info || !item.info->frameState)
            {
                continue;
            }
            if (!layerFilter.isEmpty() && item.layerKey != layerFilter)
            {
                continue;
            }
            if (!cameraFilter.isEmpty() && item.info->cameraId != cameraFilter)
            {
                continue;
            }
            if (rawOnly && item.processed)
            {
                continue;
            }
            if (item.display.blending != Blending::Opaque && item.display.opacityPercent <= 0)
            {
                continue;
            }
            if (!item.area.contains(widgetPos))
            {
                continue;
            }

            const CameraFrameState& frameState = *item.info->frameState;
            QRect displayRect;
            QSize imageSize;
            if (!resolveDisplayGeometry(frameState, item.processed, item.area, displayRect, imageSize)
                || !displayRect.contains(widgetPos))
            {
                continue;
            }

            QPoint imagePos;
            if (!mapWidgetPositionToImage(frameState, item.processed, item.area, widgetPos, imagePos))
            {
                continue;
            }

            outTarget.layerKey = item.layerKey;
            outTarget.cameraId = item.info->cameraId;
            outTarget.imagePos = imagePos;
            outTarget.imageSize = imageSize;
            outTarget.itemArea = item.area;
            outTarget.displayRect = displayRect;
            outTarget.processed = item.processed;
            return true;
        }

        return false;
    }

    // Resolves one widget point to the topmost matching preview layer
    bool PreviewWidget::interactionTargetAt(const QPoint& widgetPos,
                                            PreviewInteractionTarget& outTarget,
                                            const QString& cameraId,
                                            bool rawOnly) const
    {
        return resolveInteractionTarget(widgetPos, outTarget, cameraId, rawOnly, QString());
    }

    // Reads one raw or processed pixel value
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

    // Returns a sampled line profile from one preview layer
    bool PreviewWidget::lineProfile(const QString& cameraId,
                                    const QPoint& start,
                                    const QPoint& end,
                                    bool processed,
                                    QVector<int>& outValues) const
    {
        const QString trimmedCameraId = cameraId.trimmed();
        if (trimmedCameraId.isEmpty())
        {
            outValues.clear();
            return false;
        }

        ImageFrame frame;
        {
            QMutexLocker lock(&m_mutex);
            const auto it = m_cameraFrames.constFind(trimmedCameraId);
            if (it == m_cameraFrames.constEnd())
            {
                outValues.clear();
                return false;
            }
            frame = processed ? it.value().processedFrame : it.value().rawFrame;
        }

        return sampleLineValues(frame, start, end, outValues);
    }

    // Initializes OpenGL state for preview rendering
    void PreviewWidget::initializeGL()
    {
        initializeOpenGLFunctions();

        glDisable(GL_DEPTH_TEST);
        ensureGlPipeline();
    }

    // Updates the OpenGL viewport after resize
    void PreviewWidget::resizeGL(int, int)
    {
        applyViewportForRect(rect());
    }

    // Computes tiled preview rectangles for visible layers
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

    // Builds the render items that will be drawn this frame
    std::vector<PreviewWidget::RenderItem> PreviewWidget::buildRenderItems(
        const std::vector<CameraRenderInfo>& cameraRenderInfos) const
    {
        std::vector<RenderItem> items;
        if (cameraRenderInfos.empty())
        {
            return items;
        }

        const QRect full(0, 0, width(), height());
        QMap<QString, const CameraRenderInfo*> renderInfoByCameraId;
        for (const auto& info : cameraRenderInfos)
        {
            renderInfoByCameraId.insert(info.cameraId, &info);
        }

        auto addItem = [&](const CameraRenderInfo* info, bool processed, const QRect& area)
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
            const QString key = previewLayerKey(info->cameraId, processed);
            LayerDisplaySettings display = layerDisplaySettings(key);
            if (!display.visible)
            {
                return;
            }
            const bool firstVisibleInArea = std::none_of(items.cbegin(), items.cend(),
                                                         [&area](const RenderItem& item)
                                                         {
                                                             return item.area == area;
                                                         });
            items.push_back({info, processed, key, area, display, firstVisibleInArea});
        };

        std::vector<LayerRenderItem> layers;
        layers.reserve(cameraRenderInfos.size() * 2);

        for (const QString& layerKey : m_layerOrder)
        {
            if (m_layerLayoutMode == LayerLayoutMode::SideBySide && layers.size() >= 4)
            {
                break;
            }
            if (!layerDisplaySettings(layerKey).visible)
            {
                continue;
            }

            const QString cameraId = cameraIdFromLayerKey(layerKey);
            const auto it = renderInfoByCameraId.constFind(cameraId);
            if (it == renderInfoByCameraId.constEnd())
            {
                continue;
            }

            const CameraRenderInfo* info = it.value();
            const bool processed = isProcessedLayerKey(layerKey);
            if (processed && !info->hasProcessedFrame)
            {
                continue;
            }
            if (!processed && !info->hasRawFrame)
            {
                continue;
            }

            layers.push_back({info, processed});
        }

        if (layers.empty())
        {
            return items;
        }

        if (m_layerLayoutMode == LayerLayoutMode::SideBySide)
        {
            const auto areas = computeLayout(static_cast<int>(layers.size()));
            const size_t limit = std::min<size_t>(layers.size(), areas.size());
            for (size_t i = 0; i < limit; ++i)
            {
                addItem(layers[i].info, layers[i].processed, areas[i]);
            }
            return items;
        }

        for (const LayerRenderItem& layer : layers)
        {
            addItem(layer.info, layer.processed, full);
        }
        return items;
    }

    // Draws the current preview frame with OpenGL
    void PreviewWidget::paintGL()
    {
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

                QPoint lineStart = m_lineStart;
                QPoint lineEnd = m_lineEnd;
                if (m_lineVisible)
                {
                    for (const auto& item : renderItems)
                    {
                        if (item.layerKey != m_lineTargetLayerKey || !item.info || !item.info->frameState)
                        {
                            continue;
                        }
                        QPoint startWidget;
                        QPoint endWidget;
                        if (mapImagePositionToWidget(*item.info->frameState,
                                                     item.processed,
                                                     item.area,
                                                     m_lineStartImage,
                                                     startWidget)
                            && mapImagePositionToWidget(*item.info->frameState,
                                                        item.processed,
                                                        item.area,
                                                        m_lineEndImage,
                                                        endWidget))
                        {
                            lineStart = startWidget;
                            lineEnd = endWidget;
                        }
                        break;
                    }
                }
                p.drawLine(lineStart, lineEnd);
            }
            return;
        }

        static bool warned = false;
        if (!warned)
        {
            qWarning() << "PreviewWidget: GPU rendering unavailable; CPU rendering is not enabled";
            warned = true;
        }
        paintPlaceholder(QStringLiteral(
            "Preview unavailable\nOpenGL initialization failed on this system"));
        return;
    }

    // Creates shaders buffers and uniforms for preview rendering
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
        uniform float uGamma;
        uniform int uColormap;
        vec3 applyColormap(float t, int map){
            if (map == 1) return vec3(0.0, t, 0.0);
            if (map == 2) return vec3(t, 0.0, t);
            if (map == 3) return vec3(0.0, t, t);
            if (map == 4) return vec3(t, 0.0, 0.0);
            if (map == 5) return vec3(0.0, 0.0, t);
            if (map == 6) return vec3(t, t, 0.0);
            if (map == 7) {
                float r = smoothstep(0.0, 0.45, t);
                float g = smoothstep(0.25, 0.80, t);
                float b = smoothstep(0.70, 1.0, t);
                return vec3(r, g, b);
            }
            return vec3(t, t, t);
        }
        void main(){
            vec4 s = texture(uTex, vUV);
            float t0 = s.r * uTexNormScale;
            float t = clamp((t0 - uMinNorm) / max(uMaxNorm - uMinNorm, 1e-6), 0.0, 1.0);
            t = pow(t, 1.0 / max(uGamma, 1e-3));
            FragColor = vec4(applyColormap(t, uColormap), uAlpha);
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
        m_uGamma = m_prog.uniformLocation("uGamma");
        m_uColormap = m_prog.uniformLocation("uColormap");
        m_uUvScale = m_prog.uniformLocation("uUvScale");
        m_uUvOffset = m_prog.uniformLocation("uUvOffset");
        m_prog.release();
        m_vao.release();
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_glInited = true;
    }

    // Sets texture coordinate transform for flips
    void PreviewWidget::setUvTransform(bool flipX, bool flipY)
    {
        const float sx = flipX ? -1.0f : 1.0f;
        const float sy = flipY ? 1.0f : -1.0f;
        const float ox = flipX ? 1.0f : 0.0f;
        const float oy = flipY ? 0.0f : 1.0f;

        if (m_uUvScale >= 0) m_prog.setUniformValue(m_uUvScale, sx, sy);
        if (m_uUvOffset >= 0) m_prog.setUniformValue(m_uUvOffset, ox, oy);
    }

    // Uploads and draws one image frame into a target rectangle
    void PreviewWidget::drawFrameInRect(const QString& textureKey,
                                        const ImageFrame& frame,
                                        const QRect& r,
                                        bool flipX,
                                        bool flipY,
                                        const LayerDisplaySettings& display,
                                        bool firstVisibleInArea)
    {
        if (!frame.isValid() || r.width() <= 0 || r.height() <= 0) return;

        ensureGlPipeline();

        GLenum uploadType = GL_UNSIGNED_BYTE;
        GLint internalFormat = GL_R8;
        int unpackAlign = 1;

        if (frame.isMono16())
        {
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
        if (frame.stride > 0)
        {
            const int rowPixels = frame.stride / bytesPerPixel;
            if (rowPixels != frame.width)
            {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, rowPixels);
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        frame.width, frame.height,
                        GL_RED, uploadType, frame.bytes.constData());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

        m_prog.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);
        m_prog.setUniformValue(m_uTex, 0);
        const int levelDomain = qMax(1, display.levelDomainMax);
        m_prog.setUniformValue(m_uMinNorm, static_cast<float>(display.levelMin) / static_cast<float>(levelDomain));
        m_prog.setUniformValue(m_uMaxNorm, static_cast<float>(display.levelMax) / static_cast<float>(levelDomain));
        const int bitDepth = qBound(8, frame.bitsPerSample, 16);
        const float bitMax = static_cast<float>((1u << bitDepth) - 1u);
        const float sampleMax = (internalFormat == GL_R16) ? 65535.0f : 255.0f;
        const float texNormScale = sampleMax / bitMax;
        m_prog.setUniformValue(m_uTexNormScale, texNormScale);
        const float opacity = display.blending == Blending::Opaque
                                  ? 1.0f
                                  : qBound(0.0f, static_cast<float>(display.opacityPercent) / 100.0f, 1.0f);
        m_prog.setUniformValue(m_uAlpha, opacity);
        m_prog.setUniformValue(m_uGamma, static_cast<float>(std::clamp(display.gamma, 0.2, 2.0)));
        m_prog.setUniformValue(m_uColormap, static_cast<int>(display.colormap));
        setUvTransform(flipX, flipY);

        if (display.blending == Blending::Opaque)
        {
            glDisable(GL_BLEND);
        }
        else
        {
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);

            if (firstVisibleInArea)
            {
                if (display.blending == Blending::Minimum)
                {
                    glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);
                }
                else if (display.blending == Blending::Additive)
                {
                    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ZERO, GL_ONE, GL_ONE);
                }
            }
            else if (display.blending == Blending::Additive)
            {
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ONE);
            }
            else if (display.blending == Blending::Minimum)
            {
                glBlendEquation(GL_MIN);
                glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
            }
            else if (display.blending == Blending::Multiplicative)
            {
                glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ONE);
            }
        }

        applyViewportForRect(r);

        m_vao.bind();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_vao.release();
        glBlendEquation(GL_FUNC_ADD);
        glDisable(GL_BLEND);
        m_prog.release();
    }


    // Computes the target rectangle for an image inside an area
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

    // Applies an OpenGL viewport for a logical widget rectangle
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

    // Returns an existing texture or creates one with matching shape
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
        const GLenum uploadType = (internalFormat == GL_R16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RED, uploadType, nullptr);

        CachedTexture cached;
        cached.texId = texId;
        cached.width = width;
        cached.height = height;
        cached.internalFormat = internalFormat;
        m_textureCache[key] = cached;

        return texId;
    }

    // Deletes all cached OpenGL textures
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

    // Starts ROI drawing for one camera
    void PreviewWidget::startROIDrawing(const QString& cameraId)
    {
        if (m_lineDrawingMode)
        {
            cancelLineDrawing();
        }
        m_roiDrawingMode = true;
        m_roiTargetCameraId = cameraId;
        m_roiTargetLayerKey.clear();
        m_roiDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    // Cancels active ROI drawing
    void PreviewWidget::cancelROIDrawing()
    {
        if (!m_roiDrawingMode)
        {
            return;
        }
        m_roiDrawingMode = false;
        m_roiDragging = false;
        m_roiTargetCameraId.clear();
        m_roiTargetLayerKey.clear();
        unsetCursor();
        update();
    }

    // Starts line drawing for one exact preview layer
    void PreviewWidget::startLineDrawingForLayer(const QString& layerKey)
    {
        if (m_roiDrawingMode)
        {
            cancelROIDrawing();
        }
        m_lineDrawingMode = true;
        m_lineTargetLayerKey = layerKey.trimmed();
        m_lineTargetCameraId = cameraIdFromLayerKey(m_lineTargetLayerKey);
        m_lineDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    // Cancels active line drawing
    void PreviewWidget::cancelLineDrawing()
    {
        if (!m_lineDrawingMode)
        {
            return;
        }
        m_lineDrawingMode = false;
        m_lineDragging = false;
        m_lineTargetCameraId.clear();
        m_lineTargetLayerKey.clear();
        unsetCursor();
        update();
    }

    // Clears the current line overlay
    void PreviewWidget::clearLine()
    {
        m_lineVisible = false;
        m_lineDragging = false;
        m_lineDrawingMode = false;
        m_lineProcessed = false;
        m_lineTargetCameraId.clear();
        m_lineTargetLayerKey.clear();
        unsetCursor();
        update();
    }

    // Starts ROI or line interaction from a mouse press
    void PreviewWidget::mousePressEvent(QMouseEvent* event)
    {
        emit mousePositionChanged(event->pos());
        if (m_lineDrawingMode && event->button() == Qt::LeftButton)
        {
            QString cameraId = m_lineTargetCameraId;
            PreviewInteractionTarget target;
            const bool ok = resolveInteractionTarget(event->pos(),
                                                     target,
                                                     cameraId,
                                                     false,
                                                     m_lineTargetLayerKey);
            if (!ok)
            {
                return;
            }

            m_lineTargetCameraId = target.cameraId;
            m_lineTargetLayerKey = target.layerKey;
            m_lineStart = event->pos();
            m_lineEnd = event->pos();
            m_lineStartImage = target.imagePos;
            m_lineEndImage = target.imagePos;
            m_lineProcessed = target.processed;
            m_lineDragging = true;
            update();
            return;
        }

        if (m_roiDrawingMode && event->button() == Qt::LeftButton)
        {
            PreviewInteractionTarget target;
            if (!interactionTargetAt(event->pos(), target, m_roiTargetCameraId, true))
            {
                return;
            }
            if (m_staticCameraIds.contains(target.cameraId))
            {
                return;
            }

            m_roiTargetCameraId = target.cameraId;
            m_roiTargetLayerKey = target.layerKey;
            m_roiStart = event->pos();
            m_roiEnd = event->pos();
            m_roiDragging = true;
            update();
            return;
        }

        QOpenGLWidget::mousePressEvent(event);
    }

    // Updates active ROI or line interaction during mouse move
    void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
    {
        emit mousePositionChanged(event->pos());
        if (m_lineDrawingMode && m_lineDragging)
        {
            m_lineEnd = event->pos();
            PreviewInteractionTarget target;
            if (resolveInteractionTarget(event->pos(),
                                         target,
                                         m_lineTargetCameraId,
                                         false,
                                         m_lineTargetLayerKey))
            {
                m_lineEndImage = target.imagePos;
            }
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

    // Finishes ROI or line drawing in image coordinates
    void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
    {
        emit mousePositionChanged(event->pos());
        if (m_lineDrawingMode && event->button() == Qt::LeftButton && m_lineDragging)
        {
            m_lineDragging = false;
            m_lineEnd = event->pos();

            PreviewInteractionTarget startTarget;
            const bool okStart = resolveInteractionTarget(m_lineStart,
                                                          startTarget,
                                                          m_lineTargetCameraId,
                                                          false,
                                                          m_lineTargetLayerKey);
            CameraFrameState frameState;
            bool processed = false;
            QRect itemArea;
            QRect displayRect;
            QSize imageSize;
            QPoint clippedStart;
            QPoint clippedEnd;
            QPoint imgStart;
            QPoint imgEnd;
            if (!okStart
                || !resolveLayerDisplayGeometry(startTarget.layerKey,
                                                frameState,
                                                processed,
                                                itemArea,
                                                displayRect,
                                                imageSize)
                || !clipLineToRect(m_lineStart, m_lineEnd, displayRect, clippedStart, clippedEnd)
                || !mapWidgetPositionToImage(frameState, processed, itemArea, clippedStart, imgStart)
                || !mapWidgetPositionToImage(frameState, processed, itemArea, clippedEnd, imgEnd))
            {
                cancelLineDrawing();
                return;
            }

            m_lineTargetLayerKey = startTarget.layerKey;
            m_lineTargetCameraId = startTarget.cameraId;
            m_lineStart = clippedStart;
            m_lineEnd = clippedEnd;
            m_lineStartImage = imgStart;
            m_lineEndImage = imgEnd;
            m_lineProcessed = processed;
            m_lineVisible = true;
            m_lineDrawingMode = false;
            m_lineDragging = false;
            unsetCursor();
            emit lineDrawn(m_lineTargetLayerKey,
                           m_lineTargetCameraId,
                           m_lineStartImage.x(), m_lineStartImage.y(),
                           m_lineEndImage.x(), m_lineEndImage.y(),
                           m_lineProcessed);
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

            PreviewInteractionTarget startTarget;
            const bool okStart = resolveInteractionTarget(m_roiStart,
                                                          startTarget,
                                                          m_roiTargetCameraId,
                                                          true,
                                                          m_roiTargetLayerKey);
            CameraFrameState frameState;
            bool processed = false;
            QRect itemArea;
            QRect displayRect;
            QSize imageSize;
            if (!okStart
                || !resolveLayerDisplayGeometry(startTarget.layerKey,
                                                frameState,
                                                processed,
                                                itemArea,
                                                displayRect,
                                                imageSize))
            {
                cancelROIDrawing();
                return;
            }

            const QRect clippedRect = rect.intersected(displayRect);
            if (clippedRect.width() < 10 || clippedRect.height() < 10)
            {
                cancelROIDrawing();
                return;
            }

            QRect imageRect;
            if (!mapWidgetRectToImage(frameState, false, itemArea, clippedRect, imageRect))
            {
                cancelROIDrawing();
                return;
            }

            const int imgX = imageRect.x();
            const int imgY = imageRect.y();
            const int imgW = imageRect.width();
            const int imgH = imageRect.height();
            if (imgW > 0 && imgH > 0)
            {
                const ImageFrame& rawFrame = frameState.rawFrame;
                const int sourceRoiX = rawFrame.hasSourceRoi() ? rawFrame.sourceRoiX : 0;
                const int sourceRoiY = rawFrame.hasSourceRoi() ? rawFrame.sourceRoiY : 0;
                emit roiDrawn(startTarget.cameraId,
                              imgX,
                              imgY,
                              imgW,
                              imgH,
                              sourceRoiX,
                              sourceRoiY);
            }

            cancelROIDrawing();
            return;
        }

        QOpenGLWidget::mouseReleaseEvent(event);
    }

    // Handles control wheel zoom around the cursor anchor
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
        PreviewInteractionTarget target;
        QPointF relativePos;
        const bool hasAnchor = interactionTargetAt(event->position().toPoint(), target)
            && !target.cameraId.isEmpty()
            && target.displayRect.width() > 0
            && target.displayRect.height() > 0;
        if (hasAnchor)
        {
            relativePos = QPointF(
                static_cast<double>(event->position().x() - target.displayRect.x())
                    / static_cast<double>(target.displayRect.width()),
                static_cast<double>(event->position().y() - target.displayRect.y())
                    / static_cast<double>(target.displayRect.height()));
        }
        if (m_fitToWindow)
        {
            setFitToWindow(false);
        }
        setZoomPercent(m_zoomPercent + steps * 10);

        if (hasAnchor)
        {
            CameraFrameState frameState;
            bool hasFrameState = false;
            {
                QMutexLocker lock(&m_mutex);
                const auto it = m_cameraFrames.constFind(target.cameraId);
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
                if (!resolveDisplayGeometry(frameState, target.processed, target.itemArea, newRect, imageSize))
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

    // Cancels active drawing modes from keyboard input
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
