#include "PreviewWidget.h"

#include "scopeone/ImageSceneModel.h"
#include "scopeone/ScopeOneCore.h"
#include <QDebug>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QMouseEvent>
#include <QPalette>
#include <QKeyEvent>
#include <QLineF>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QtGlobal>
#include <QtMath>
#include <algorithm>
#include <vector>

namespace scopeone::ui
{
    using scopeone::core::ImageFrame;
    using scopeone::core::ScopeOneCore;

    namespace
    {
        constexpr int kColormapSize = 256;

        QByteArray loadImageJLut(const QString& name)
        {
            QFile file(QStringLiteral(":/luts/%1.lut").arg(name));
            if (!file.open(QIODevice::ReadOnly))
            {
                return {};
            }

            const QByteArray planar = file.readAll();
            if (planar.size() != kColormapSize * 3)
            {
                return {};
            }

            QByteArray interleaved;
            interleaved.reserve(planar.size());
            for (int index = 0; index < kColormapSize; ++index)
            {
                interleaved.append(planar[index]);
                interleaved.append(planar[kColormapSize + index]);
                interleaved.append(planar[2 * kColormapSize + index]);
            }
            return interleaved;
        }

        QByteArray buildColormapAtlas(const QStringList& names)
        {
            QByteArray atlas;
            atlas.reserve(names.size() * kColormapSize * 3);
            for (const QString& name : names)
            {
                const QByteArray data = loadImageJLut(name);
                if (data.size() != kColormapSize * 3)
                {
                    qCritical() << "PreviewWidget: failed to load colormap" << name;
                    return {};
                }
                atlas.append(data);
            }
            return atlas;
        }

        int colormapIndex(const QString& name)
        {
            const QStringList names = ImageSceneModel::supportedColormaps();
            for (int index = 0; index < names.size(); ++index)
            {
                if (names[index].compare(name, Qt::CaseInsensitive) == 0)
                {
                    return index;
                }
            }
            return 0;
        }

        // Builds a stable layer key for raw or processed preview
        QString previewLayerKey(const QString& cameraId, bool processed)
        {
            return processed
                       ? ScopeOneCore::processedLayerKey(cameraId)
                       : ScopeOneCore::rawLayerKey(cameraId);
        }

        // Normalizes one source id before it becomes a layer key
        QString normalizedSourceId(const QString& sourceId)
        {
            return sourceId.trimmed();
        }

        // Normalize source ids before they become layer keys
        QStringList normalizedSourceIds(const QStringList& sourceIds)
        {
            QStringList normalizedIds;
            normalizedIds.reserve(sourceIds.size());
            for (const QString& sourceId : sourceIds)
            {
                const QString trimmedSourceId = normalizedSourceId(sourceId);
                if (!trimmedSourceId.isEmpty() && !normalizedIds.contains(trimmedSourceId))
                {
                    normalizedIds.append(trimmedSourceId);
                }
            }
            return normalizedIds;
        }

        // Builds all valid layer keys for available cameras
        QSet<QString> validPreviewLayerKeys(const QStringList& cameraIds)
        {
            QSet<QString> keys;
            for (const QString& cameraId : normalizedSourceIds(cameraIds))
            {
                keys.insert(previewLayerKey(cameraId, false));
                keys.insert(previewLayerKey(cameraId, true));
            }
            return keys;
        }

        // Clips one line to a rectangle
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

        double pointSegmentDistance(const QPoint& point, const QPoint& start, const QPoint& end)
        {
            const double dx = end.x() - start.x();
            const double dy = end.y() - start.y();
            if (qFuzzyIsNull(dx) && qFuzzyIsNull(dy))
            {
                return QLineF(QPointF(point), QPointF(start)).length();
            }

            const double t = qBound(0.0,
                                    ((point.x() - start.x()) * dx + (point.y() - start.y()) * dy)
                                        / (dx * dx + dy * dy),
                                    1.0);
            const QPointF projected(start.x() + t * dx, start.y() + t * dy);
            return QLineF(QPointF(point), projected).length();
        }
    } // namespace

    // Creates the OpenGL preview widget
    PreviewWidget::PreviewWidget(ImageSceneModel* sceneModel, QWidget* parent)
        : QOpenGLWidget(parent),
          m_sceneModel(sceneModel)
    {
        connect(m_sceneModel, &ImageSceneModel::layersChanged,
                this, [this]()
                {
                    emit availableLayerKeysChanged(availableLayerKeys());
                    emit visibleLayerKeysChanged(visibleLayerKeys());
                    updateLayerInfoDisplay();
                    updateImageDisplay();
                });
        connect(m_sceneModel, &ImageSceneModel::layerDisplayChanged,
                this, [this](const QString&)
                {
                    emit visibleLayerKeysChanged(visibleLayerKeys());
                    updateImageDisplay();
                });
        connect(m_sceneModel, &ImageSceneModel::sourceDisplayTransformChanged,
                this, [this](const QString&)
                {
                    update();
                });
        connect(m_sceneModel, &ImageSceneModel::markupsChanged,
                this, [this]()
                {
                    update();
                });

        setMinimumSize(256, 256);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);

        m_placeholderLabel = new QLabel(m_placeholderText, this);
        m_placeholderLabel->setAlignment(Qt::AlignCenter);
        m_placeholderLabel->setWordWrap(true);
        m_placeholderLabel->setTextFormat(Qt::PlainText);
        m_placeholderLabel->setForegroundRole(QPalette::PlaceholderText);
        m_placeholderLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
        QFont placeholderFont = font();
        if (placeholderFont.pointSizeF() > 0.0)
        {
            placeholderFont.setPointSizeF(placeholderFont.pointSizeF() + 1.0);
        }
        m_placeholderLabel->setFont(placeholderFont);
        m_placeholderLabel->setGeometry(rect());

        m_fpsUpdateTimer.setInterval(3000);
        connect(&m_fpsUpdateTimer, &QTimer::timeout, this, &PreviewWidget::updateFrameRates);
        m_fpsUpdateTimer.start();
    }

    // Releases cached OpenGL textures
    PreviewWidget::~PreviewWidget()
    {
        cleanupTextureCache();
    }

    // Stores one frame in the preview sink cache
    bool PreviewWidget::storeSourceFrame(const QString& sourceId,
                                         FrameRole role,
                                         const ImageFrame& frame,
                                         bool* replacedFrame)
    {
        const QString normalizedId = normalizedSourceId(sourceId);
        if (normalizedId.isEmpty())
        {
            if (replacedFrame)
            {
                *replacedFrame = false;
            }
            return false;
        }

        const QString frameSourceId = normalizedSourceId(frame.cameraId);
        if (!frame.isValid() || frameSourceId != normalizedId)
        {
            if (replacedFrame)
            {
                *replacedFrame = false;
            }
            return false;
        }

        QMutexLocker lock(&m_mutex);
        FrameSourceState& frameState = m_frameSources[normalizedId];
        ++m_nextFrameRevision;
        bool hadFrame = false;
        if (role == FrameRole::Processed)
        {
            hadFrame = frameState.processedFrame.isValid();
            frameState.processedFrame = frame;
            frameState.processedRevision = m_nextFrameRevision;
        }
        else
        {
            hadFrame = frameState.rawFrame.isValid();
            frameState.rawFrame = frame;
            frameState.rawRevision = m_nextFrameRevision;
        }
        if (replacedFrame)
        {
            *replacedFrame = hadFrame;
        }
        return true;
    }

    // Initializes display statistics for one layer
    void PreviewWidget::initializeLayerInfo(const QString& layerKey)
    {
        if (!m_layerFps.contains(layerKey))
        {
            m_layerFps.insert(layerKey, 0.0);
            updateLayerInfoDisplay();
        }
    }

    // Counts one frame in a live layer throughput window
    void PreviewWidget::updateLayerFps(const QString& layerKey, quint64 frameCount)
    {
        FpsState& state = m_fpsStates[layerKey];
        if (!state.intervalTimer.isValid())
        {
            state.intervalTimer.start();
        }
        state.framesSinceUpdate += frameCount;
    }

    // Stores one graph processed frame and updates layer statistics
    void PreviewWidget::setGraphProcessedFrame(const ImageFrame& frame)
    {
        const QString sourceId = normalizedSourceId(frame.cameraId);
        if (sourceId.isEmpty() || !frame.isValid())
        {
            return;
        }
        bool hadProcessedFrame = false;
        const bool stored = storeSourceFrame(
            sourceId, FrameRole::Processed, frame, &hadProcessedFrame);
        const QString layerKey = previewLayerKey(sourceId, true);
        initializeLayerInfo(layerKey);

        const bool registeredCamera = stored && registerAvailableCamera(sourceId);
        if (stored && !hadProcessedFrame)
        {
            emit availableLayerKeysChanged(availableLayerKeys());
        }

        if (registeredCamera)
        {
            return;
        }
        updateImageDisplay();
    }

    // Stores one graph raw frame and updates layer statistics
    void PreviewWidget::setGraphRawFrame(const ImageFrame& frame)
    {
        const QString sourceId = normalizedSourceId(frame.cameraId);
        if (sourceId.isEmpty() || !frame.isValid())
        {
            return;
        }
        const bool stored = storeSourceFrame(sourceId, FrameRole::Raw, frame);
        const QString layerKey = previewLayerKey(sourceId, false);
        initializeLayerInfo(layerKey);
        if (stored && registerAvailableCamera(sourceId))
        {
            return;
        }
        update();
    }

    // Tracks completed processing throughput from aggregated frame counts
    void PreviewWidget::trackProcessedFrameRate(const QString& cameraId, quint64 frameCount)
    {
        const QString sourceId = normalizedSourceId(cameraId);
        if (!sourceId.isEmpty() && frameCount > 0)
        {
            updateLayerFps(previewLayerKey(sourceId, true), frameCount);
        }
    }

    // Tracks acquired raw throughput from backend frame counts
    void PreviewWidget::trackRawFrameRate(const QString& cameraId, quint64 frameCount)
    {
        const QString sourceId = normalizedSourceId(cameraId);
        if (!sourceId.isEmpty() && frameCount > 0)
        {
            updateLayerFps(previewLayerKey(sourceId, false), frameCount);
        }
    }

    // Clears live throughput windows when preview stops
    void PreviewWidget::resetLiveFrameRates()
    {
        bool changed = false;
        for (auto it = m_layerFps.begin(); it != m_layerFps.end(); ++it)
        {
            if ((ScopeOneCore::isRawLayerKey(it.key()) || ScopeOneCore::isProcessedLayerKey(it.key()))
                && !qFuzzyIsNull(it.value()))
            {
                it.value() = 0.0;
                changed = true;
            }
        }
        m_fpsStates.clear();
        if (changed)
        {
            updateLayerInfoDisplay();
        }
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

    // Replaces the available camera list and keeps valid visible layers
    void PreviewWidget::setAvailableCameraIds(const QStringList& cameraIds)
    {
        m_availableCameraIds = normalizedSourceIds(cameraIds);
        emit availableCameraIdsChanged(m_availableCameraIds);
        emit visibleLayerKeysChanged(visibleLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
    }

    // Adds a graph static image frame as a preview layer
    QString PreviewWidget::setGraphStaticLayerFrame(const QString& layerId,
                                                    const ImageFrame& frame)
    {
        const QString normalizedId = layerId.trimmed();
        if (normalizedId.isEmpty() || !frame.isValid())
        {
            return {};
        }

        const QString layerKey = ScopeOneCore::staticLayerKey(normalizedId);
        const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
        if (normalizedSourceId(frame.cameraId) != sourceId)
        {
            return {};
        }
        const bool hadStaticLayer = m_staticSourceIds.contains(sourceId);
        m_staticSourceIds.insert(sourceId);

        if (!storeSourceFrame(sourceId, FrameRole::Raw, frame))
        {
            return {};
        }
        initializeLayerInfo(layerKey);

        updateLayerInfoDisplay();
        if (!hadStaticLayer)
        {
            emit availableLayerKeysChanged(availableLayerKeys());
            emit visibleLayerKeysChanged(visibleLayerKeys());
        }
        updateImageDisplay();
        return layerKey;
    }

    // Adds or updates one realtime tool layer
    QString PreviewWidget::setGraphToolLayerFrame(const QString& layerId,
                                                   const ImageFrame& frame)
    {
        const QString normalizedId = layerId.trimmed();
        if (normalizedId.isEmpty() || !frame.isValid())
        {
            return {};
        }
        const QString layerKey = ScopeOneCore::toolLayerKey(normalizedId);
        const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
        if (normalizedSourceId(frame.cameraId) != sourceId)
        {
            return {};
        }
        const bool newLayer = !m_toolSourceIds.contains(sourceId);
        m_toolSourceIds.insert(sourceId);
        if (!storeSourceFrame(sourceId, FrameRole::Raw, frame))
        {
            return {};
        }
        initializeLayerInfo(layerKey);
        updateLayerFps(layerKey, 1);
        updateLayerInfoDisplay();
        updateImageDisplay();
        if (newLayer)
        {
            emit availableLayerKeysChanged(availableLayerKeys());
            emit visibleLayerKeysChanged(visibleLayerKeys());
        }
        return layerKey;
    }

    // Removes one static image layer from the preview
    bool PreviewWidget::removeStaticLayer(const QString& layerKey)
    {
        const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
        if (layerKey != ScopeOneCore::staticLayerKey(sourceId) || !m_staticSourceIds.contains(sourceId))
        {
            return false;
        }

        removeStaticLayerData(sourceId);
        updateLayerInfoDisplay();
        emit visibleLayerKeysChanged(visibleLayerKeys());
        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
        return true;
    }

    // Removes all static image layers from the preview
    void PreviewWidget::clearStaticLayers()
    {
        if (m_staticSourceIds.isEmpty())
        {
            return;
        }

        QStringList sourceIds;
        sourceIds.reserve(m_staticSourceIds.size());
        for (const QString& sourceId : m_staticSourceIds)
        {
            sourceIds.append(sourceId);
        }
        for (const QString& sourceId : sourceIds)
        {
            removeStaticLayerData(sourceId);
        }

        updateLayerInfoDisplay();
        emit visibleLayerKeysChanged(visibleLayerKeys());
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

        QMap<QString, FrameSourceState> frameSources = snapshotFrameSources();
        for (const QString& cameraId : m_availableCameraIds)
        {
            const QString rawKey = previewLayerKey(cameraId, false);
            availableKeys.append(rawKey);
            availableSet.insert(rawKey);

            const auto it = frameSources.constFind(cameraId);
            if (it != frameSources.constEnd() && it.value().processedFrame.isValid())
            {
                const QString processedKey = previewLayerKey(cameraId, true);
                availableKeys.append(processedKey);
                availableSet.insert(processedKey);
            }
        }
        for (const QString& sourceId : m_staticSourceIds)
        {
            const QString layerKey = ScopeOneCore::staticLayerKey(sourceId);
            scopeone::core::DocumentLayer layer;
            if (!m_sceneModel->findLayer(layerKey, layer))
            {
                continue;
            }
            availableKeys.append(layerKey);
            availableSet.insert(layerKey);
        }
        for (const QString& sourceId : m_toolSourceIds)
        {
            const QString layerKey = ScopeOneCore::toolLayerKey(sourceId);
            scopeone::core::DocumentLayer layer;
            if (!m_sceneModel->findLayer(layerKey, layer))
            {
                continue;
            }
            availableKeys.append(layerKey);
            availableSet.insert(layerKey);
        }

        QStringList layerKeys;
        layerKeys.reserve(availableKeys.size());
        for (const QString& layerKey : m_sceneModel->layerIds())
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

    QStringList PreviewWidget::visibleLayerKeys() const
    {
        QStringList visibleLayers;
        const QSet<QString> validKeys = validLayerKeys();
        for (const QString& layerKey : m_sceneModel->layerIds())
        {
            scopeone::core::DocumentLayer layer;
            if (validKeys.contains(layerKey)
                && m_sceneModel->findLayer(layerKey, layer)
                && layer.display.visible)
            {
                visibleLayers.append(layerKey);
            }
        }
        return visibleLayers;
    }

    QStringList PreviewWidget::supportedLayerColormaps() const
    {
        return ImageSceneModel::supportedColormaps();
    }

    QStringList PreviewWidget::supportedLayerBlendingModes() const
    {
        return ImageSceneModel::supportedBlendingModes();
    }

    QString PreviewWidget::layerName(const QString& layerKey) const
    {
        scopeone::core::DocumentLayer layer;
        if (m_sceneModel->findLayer(layerKey, layer) && !layer.name.isEmpty())
        {
            return layer.name;
        }

        const QString cameraId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
        return ScopeOneCore::isProcessedLayerKey(layerKey)
                   ? QStringLiteral("%1 Processed").arg(cameraId)
                   : QStringLiteral("%1 Raw").arg(cameraId);
    }

    QString PreviewWidget::layerInfoText(const QString& layerKey) const
    {
        if (!m_layerFps.contains(layerKey))
        {
            return {};
        }

        scopeone::core::DocumentLayer layer;
        if (!m_sceneModel->findLayer(layerKey, layer) || layer.width <= 0 || layer.height <= 0)
        {
            return {};
        }

        const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
        if (m_staticSourceIds.contains(sourceId))
        {
            return QStringLiteral("%1x%2")
                .arg(layer.width)
                .arg(layer.height);
        }

        return QStringLiteral("%1x%2 @ %3 FPS")
            .arg(layer.width)
            .arg(layer.height)
            .arg(m_layerFps.value(layerKey), 0, 'f', 1);
    }

    QString PreviewWidget::layerInfoSummaryText() const
    {
        return m_layerInfoText;
    }

    // Clears preview state for one source
    void PreviewWidget::clearSourceFrames(const QString& sourceId)
    {
        const QString normalizedId = normalizedSourceId(sourceId);
        m_layerFps.remove(previewLayerKey(normalizedId, false));
        m_layerFps.remove(previewLayerKey(normalizedId, true));
        m_fpsStates.remove(previewLayerKey(normalizedId, false));
        m_fpsStates.remove(previewLayerKey(normalizedId, true));
        updateLayerInfoDisplay();

        QMutexLocker lock(&m_mutex);
        if (m_frameSources.contains(normalizedId))
        {
            m_frameSources.remove(normalizedId);
            update();
        }
        lock.unlock();

        makeCurrent();
        const QString rawKey = previewLayerKey(normalizedId, false);
        const QString procKey = previewLayerKey(normalizedId, true);
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

    // Clears all processed preview frame caches
    void PreviewWidget::clearProcessedFrames()
    {
        for (const QString& cameraId : m_availableCameraIds)
        {
            m_layerFps.remove(previewLayerKey(cameraId, true));
            m_fpsStates.remove(previewLayerKey(cameraId, true));
        }
        updateLayerInfoDisplay();

        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_frameSources.begin(); it != m_frameSources.end(); ++it)
            {
                it->processedFrame = ImageFrame();
                it->processedRevision = 0;
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

        emit availableLayerKeysChanged(availableLayerKeys());
        updateImageDisplay();
    }

    // Registers a camera when its first frame arrives
    bool PreviewWidget::registerAvailableCamera(const QString& cameraId)
    {
        const QString sourceId = normalizedSourceId(cameraId);
        if (sourceId.isEmpty() || m_availableCameraIds.contains(sourceId))
        {
            return false;
        }

        m_availableCameraIds.append(sourceId);
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
        settings.colormapIndex = colormapIndex(processed ? QStringLiteral("Green")
                                                         : QStringLiteral("Gray"));
        settings.blending = Blending::Translucent;
        settings.levelMin = 0;
        settings.levelMax = 255;
        settings.levelDomainMax = 255;
        return settings;
    }

    // Returns display settings for one layer id
    PreviewWidget::LayerDisplaySettings PreviewWidget::layerDisplaySettings(const QString& layerKey) const
    {
        scopeone::core::DocumentLayer layer;
        if (m_sceneModel->findLayer(layerKey, layer))
        {
            LayerDisplaySettings settings;
            settings.visible = layer.display.visible;
            settings.opacityPercent = layer.display.opacityPercent;
            settings.gamma = layer.display.gamma;
            settings.colormapIndex = colormapIndex(layer.display.colormap);
            settings.blending = blendingFromName(layer.display.blending);
            settings.levelMin = layer.display.levelMin;
            settings.levelMax = layer.display.levelMax;
            settings.levelDomainMax = layer.display.levelDomainMax;
            return settings;
        }
        return defaultLayerDisplaySettings(ScopeOneCore::isProcessedLayerKey(layerKey));
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

    // Removes stored state for one static image source
    void PreviewWidget::removeStaticLayerData(const QString& sourceId)
    {
        const QString layerKey = ScopeOneCore::staticLayerKey(sourceId);
        m_staticSourceIds.remove(sourceId);
        m_layerFps.remove(layerKey);
        m_fpsStates.remove(layerKey);

        {
            QMutexLocker lock(&m_mutex);
            m_frameSources.remove(sourceId);
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

    QSet<QString> PreviewWidget::validLayerKeys() const
    {
        QSet<QString> keys = validPreviewLayerKeys(m_availableCameraIds);
        for (const QString& sourceId : m_staticSourceIds)
        {
            keys.insert(ScopeOneCore::staticLayerKey(sourceId));
        }
        for (const QString& sourceId : m_toolSourceIds)
        {
            keys.insert(ScopeOneCore::toolLayerKey(sourceId));
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

    // Sets whether the calibrated scale bar is drawn on preview
    void PreviewWidget::setScaleBarVisible(bool visible)
    {
        if (m_scaleBarVisible == visible)
        {
            return;
        }
        m_scaleBarVisible = visible;
        emit scaleBarVisibilityChanged(m_scaleBarVisible);
        update();
    }

    bool PreviewWidget::isScaleBarVisible() const
    {
        return m_scaleBarVisible;
    }

    // Sets whether overexposure and underexposure clipping warning is active
    void PreviewWidget::setClippingWarningEnabled(bool enabled)
    {
        if (m_clippingWarning == enabled)
        {
            return;
        }
        m_clippingWarning = enabled;
        emit clippingWarningChanged(m_clippingWarning);
        update();
    }

    bool PreviewWidget::isClippingWarningEnabled() const
    {
        return m_clippingWarning;
    }

    void PreviewWidget::setActiveLayerKey(const QString& key)
    {
        if (m_activeLayerKey == key)
        {
            return;
        }
        m_activeLayerKey = key;
        update();
    }

    // Sets the callback providing pixel size in micrometers per layer
    void PreviewWidget::setPixelSizeCallback(std::function<double(const QString&)> callback)
    {
        m_pixelSizeCallback = std::move(callback);
        update();
    }

    // Refreshes placeholder state and schedules repaint
    void PreviewWidget::updateImageDisplay()
    {
        bool hasDisplayableFrame = false;
        {
            QMutexLocker lock(&m_mutex);
            for (auto it = m_frameSources.constBegin(); it != m_frameSources.constEnd(); ++it)
            {
                const FrameSourceState& frameState = it.value();
                if (hasRawFrame(frameState) || frameState.processedFrame.isValid())
                {
                    hasDisplayableFrame = true;
                    break;
                }
            }
        }

        m_placeholderText = QStringLiteral("No image loaded\nClick 'Start Preview' to view the camera feed");
        if (hasDisplayableFrame && visibleLayerKeys().isEmpty())
        {
            m_placeholderText = QStringLiteral("No layer visible");
        }
        update();
    }

    // Publishes raw and processed throughput over one shared time window
    void PreviewWidget::updateFrameRates()
    {
        if (m_fpsStates.isEmpty())
        {
            return;
        }

        bool changed = false;
        for (auto it = m_fpsStates.begin(); it != m_fpsStates.end(); ++it)
        {
            FpsState& state = it.value();
            const qint64 elapsedNs = state.intervalTimer.nsecsElapsed();
            const double fps = elapsedNs > 0
                                   ? (static_cast<double>(state.framesSinceUpdate) * 1000000000.0)
                                       / static_cast<double>(elapsedNs)
                                   : 0.0;
            state.framesSinceUpdate = 0;
            state.intervalTimer.restart();
            double& currentFps = m_layerFps[it.key()];
            if (!qFuzzyCompare(currentFps + 1.0, fps + 1.0))
            {
                currentFps = fps;
                changed = true;
            }
        }
        if (changed)
        {
            updateLayerInfoDisplay();
        }
    }

    // Checks whether a frame source has raw data
    bool PreviewWidget::hasRawFrame(const FrameSourceState& frameState) const
    {
        return frameState.rawFrame.isValid();
    }

    // Copies frame source state under the preview mutex
    QMap<QString, PreviewWidget::FrameSourceState> PreviewWidget::snapshotFrameSources() const
    {
        QMap<QString, FrameSourceState> frameSources;
        {
            QMutexLocker lock(&m_mutex);
            frameSources = m_frameSources;
        }
        for (auto it = frameSources.begin(); it != frameSources.end(); ++it)
        {
            const ImageSceneModel::SourceDisplayTransform transform =
                m_sceneModel->sourceDisplayTransform(it.key());
            it->offsetX = transform.offsetX;
            it->offsetY = transform.offsetY;
            it->flipX = transform.flipX;
            it->flipY = transform.flipY;
            it->zoomPercent = transform.zoomPercent;
        }
        return frameSources;
    }

    // Builds render metadata for sources with displayable frames
    std::vector<PreviewWidget::FrameSourceRenderInfo> PreviewWidget::buildFrameSourceRenderInfos(
        const QMap<QString, FrameSourceState>& frameSources) const
    {
        std::vector<FrameSourceRenderInfo> frameSourceRenderInfos;
        for (auto it = frameSources.constBegin(); it != frameSources.constEnd(); ++it)
        {
            const QString& sourceId = it.key();
            const FrameSourceState& frameState = it.value();
            const bool hasProcessedFrame = frameState.processedFrame.isValid();
            const bool hasRawFrameNow = hasRawFrame(frameState);
            if (hasProcessedFrame || hasRawFrameNow)
            {
                frameSourceRenderInfos.push_back({sourceId, &frameState, hasProcessedFrame, hasRawFrameNow});
            }
        }
        return frameSourceRenderInfos;
    }

    // Builds a complete render snapshot for painting or hit testing
    void PreviewWidget::buildRenderSnapshot(QMap<QString, FrameSourceState>& frameSources,
                                            std::vector<FrameSourceRenderInfo>& frameSourceRenderInfos,
                                            std::vector<RenderItem>& renderItems) const
    {
        frameSources = snapshotFrameSources();
        frameSourceRenderInfos = buildFrameSourceRenderInfos(frameSources);
        renderItems = buildRenderItems(frameSourceRenderInfos);
    }

    // Resolves the displayed image rectangle for one layer
    bool PreviewWidget::resolveDisplayGeometry(const FrameSourceState& frameState,
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
                                                    FrameSourceState& frameState,
                                                    bool& processed,
                                                    QRect& itemArea,
                                                    QRect& displayRect,
                                                    QSize& imageSize) const
    {
        QMap<QString, FrameSourceState> frameSources;
        std::vector<FrameSourceRenderInfo> frameSourceRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(frameSources, frameSourceRenderInfos, renderItems);

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
    bool PreviewWidget::mapWidgetPositionToImage(const FrameSourceState& frameState,
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
    bool PreviewWidget::mapWidgetRectToImage(const FrameSourceState& frameState,
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
    bool PreviewWidget::mapImagePositionToWidget(const FrameSourceState& frameState,
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

    // Shows placeholder text above the OpenGL surface
    void PreviewWidget::showPlaceholder(const QString& text)
    {
        m_placeholderLabel->setText(
            text.isEmpty() ? QStringLiteral("No image loaded") : text);
        m_placeholderLabel->show();
        m_placeholderLabel->raise();
    }

    // Draws one image-space markup over one render item
    bool PreviewWidget::drawMarkup(QPainter& painter,
                                   const ImageSceneModel::Markup& markup,
                                   const RenderItem& item) const
    {
        if (item.layerKey != markup.layerKey || !item.info || !item.info->frameState)
        {
            return false;
        }
        if (!markup.visible)
        {
            return false;
        }
        QRect displayRect;
        QSize imageSize;
        if (!resolveDisplayGeometry(*item.info->frameState,
                                    item.processed,
                                    item.area,
                                    displayRect,
                                    imageSize))
        {
            return false;
        }
        const QRect imageBounds(QPoint(0, 0), imageSize);

        QPen linePen(QColor(255, 210, 0));
        linePen.setWidth(markup.selected ? 3 : 2);
        QPen rectPen(QColor(0, 220, 180));
        rectPen.setWidth(markup.selected ? 3 : 2);
        painter.setBrush(Qt::NoBrush);

        if (markup.type == ImageSceneModel::MarkupType::Line)
        {
            QPoint startImage;
            QPoint endImage;
            if (!clipLineToRect(markup.start, markup.end, imageBounds, startImage, endImage))
            {
                return false;
            }
            QPoint startWidget;
            QPoint endWidget;
            if (!mapImagePositionToWidget(*item.info->frameState,
                                          item.processed,
                                          item.area,
                                          startImage,
                                          startWidget)
                || !mapImagePositionToWidget(*item.info->frameState,
                                             item.processed,
                                             item.area,
                                             endImage,
                                             endWidget))
            {
                return false;
            }

            QPen pen = linePen;
            if (markup.role == ImageSceneModel::MarkupRole::CrossSection && item.processed)
            {
                pen.setStyle(Qt::DashLine);
            }
            painter.setPen(pen);
            painter.drawLine(startWidget, endWidget);
            if (!markup.label.isEmpty())
            {
                painter.drawText(startWidget + QPoint(4, -4), markup.label);
            }
            return true;
        }

        if (markup.type == ImageSceneModel::MarkupType::Rect)
        {
            QPoint topLeft;
            QPoint bottomRight;
            const QRect imageRect = markup.rect.normalized().intersected(imageBounds);
            if (imageRect.isEmpty())
            {
                return false;
            }
            if (!mapImagePositionToWidget(*item.info->frameState,
                                          item.processed,
                                          item.area,
                                          imageRect.topLeft(),
                                          topLeft)
                || !mapImagePositionToWidget(*item.info->frameState,
                                             item.processed,
                                             item.area,
                                             imageRect.bottomRight(),
                                             bottomRight))
            {
                return false;
            }

            QPen pen = rectPen;
            if (markup.role == ImageSceneModel::MarkupRole::Roi)
            {
                pen.setColor(QColor(0, 180, 255));
                pen.setStyle(Qt::DashLine);
            }
            painter.setPen(pen);
            const QRect widgetRect = QRect(topLeft, bottomRight).normalized();
            painter.drawRect(widgetRect);
            if (!markup.label.isEmpty())
            {
                painter.drawText(widgetRect.topLeft() + QPoint(4, -4), markup.label);
            }
            return true;
        }

        return false;
    }

    // Draws persisted markups over their target layers
    void PreviewWidget::drawMarkups(QPainter& painter, const std::vector<RenderItem>& renderItems) const
    {
        if (!m_sceneModel->hasMarkups())
        {
            return;
        }

        for (const ImageSceneModel::Markup& markup : m_sceneModel->markups())
        {
            for (const RenderItem& item : renderItems)
            {
                if (drawMarkup(painter, markup, item))
                {
                    break;
                }
            }
        }
    }

    // Draws the current drag operation through the same markup renderer
    void PreviewWidget::drawActiveInteractionMarkup(QPainter& painter,
                                                    const std::vector<RenderItem>& renderItems) const
    {
        if (m_measurementLineDrawingMode
            && m_measurementLineDragging
            && !m_measurementLineTargetLayerKey.isEmpty())
        {
            for (const RenderItem& item : renderItems)
            {
                if (item.layerKey != m_measurementLineTargetLayerKey || !item.info || !item.info->frameState)
                {
                    continue;
                }

                ImageSceneModel::Markup markup;
                markup.type = ImageSceneModel::MarkupType::Line;
                markup.role = ImageSceneModel::MarkupRole::Measurement;
                markup.layerKey = m_measurementLineTargetLayerKey;
                QRect displayRect;
                QSize imageSize;
                QPoint clippedStart;
                QPoint clippedEnd;
                if (!resolveDisplayGeometry(*item.info->frameState,
                                            item.processed,
                                            item.area,
                                            displayRect,
                                            imageSize)
                    || !clipLineToRect(m_measurementLineStart,
                                       m_measurementLineEnd,
                                       displayRect,
                                       clippedStart,
                                       clippedEnd)
                    || !mapWidgetPositionToImage(*item.info->frameState,
                                                 item.processed,
                                                 item.area,
                                                 clippedStart,
                                                 markup.start)
                    || !mapWidgetPositionToImage(*item.info->frameState,
                                                 item.processed,
                                                 item.area,
                                                 clippedEnd,
                                                 markup.end))
                {
                    break;
                }
                drawMarkup(painter, markup, item);
                break;
            }
        }

        if (m_crossSectionDrawingMode && m_crossSectionDragging && !m_crossSectionTargetLayerKey.isEmpty())
        {
            for (const RenderItem& item : renderItems)
            {
                if (item.layerKey != m_crossSectionTargetLayerKey || !item.info || !item.info->frameState)
                {
                    continue;
                }

                QRect displayRect;
                QSize imageSize;
                QPoint clippedStart;
                QPoint clippedEnd;
                QPoint imageStart;
                QPoint imageEnd;
                if (!resolveDisplayGeometry(*item.info->frameState,
                                            item.processed,
                                            item.area,
                                            displayRect,
                                            imageSize)
                    || !clipLineToRect(m_crossSectionStart, m_crossSectionEnd, displayRect, clippedStart, clippedEnd)
                    || !mapWidgetPositionToImage(*item.info->frameState,
                                                 item.processed,
                                                 item.area,
                                                 clippedStart,
                                                 imageStart)
                    || !mapWidgetPositionToImage(*item.info->frameState,
                                                 item.processed,
                                                 item.area,
                                                 clippedEnd,
                                                 imageEnd))
                {
                    break;
                }

                ImageSceneModel::Markup markup;
                markup.type = ImageSceneModel::MarkupType::Line;
                markup.role = ImageSceneModel::MarkupRole::CrossSection;
                markup.layerKey = m_crossSectionTargetLayerKey;
                markup.start = imageStart;
                markup.end = imageEnd;
                drawMarkup(painter, markup, item);
                break;
            }
        }

        if (m_roiDrawingMode && m_roiDragging && !m_roiTargetLayerKey.isEmpty())
        {
            for (const RenderItem& item : renderItems)
            {
                if (item.layerKey != m_roiTargetLayerKey || !item.info || !item.info->frameState)
                {
                    continue;
                }

                QRect imageRect;
                if (!mapWidgetRectToImage(*item.info->frameState,
                                          item.processed,
                                          item.area,
                                          QRect(m_roiStart, m_roiEnd),
                                          imageRect))
                {
                    break;
                }

                ImageSceneModel::Markup markup;
                markup.type = ImageSceneModel::MarkupType::Rect;
                markup.role = ImageSceneModel::MarkupRole::Roi;
                markup.layerKey = m_roiTargetLayerKey;
                markup.rect = imageRect;
                drawMarkup(painter, markup, item);
                break;
            }
        }
    }

    // Draws a calibrated scale bar overlay in the corner of visible image areas
    void PreviewWidget::drawScaleBar(QPainter& painter, const std::vector<RenderItem>& renderItems) const
    {
        if (renderItems.empty())
        {
            return;
        }

        QSet<QRect> drawnAreas;
        for (const RenderItem& item : renderItems)
        {
            if (!item.info || !item.info->frameState || drawnAreas.contains(item.area))
            {
                continue;
            }

            QRect displayRect;
            QSize imageSize;
            if (!resolveDisplayGeometry(*item.info->frameState, item.processed, item.area, displayRect, imageSize))
            {
                continue;
            }

            if (imageSize.width() <= 0 || displayRect.width() <= 0)
            {
                continue;
            }

            const double pixelSize = m_pixelSizeCallback ? m_pixelSizeCallback(item.layerKey) : 0.0;
            if (pixelSize <= 0.0)
            {
                continue;
            }

            const double pixelsPerImagePixel = static_cast<double>(displayRect.width()) / static_cast<double>(imageSize.width());
            const double screenPixelsPerUm = pixelsPerImagePixel / pixelSize;
            if (screenPixelsPerUm <= 1e-6)
            {
                continue;
            }

            const double targetUm = 80.0 / screenPixelsPerUm;
            static const double niceSteps[] = {
                0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0,
                100.0, 200.0, 250.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 25000.0, 50000.0
            };

            double bestUm = niceSteps[0];
            double minDiff = std::abs(targetUm - bestUm);
            for (double step : niceSteps)
            {
                const double diff = std::abs(targetUm - step);
                if (diff < minDiff)
                {
                    minDiff = diff;
                    bestUm = step;
                }
            }

            const int barWidthPx = static_cast<int>(std::round(bestUm * screenPixelsPerUm));
            if (barWidthPx < 10 || barWidthPx > displayRect.width() - 20)
            {
                continue;
            }

            QString labelText;
            if (bestUm >= 1000.0)
            {
                labelText = QString::number(bestUm / 1000.0, 'g', 3) + QStringLiteral(" mm");
            }
            else if (bestUm >= 1.0)
            {
                labelText = QString::number(bestUm, 'g', 3) + QStringLiteral(" um");
            }
            else
            {
                labelText = QString::number(bestUm * 1000.0, 'g', 3) + QStringLiteral(" nm");
            }

            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);

            QFont font = painter.font();
            font.setPointSize(9);
            font.setBold(true);
            painter.setFont(font);

            const QFontMetrics fm(font);
            const int textWidth = fm.horizontalAdvance(labelText);
            const int boxWidth = std::max(barWidthPx, textWidth) + 16;
            const int boxHeight = fm.height() + 14;

            const int margin = 12;
            const int boxX = displayRect.right() - boxWidth - margin;
            const int boxY = displayRect.bottom() - boxHeight - margin;
            const QRect boxRect(boxX, boxY, boxWidth, boxHeight);

            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 160));
            painter.drawRoundedRect(boxRect, 4, 4);

            painter.setPen(Qt::white);
            const QRect textRect(boxX, boxY + 2, boxWidth, fm.height());
            painter.drawText(textRect, Qt::AlignCenter, labelText);

            const int barX = boxX + (boxWidth - barWidthPx) / 2;
            const int barY = boxY + fm.height() + 6;
            QPen linePen(Qt::white, 3, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(linePen);
            painter.drawLine(barX, barY, barX + barWidthPx, barY);

            painter.restore();
            drawnAreas.insert(item.area);
        }
    }

    // Draws tile names and active border outlines in Grid View
    void PreviewWidget::drawTileLabelsAndBadges(QPainter& painter, const std::vector<RenderItem>& renderItems) const
    {
        if (renderItems.empty())
        {
            return;
        }
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing);

        const QFont font(QStringLiteral("Segoe UI"), 9);
        painter.setFont(font);
        const QFontMetrics fm(font);
        const bool overlay = m_layerLayoutMode == LayerLayoutMode::Overlay
                             && renderItems.size() > 1;
        bool overlayBadgeDrawn = false;

        for (const auto& item : renderItems)
        {
            if (!item.info || !item.info->frameState)
            {
                continue;
            }

            if (overlay)
            {
                if (!m_activeLayerKey.isEmpty() && item.layerKey != m_activeLayerKey)
                {
                    continue;
                }
                if (overlayBadgeDrawn)
                {
                    continue;
                }
            }

            if (!m_activeLayerKey.isEmpty() && item.layerKey == m_activeLayerKey && renderItems.size() > 1)
            {
                painter.setPen(QPen(QColor(0, 200, 255, 200), 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(item.area.adjusted(1, 1, -1, -1));
            }

            if (m_layerLayoutMode == LayerLayoutMode::SideBySide || renderItems.size() > 1)
            {
                const QString name = layerName(item.layerKey);
                const int frameW = item.processed ? item.info->frameState->processedFrame.width : item.info->frameState->rawFrame.width;
                const int frameH = item.processed ? item.info->frameState->processedFrame.height : item.info->frameState->rawFrame.height;
                const QString labelText = (frameW > 0 && frameH > 0)
                                              ? QString("%1 (%2x%3)").arg(name).arg(frameW).arg(frameH)
                                              : name;
                const int textWidth = fm.horizontalAdvance(labelText);
                const QRect badgeRect(item.area.left() + 8, item.area.top() + 8, textWidth + 14, fm.height() + 6);

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(15, 18, 22, 180));
                painter.drawRoundedRect(badgeRect, 4, 4);

                painter.setPen(QColor(230, 235, 240));
                painter.drawText(badgeRect, Qt::AlignCenter, labelText);
                overlayBadgeDrawn = true;
            }
        }
        painter.restore();
    }

    // Draws live cursor coordinates and pixel value HUD badge in bottom-left
    void PreviewWidget::drawCursorHud(QPainter& painter, const std::vector<RenderItem>& renderItems) const
    {
        if (m_hoverWidgetPos.x() < 0 || m_hoverWidgetPos.y() < 0)
        {
            return;
        }

        for (const auto& item : renderItems)
        {
            if (!item.info || !item.info->frameState || !item.area.contains(m_hoverWidgetPos))
            {
                continue;
            }

            QPoint imagePos;
            if (mapWidgetPositionToImage(*item.info->frameState, item.processed, item.area, m_hoverWidgetPos, imagePos))
            {
                const auto& frame = item.processed ? item.info->frameState->processedFrame : item.info->frameState->rawFrame;
                int pixelVal = 0;
                bool hasVal = false;
                if (frame.isValid() && imagePos.x() >= 0 && imagePos.x() < frame.width
                    && imagePos.y() >= 0 && imagePos.y() < frame.height)
                {
                    if (frame.isMono8())
                    {
                        const uchar* ptr = reinterpret_cast<const uchar*>(frame.bytes.constData());
                        pixelVal = ptr[imagePos.y() * frame.stride + imagePos.x()];
                        hasVal = true;
                    }
                    else if (frame.isMono16())
                    {
                        const quint16* ptr = reinterpret_cast<const quint16*>(
                            frame.bytes.constData() + imagePos.y() * frame.stride);
                        pixelVal = ptr[imagePos.x()];
                        hasVal = true;
                    }
                }

                painter.save();
                painter.setRenderHint(QPainter::Antialiasing);
                const QFont font(QStringLiteral("Segoe UI"), 9);
                painter.setFont(font);
                const QFontMetrics fm(font);

                const QString hudText = hasVal
                                            ? QString("X: %1  Y: %2 | Val: %3").arg(imagePos.x()).arg(imagePos.y()).arg(pixelVal)
                                            : QString("X: %1  Y: %2").arg(imagePos.x()).arg(imagePos.y());
                const int textWidth = fm.horizontalAdvance(hudText);
                const QRect hudRect(item.area.left() + 8, item.area.bottom() - fm.height() - 14, textWidth + 14, fm.height() + 6);

                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(15, 18, 22, 200));
                painter.drawRoundedRect(hudRect, 4, 4);

                painter.setPen(QColor(0, 200, 255));
                painter.drawText(hudRect, Qt::AlignCenter, hudText);
                painter.restore();
                break;
            }
        }
    }

    bool PreviewWidget::markupAtWidgetPosition(const QPoint& widgetPos,
                                               ImageSceneModel::Markup& outMarkup,
                                               PreviewInteractionTarget& outTarget,
                                               MarkupEditMode& outEditMode) const
    {
        if (!m_sceneModel->hasMarkups())
        {
            return false;
        }

        QMap<QString, FrameSourceState> frameSources;
        std::vector<FrameSourceRenderInfo> frameSourceRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(frameSources, frameSourceRenderInfos, renderItems);

        const QList<ImageSceneModel::Markup> markups = m_sceneModel->markups();
        for (int markupIndex = markups.size() - 1; markupIndex >= 0; --markupIndex)
        {
            const ImageSceneModel::Markup& markup = markups.at(markupIndex);
            if (!markup.visible)
            {
                continue;
            }

            for (const RenderItem& item : renderItems)
            {
                if (item.layerKey != markup.layerKey || !item.info || !item.info->frameState)
                {
                    continue;
                }

                QRect displayRect;
                QSize imageSize;
                if (!resolveDisplayGeometry(*item.info->frameState,
                                            item.processed,
                                            item.area,
                                            displayRect,
                                            imageSize)
                    || !displayRect.contains(widgetPos))
                {
                    continue;
                }

                QPoint imagePos;
                if (!mapWidgetPositionToImage(*item.info->frameState,
                                               item.processed,
                                               item.area,
                                               widgetPos,
                                               imagePos))
                {
                    continue;
                }

                if (markup.type == ImageSceneModel::MarkupType::Line)
                {
                    QPoint startWidget;
                    QPoint endWidget;
                    if (!mapImagePositionToWidget(*item.info->frameState,
                                                  item.processed,
                                                  item.area,
                                                  markup.start,
                                                  startWidget)
                        || !mapImagePositionToWidget(*item.info->frameState,
                                                     item.processed,
                                                     item.area,
                                                     markup.end,
                                                     endWidget))
                    {
                        continue;
                    }
                    if (QLineF(QPointF(widgetPos), QPointF(startWidget)).length() <= 8.0)
                    {
                        outEditMode = MarkupEditMode::LineStart;
                    }
                    else if (QLineF(QPointF(widgetPos), QPointF(endWidget)).length() <= 8.0)
                    {
                        outEditMode = MarkupEditMode::LineEnd;
                    }
                    else if (pointSegmentDistance(widgetPos, startWidget, endWidget) <= 6.0)
                    {
                        outEditMode = MarkupEditMode::Move;
                    }
                    else
                    {
                        continue;
                    }
                }
                else if (markup.type == ImageSceneModel::MarkupType::Rect)
                {
                    QPoint topLeft;
                    QPoint bottomRight;
                    if (!mapImagePositionToWidget(*item.info->frameState,
                                                  item.processed,
                                                  item.area,
                                                  markup.rect.normalized().topLeft(),
                                                  topLeft)
                        || !mapImagePositionToWidget(*item.info->frameState,
                                                     item.processed,
                                                     item.area,
                                                     markup.rect.normalized().bottomRight(),
                                                     bottomRight))
                    {
                        continue;
                    }
                    const QRect widgetRect = QRect(topLeft, bottomRight).normalized();
                    if (QLineF(QPointF(widgetPos), QPointF(widgetRect.topLeft())).length() <= 8.0)
                    {
                        outEditMode = MarkupEditMode::RectTopLeft;
                    }
                    else if (QLineF(QPointF(widgetPos), QPointF(widgetRect.topRight())).length() <= 8.0)
                    {
                        outEditMode = MarkupEditMode::RectTopRight;
                    }
                    else if (QLineF(QPointF(widgetPos), QPointF(widgetRect.bottomLeft())).length() <= 8.0)
                    {
                        outEditMode = MarkupEditMode::RectBottomLeft;
                    }
                    else if (QLineF(QPointF(widgetPos), QPointF(widgetRect.bottomRight())).length() <= 8.0)
                    {
                        outEditMode = MarkupEditMode::RectBottomRight;
                    }
                    else if (widgetRect.adjusted(-6, -6, 6, 6).contains(widgetPos))
                    {
                        outEditMode = MarkupEditMode::Move;
                    }
                    else
                    {
                        continue;
                    }
                }

                outMarkup = markup;
                outTarget.layerKey = item.layerKey;
                outTarget.sourceId = item.info->sourceId;
                outTarget.imagePos = imagePos;
                outTarget.itemArea = item.area;
                outTarget.processed = item.processed;
                outTarget.displayRect = displayRect;
                return true;
            }
        }

        return false;
    }

    void PreviewWidget::clearSelectedMarkups()
    {
        if (!m_sceneModel->hasMarkups())
        {
            return;
        }

        const QList<ImageSceneModel::Markup> markups = m_sceneModel->markups();
        bool measurementRemoved = false;
        for (const ImageSceneModel::Markup& markup : markups)
        {
            if (!markup.selected)
            {
                continue;
            }
            measurementRemoved = measurementRemoved
                || markup.role == ImageSceneModel::MarkupRole::Measurement;
            m_sceneModel->remove(markup.id);
        }
        if (measurementRemoved)
        {
            emit measurementLineCleared();
        }
    }

    // Draws one render item into its assigned area
    void PreviewWidget::drawRenderItem(const RenderItem& item)
    {
        if (!item.info || !item.info->frameState)
        {
            return;
        }

        const FrameSourceState& frameState = *item.info->frameState;
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
                            frameState.processedRevision,
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
                            frameState.rawRevision,
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
        if (m_layerFps.isEmpty())
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
            if (!m_layerFps.contains(key))
            {
                return;
            }
            scopeone::core::DocumentLayer layer;
            if (!m_sceneModel->findLayer(key, layer) || layer.width <= 0 || layer.height <= 0)
            {
                return;
            }
            appendedKeys.insert(key);
            const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(key);
            if (m_staticSourceIds.contains(sourceId))
            {
                lines.append(QString("%1: %2×%3")
                             .arg(layerName(key))
                             .arg(layer.width)
                             .arg(layer.height));
                return;
            }

            lines.append(QString("%1: %2×%3 @ %4 FPS")
                         .arg(layerName(key))
                         .arg(layer.width)
                         .arg(layer.height)
                         .arg(m_layerFps.value(key), 0, 'f', 1));
        };

        for (const QString& cameraId : m_availableCameraIds)
        {
            appendInfoLine(previewLayerKey(cameraId, false));
            appendInfoLine(previewLayerKey(cameraId, true));
        }
        for (auto it = m_layerFps.constBegin(); it != m_layerFps.constEnd(); ++it)
        {
            appendInfoLine(it.key());
        }

        m_layerInfoText = lines.join(QStringLiteral("\n"));
        emit layerInfoTextChanged(m_layerInfoText);
    }

    // Resolves one widget point to the topmost matching preview item
    bool PreviewWidget::resolveInteractionTarget(const QPoint& widgetPos,
                                                 PreviewInteractionTarget& outTarget,
                                                 const QString& sourceId,
                                                 bool rawOnly,
                                                 const QString& layerKey) const
    {
        QMap<QString, FrameSourceState> frameSources;
        std::vector<FrameSourceRenderInfo> frameSourceRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(frameSources, frameSourceRenderInfos, renderItems);
        const QString sourceFilter = sourceId.trimmed();
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
            if (!sourceFilter.isEmpty() && item.info->sourceId != sourceFilter)
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

            const FrameSourceState& frameState = *item.info->frameState;
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
            outTarget.sourceId = item.info->sourceId;
            outTarget.imagePos = imagePos;
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
                                            const QString& sourceId,
                                            bool rawOnly) const
    {
        return resolveInteractionTarget(widgetPos, outTarget, sourceId, rawOnly, QString());
    }

    // Initializes OpenGL state for preview rendering
    void PreviewWidget::initializeGL()
    {
        initializeOpenGLFunctions();

        const QSurfaceFormat format = context()->format();
        QString profile = QStringLiteral("No profile");
        if (format.profile() == QSurfaceFormat::CoreProfile)
        {
            profile = QStringLiteral("Core profile");
        }
        else if (format.profile() == QSurfaceFormat::CompatibilityProfile)
        {
            profile = QStringLiteral("Compatibility profile");
        }
        const auto glString = [this](GLenum name)
        {
            const GLubyte* value = glGetString(name);
            return value
                       ? QString::fromLatin1(reinterpret_cast<const char*>(value))
                       : QStringLiteral("Unavailable");
        };
        qInfo().noquote()
            << QStringLiteral("OpenGL context: %1 %2.%3, %4")
                   .arg(context()->isOpenGLES() ? QStringLiteral("OpenGL ES")
                                               : QStringLiteral("Desktop OpenGL"))
                   .arg(format.majorVersion())
                   .arg(format.minorVersion())
                   .arg(profile);
        qInfo().noquote() << QStringLiteral("OpenGL vendor: %1").arg(glString(GL_VENDOR));
        qInfo().noquote() << QStringLiteral("OpenGL renderer: %1").arg(glString(GL_RENDERER));
        qInfo().noquote() << QStringLiteral("OpenGL version: %1").arg(glString(GL_VERSION));
        qInfo().noquote() << QStringLiteral("GLSL version: %1").arg(glString(GL_SHADING_LANGUAGE_VERSION));

        glDisable(GL_DEPTH_TEST);
        ensureGlPipeline();
    }

    // Updates the OpenGL viewport after resize
    void PreviewWidget::resizeGL(int, int)
    {
        applyViewportForRect(rect());
        m_placeholderLabel->setGeometry(rect());
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
        const std::vector<FrameSourceRenderInfo>& frameSourceRenderInfos) const
    {
        std::vector<RenderItem> items;
        if (frameSourceRenderInfos.empty())
        {
            return items;
        }

        const QRect full(0, 0, width(), height());
        QMap<QString, const FrameSourceRenderInfo*> renderInfoBySourceId;
        for (const auto& info : frameSourceRenderInfos)
        {
            renderInfoBySourceId.insert(info.sourceId, &info);
        }

        auto addItem = [&](const FrameSourceRenderInfo* info, bool processed, const QString& layerKey, const QRect& area)
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
            LayerDisplaySettings display = layerDisplaySettings(layerKey);
            if (!display.visible)
            {
                return;
            }
            const bool firstVisibleInArea = std::none_of(items.cbegin(), items.cend(),
                                                         [&area](const RenderItem& item)
                                                         {
                                                             return item.area == area;
                                                         });
            items.push_back({info, processed, layerKey, area, display, firstVisibleInArea});
        };

        std::vector<LayerRenderItem> layers;
        layers.reserve(frameSourceRenderInfos.size() * 2);

        for (const QString& layerKey : m_sceneModel->layerIds())
        {
            if (m_layerLayoutMode == LayerLayoutMode::SideBySide && layers.size() >= 4)
            {
                break;
            }
            if (!layerDisplaySettings(layerKey).visible)
            {
                continue;
            }

            const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(layerKey);
            const auto it = renderInfoBySourceId.constFind(sourceId);
            if (it == renderInfoBySourceId.constEnd())
            {
                continue;
            }

            const FrameSourceRenderInfo* info = it.value();
            const bool processed = ScopeOneCore::isProcessedLayerKey(layerKey);
            if (processed && !info->hasProcessedFrame)
            {
                continue;
            }
            if (!processed && !info->hasRawFrame)
            {
                continue;
            }

            layers.push_back({info, processed, layerKey});
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
                addItem(layers[i].info, layers[i].processed, layers[i].layerKey, areas[i]);
            }
            return items;
        }

        for (const LayerRenderItem& layer : layers)
        {
            addItem(layer.info, layer.processed, layer.layerKey, full);
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
        QMap<QString, FrameSourceState> frameSources;
        std::vector<FrameSourceRenderInfo> frameSourceRenderInfos;
        std::vector<RenderItem> renderItems;
        buildRenderSnapshot(frameSources, frameSourceRenderInfos, renderItems);

        if (frameSourceRenderInfos.empty())
        {
            showPlaceholder(m_placeholderText);
            return;
        }
        if (canGpu)
        {
            if (renderItems.empty())
            {
                showPlaceholder(m_placeholderText);
                return;
            }

            m_placeholderLabel->hide();
            for (const auto& item : renderItems)
            {
                drawRenderItem(item);
            }

            QPainter p(this);
            if (m_sceneModel->hasMarkups()
                || (m_roiDrawingMode && m_roiDragging)
                || (m_crossSectionDrawingMode && m_crossSectionDragging))
            {
                drawMarkups(p, renderItems);
                drawActiveInteractionMarkup(p, renderItems);
            }
            if (m_scaleBarVisible)
            {
                drawScaleBar(p, renderItems);
            }
            drawTileLabelsAndBadges(p, renderItems);
            drawCursorHud(p, renderItems);
            return;
        }

        static bool warned = false;
        if (!warned)
        {
            qWarning() << "PreviewWidget: GPU rendering unavailable; CPU rendering is not enabled";
            warned = true;
        }
        showPlaceholder(QStringLiteral(
            "Preview unavailable\nOpenGL initialization failed on this system"));
        return;
    }

    // Creates shaders buffers and uniforms for preview rendering
    void PreviewWidget::ensureGlPipeline()
    {
        if (m_glInited) return;
        const QStringList colormaps = ImageSceneModel::supportedColormaps();
        const QByteArray atlas = buildColormapAtlas(colormaps);
        if (atlas.isEmpty())
        {
            return;
        }
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
        uniform int uShowClipping;
        uniform sampler2D uColormapLut;
        vec3 applyColormap(float t, int map) {
            vec2 lutSize = vec2(textureSize(uColormapLut, 0));
            float column = (t * (lutSize.x - 1.0) + 0.5) / lutSize.x;
            float row = (float(map) + 0.5) / lutSize.y;
            return texture(uColormapLut, vec2(column, row)).rgb;
        }
        void main(){
            vec4 s = texture(uTex, vUV);
            float t0 = s.r * uTexNormScale;
            if (uShowClipping == 1) {
                if (t0 >= 0.999) {
                    FragColor = vec4(1.0, 0.0, 0.0, uAlpha);
                    return;
                }
                if (t0 <= 0.0001) {
                    FragColor = vec4(0.0, 0.2, 1.0, uAlpha);
                    return;
                }
            }
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
        m_uColormapLut = m_prog.uniformLocation("uColormapLut");
        m_uUvScale = m_prog.uniformLocation("uUvScale");
        m_uUvOffset = m_prog.uniformLocation("uUvOffset");
        m_uShowClipping = m_prog.uniformLocation("uShowClipping");

        // Uploads all colormaps once for shader lookup
        glGenTextures(1, &m_colormapTexture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_colormapTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8,
                     kColormapSize, colormaps.size(), 0,
                     GL_RGB, GL_UNSIGNED_BYTE, atlas.constData());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glActiveTexture(GL_TEXTURE0);
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
                                        quint64 frameRevision,
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
        CachedTexture& cachedTexture = m_textureCache[textureKey];

        glBindTexture(GL_TEXTURE_2D, texId);

        if (cachedTexture.uploadedRevision != frameRevision)
        {
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
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            cachedTexture.uploadedRevision = frameRevision;
        }

        m_prog.bind();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);
        m_prog.setUniformValue(m_uTex, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_colormapTexture);
        m_prog.setUniformValue(m_uColormapLut, 1);
        glActiveTexture(GL_TEXTURE0);
        const int levelDomain = qMax(1, display.levelDomainMax);
        m_prog.setUniformValue(m_uMinNorm, static_cast<float>(display.levelMin) / static_cast<float>(levelDomain));
        m_prog.setUniformValue(m_uMaxNorm, static_cast<float>(display.levelMax) / static_cast<float>(levelDomain));
        const float bitMax = static_cast<float>(qMax(1, frame.maxValue()));
        const float sampleMax = (internalFormat == GL_R16) ? 65535.0f : 255.0f;
        const float texNormScale = sampleMax / bitMax;
        m_prog.setUniformValue(m_uTexNormScale, texNormScale);
        const float opacity = display.blending == Blending::Opaque
                                  ? 1.0f
                                  : qBound(0.0f, static_cast<float>(display.opacityPercent) / 100.0f, 1.0f);
        m_prog.setUniformValue(m_uAlpha, opacity);
        m_prog.setUniformValue(m_uGamma, static_cast<float>(std::clamp(display.gamma, 0.2, 2.0)));
        m_prog.setUniformValue(m_uColormap, display.colormapIndex);
        m_prog.setUniformValue(m_uShowClipping, m_clippingWarning ? 1 : 0);
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
                                                const FrameSourceState& frameState,
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
        if (m_colormapTexture != 0)
        {
            glDeleteTextures(1, &m_colormapTexture);
            m_colormapTexture = 0;
        }
        doneCurrent();
    }

    // Starts ROI drawing for one camera
    void PreviewWidget::startROIDrawing(const QString& cameraId)
    {
        cancelMeasurementLineDrawing();
        if (m_crossSectionDrawingMode)
        {
            cancelCrossSectionDrawing();
        }
        m_roiDrawingMode = true;
        m_roiTargetCameraId = cameraId;
        m_roiTargetLayerKey.clear();
        m_roiDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    // Starts a measurement line for one exact preview layer
    void PreviewWidget::startMeasurementLineDrawingForLayer(const QString& layerKey)
    {
        cancelROIDrawing();
        cancelCrossSectionDrawing();
        m_measurementLineDrawingMode = true;
        m_measurementLineTargetLayerKey = layerKey.trimmed();
        m_measurementLineDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    // Cancels active measurement line drawing
    void PreviewWidget::cancelMeasurementLineDrawing()
    {
        if (!m_measurementLineDrawingMode)
        {
            return;
        }
        m_measurementLineDrawingMode = false;
        m_measurementLineDragging = false;
        m_measurementLineTargetLayerKey.clear();
        unsetCursor();
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

    // Starts cross section drawing for one exact preview layer
    void PreviewWidget::startCrossSectionDrawingForLayer(const QString& layerKey)
    {
        cancelMeasurementLineDrawing();
        if (m_roiDrawingMode)
        {
            cancelROIDrawing();
        }
        m_crossSectionDrawingMode = true;
        m_crossSectionTargetLayerKey = layerKey.trimmed();
        m_crossSectionTargetSourceId = ScopeOneCore::sourceIdFromLayerKey(m_crossSectionTargetLayerKey);
        m_crossSectionDragging = false;
        setFocus();
        setCursor(Qt::CrossCursor);
        update();
    }

    // Cancels active cross section drawing
    void PreviewWidget::cancelCrossSectionDrawing()
    {
        if (!m_crossSectionDrawingMode)
        {
            return;
        }
        m_crossSectionDrawingMode = false;
        m_crossSectionDragging = false;
        m_crossSectionTargetSourceId.clear();
        m_crossSectionTargetLayerKey.clear();
        unsetCursor();
        update();
    }

    // Clears the current cross section markup
    void PreviewWidget::clearCrossSection()
    {
        m_crossSectionDragging = false;
        m_crossSectionDrawingMode = false;
        m_crossSectionTargetSourceId.clear();
        m_crossSectionTargetLayerKey.clear();
        m_sceneModel->clearRole(ImageSceneModel::MarkupRole::CrossSection);
        unsetCursor();
        update();
    }

    // Starts active drawing interactions from a mouse press
    void PreviewWidget::mousePressEvent(QMouseEvent* event)
    {
        emit activated();
        emit mousePositionChanged(event->pos());
        if (m_measurementLineDrawingMode && event->button() == Qt::LeftButton)
        {
            PreviewInteractionTarget target;
            const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(m_measurementLineTargetLayerKey);
            if (!resolveInteractionTarget(event->pos(),
                                          target,
                                          sourceId,
                                          false,
                                          m_measurementLineTargetLayerKey))
            {
                return;
            }
            m_measurementLineStart = event->pos();
            m_measurementLineEnd = event->pos();
            m_measurementLineDragging = true;
            update();
            return;
        }

        if (m_crossSectionDrawingMode && event->button() == Qt::LeftButton)
        {
            QString sourceId = m_crossSectionTargetSourceId;
            PreviewInteractionTarget target;
            const bool ok = resolveInteractionTarget(event->pos(),
                                                     target,
                                                     sourceId,
                                                     false,
                                                     m_crossSectionTargetLayerKey);
            if (!ok)
            {
                return;
            }

            m_crossSectionTargetSourceId = target.sourceId;
            m_crossSectionTargetLayerKey = target.layerKey;
            m_crossSectionStart = event->pos();
            m_crossSectionEnd = event->pos();
            m_crossSectionDragging = true;
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
            if (m_staticSourceIds.contains(target.sourceId))
            {
                return;
            }

            m_roiTargetCameraId = target.sourceId;
            m_roiTargetLayerKey = target.layerKey;
            m_roiStart = event->pos();
            m_roiEnd = event->pos();
            m_roiDragging = true;
            update();
            return;
        }

        if (event->button() == Qt::LeftButton)
        {
            ImageSceneModel::Markup markup;
            PreviewInteractionTarget target;
            MarkupEditMode editMode = MarkupEditMode::None;
            if (markupAtWidgetPosition(event->pos(), markup, target, editMode))
            {
                m_sceneModel->selectOnly(markup.id);
                if (markup.type == ImageSceneModel::MarkupType::Line
                    && markup.role == ImageSceneModel::MarkupRole::Measurement)
                {
                    emit measurementLineInspected(markup.layerKey, markup.start, markup.end);
                }
                m_dragMarkupId = markup.id;
                m_dragMarkupOriginal = markup;
                m_dragMarkupStartImagePos = target.imagePos;
                m_dragMarkupEditMode = editMode;
                m_markupDragging = true;
                update();
                return;
            }
            m_sceneModel->selectOnly(QString());

            if (interactionTargetAt(event->pos(), target) && !target.layerKey.isEmpty())
            {
                setActiveLayerKey(target.layerKey);
                emit layerClicked(target.layerKey);
            }
        }

        QOpenGLWidget::mousePressEvent(event);
    }

    // Handles double click to maximize or restore grid tile
    void PreviewWidget::mouseDoubleClickEvent(QMouseEvent* event)
    {
        if (event->button() == Qt::LeftButton)
        {
            PreviewInteractionTarget target;
            if (interactionTargetAt(event->pos(), target) && !target.layerKey.isEmpty())
            {
                if (m_savedVisibleLayerKeys.isEmpty())
                {
                    m_savedVisibleLayerKeys = m_sceneModel->visibleLayerIds();
                    m_sceneModel->setVisibleLayers({target.layerKey});
                }
                else
                {
                    m_sceneModel->setVisibleLayers(m_savedVisibleLayerKeys);
                    m_savedVisibleLayerKeys.clear();
                }
                update();
                return;
            }
            if (!m_savedVisibleLayerKeys.isEmpty())
            {
                m_sceneModel->setVisibleLayers(m_savedVisibleLayerKeys);
                m_savedVisibleLayerKeys.clear();
                update();
                return;
            }
        }
        QOpenGLWidget::mouseDoubleClickEvent(event);
    }

    // Updates active drawing interactions during mouse move
    void PreviewWidget::mouseMoveEvent(QMouseEvent* event)
    {
        m_hoverWidgetPos = event->pos();
        emit mousePositionChanged(event->pos());
        update();
        if (m_measurementLineDrawingMode && m_measurementLineDragging)
        {
            m_measurementLineEnd = event->pos();
            update();
            return;
        }

        if (m_crossSectionDrawingMode && m_crossSectionDragging)
        {
            m_crossSectionEnd = event->pos();
            update();
            return;
        }

        if (m_roiDrawingMode && m_roiDragging)
        {
            m_roiEnd = event->pos();
            update();
            return;
        }

        if (m_markupDragging)
        {
            FrameSourceState frameState;
            bool processed = false;
            QRect itemArea;
            QRect displayRect;
            QSize imageSize;
            QPoint imagePos;
            if (resolveLayerDisplayGeometry(m_dragMarkupOriginal.layerKey,
                                            frameState,
                                            processed,
                                            itemArea,
                                            displayRect,
                                            imageSize)
                && mapWidgetPositionToImage(frameState, processed, itemArea, event->pos(), imagePos))
            {
                if (m_dragMarkupOriginal.type == ImageSceneModel::MarkupType::Line)
                {
                    QPoint start = m_dragMarkupOriginal.start;
                    QPoint end = m_dragMarkupOriginal.end;
                    if (m_dragMarkupEditMode == MarkupEditMode::LineStart)
                    {
                        start = imagePos;
                    }
                    else if (m_dragMarkupEditMode == MarkupEditMode::LineEnd)
                    {
                        end = imagePos;
                    }
                    else
                    {
                        const QPoint delta = imagePos - m_dragMarkupStartImagePos;
                        start += delta;
                        end += delta;
                    }
                    if (m_sceneModel->updateLine(m_dragMarkupId, start, end)
                        && m_dragMarkupOriginal.role == ImageSceneModel::MarkupRole::Measurement)
                    {
                        emit measurementLineInspected(m_dragMarkupOriginal.layerKey, start, end);
                    }
                }
                else if (m_dragMarkupOriginal.type == ImageSceneModel::MarkupType::Rect)
                {
                    const QRect originalRect = m_dragMarkupOriginal.rect.normalized();
                    if (m_dragMarkupEditMode == MarkupEditMode::RectTopLeft)
                    {
                        m_sceneModel->updateRect(m_dragMarkupId,
                                                  QRect(imagePos, originalRect.bottomRight()).normalized());
                    }
                    else if (m_dragMarkupEditMode == MarkupEditMode::RectTopRight)
                    {
                        m_sceneModel->updateRect(m_dragMarkupId,
                                                  QRect(QPoint(originalRect.left(), imagePos.y()),
                                                        QPoint(imagePos.x(), originalRect.bottom())).normalized());
                    }
                    else if (m_dragMarkupEditMode == MarkupEditMode::RectBottomLeft)
                    {
                        m_sceneModel->updateRect(m_dragMarkupId,
                                                  QRect(QPoint(imagePos.x(), originalRect.top()),
                                                        QPoint(originalRect.right(), imagePos.y())).normalized());
                    }
                    else if (m_dragMarkupEditMode == MarkupEditMode::RectBottomRight)
                    {
                        m_sceneModel->updateRect(m_dragMarkupId,
                                                  QRect(originalRect.topLeft(), imagePos).normalized());
                    }
                    else
                    {
                        const QPoint delta = imagePos - m_dragMarkupStartImagePos;
                        m_sceneModel->updateRect(m_dragMarkupId,
                                                  originalRect.translated(delta));
                    }
                }
            }
            update();
            return;
        }

        QOpenGLWidget::mouseMoveEvent(event);
    }

    // Finishes active drawing interactions in image coordinates
    void PreviewWidget::mouseReleaseEvent(QMouseEvent* event)
    {
        emit mousePositionChanged(event->pos());
        if (m_measurementLineDrawingMode
            && event->button() == Qt::LeftButton
            && m_measurementLineDragging)
        {
            m_measurementLineDragging = false;
            m_measurementLineEnd = event->pos();

            PreviewInteractionTarget startTarget;
            const QString sourceId = ScopeOneCore::sourceIdFromLayerKey(m_measurementLineTargetLayerKey);
            FrameSourceState frameState;
            bool processed = false;
            QRect itemArea;
            QRect displayRect;
            QSize imageSize;
            if (!resolveInteractionTarget(m_measurementLineStart,
                                          startTarget,
                                          sourceId,
                                          false,
                                          m_measurementLineTargetLayerKey)
                || !resolveLayerDisplayGeometry(m_measurementLineTargetLayerKey,
                                                frameState,
                                                processed,
                                                itemArea,
                                                displayRect,
                                                imageSize))
            {
                cancelMeasurementLineDrawing();
                return;
            }

            QPoint clippedStart;
            QPoint clippedEnd;
            QPoint imageStart;
            QPoint imageEnd;
            if (!clipLineToRect(m_measurementLineStart,
                                m_measurementLineEnd,
                                displayRect,
                                clippedStart,
                                clippedEnd)
                || !mapWidgetPositionToImage(frameState,
                                             processed,
                                             itemArea,
                                             clippedStart,
                                             imageStart)
                || !mapWidgetPositionToImage(frameState,
                                             processed,
                                             itemArea,
                                             clippedEnd,
                                             imageEnd)
                || imageStart == imageEnd)
            {
                cancelMeasurementLineDrawing();
                return;
            }
            const QString layerKey = m_measurementLineTargetLayerKey;
            cancelMeasurementLineDrawing();
            emit measurementLineDrawn(layerKey, imageStart, imageEnd);
            update();
            return;
        }

        if (m_crossSectionDrawingMode && event->button() == Qt::LeftButton && m_crossSectionDragging)
        {
            m_crossSectionDragging = false;
            m_crossSectionEnd = event->pos();

            PreviewInteractionTarget startTarget;
            const bool okStart = resolveInteractionTarget(m_crossSectionStart,
                                                          startTarget,
                                                          m_crossSectionTargetSourceId,
                                                          false,
                                                          m_crossSectionTargetLayerKey);
            FrameSourceState frameState;
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
                || !clipLineToRect(m_crossSectionStart, m_crossSectionEnd, displayRect, clippedStart, clippedEnd)
                || !mapWidgetPositionToImage(frameState, processed, itemArea, clippedStart, imgStart)
                || !mapWidgetPositionToImage(frameState, processed, itemArea, clippedEnd, imgEnd))
            {
                cancelCrossSectionDrawing();
                return;
            }

            m_crossSectionTargetLayerKey = startTarget.layerKey;
            m_crossSectionTargetSourceId = startTarget.sourceId;
            m_crossSectionStart = clippedStart;
            m_crossSectionEnd = clippedEnd;
            m_crossSectionDrawingMode = false;
            m_crossSectionDragging = false;
            unsetCursor();
            m_sceneModel->createLine(m_crossSectionTargetLayerKey,
                                     imgStart,
                                     imgEnd,
                                     QString(),
                                     ImageSceneModel::MarkupRole::CrossSection);
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
            FrameSourceState frameState;
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
                emit roiDrawn(startTarget.sourceId,
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

        if (m_markupDragging && event->button() == Qt::LeftButton)
        {
            m_markupDragging = false;
            m_dragMarkupId.clear();
            m_dragMarkupEditMode = MarkupEditMode::None;
            update();
            return;
        }

        QOpenGLWidget::mouseReleaseEvent(event);
    }

    // Clears the cursor readout after the pointer leaves the preview
    void PreviewWidget::leaveEvent(QEvent* event)
    {
        m_hoverWidgetPos = QPoint(-1, -1);
        emit mousePositionChanged(QPoint(-1, -1));
        update();
        QOpenGLWidget::leaveEvent(event);
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
            && !target.sourceId.isEmpty()
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
            FrameSourceState frameState;
            const QMap<QString, FrameSourceState> frameSources = snapshotFrameSources();
            const auto frameStateIt = frameSources.constFind(target.sourceId);
            const bool hasFrameState = frameStateIt != frameSources.constEnd();
            if (hasFrameState)
            {
                frameState = frameStateIt.value();
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
        if (m_measurementLineDrawingMode && event->key() == Qt::Key_Escape)
        {
            cancelMeasurementLineDrawing();
            event->accept();
            return;
        }

        if (m_crossSectionDrawingMode && event->key() == Qt::Key_Escape)
        {
            cancelCrossSectionDrawing();
            event->accept();
            return;
        }

        if (m_roiDrawingMode && event->key() == Qt::Key_Escape)
        {
            cancelROIDrawing();
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
        {
            clearSelectedMarkups();
            event->accept();
            return;
        }

        if (event->key() == Qt::Key_Escape)
        {
            m_sceneModel->selectOnly(QString());
            event->accept();
            return;
        }

        const bool bigStep = (event->modifiers() & Qt::ShiftModifier);
        if (event->key() == Qt::Key_Up)
        {
            emit stageStepRequested(0.0, 1.0, bigStep);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Down)
        {
            emit stageStepRequested(0.0, -1.0, bigStep);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Left)
        {
            emit stageStepRequested(-1.0, 0.0, bigStep);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_Right)
        {
            emit stageStepRequested(1.0, 0.0, bigStep);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageUp)
        {
            emit stageZStepRequested(1.0, bigStep);
            event->accept();
            return;
        }
        if (event->key() == Qt::Key_PageDown)
        {
            emit stageZStepRequested(-1.0, bigStep);
            event->accept();
            return;
        }

        QOpenGLWidget::keyPressEvent(event);
    }
} // namespace scopeone::ui
