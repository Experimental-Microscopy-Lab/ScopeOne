#include "scopeone/ImageSceneModel.h"

#include <algorithm>
#include <QtGlobal>

namespace scopeone::core
{
    namespace
    {
        bool transformsEqual(const scopeone::core::ImageTransform2D& left,
                             const scopeone::core::ImageTransform2D& right)
        {
            return qFuzzyCompare(left.m11, right.m11)
                && qFuzzyCompare(left.m12, right.m12)
                && qFuzzyCompare(left.m21, right.m21)
                && qFuzzyCompare(left.m22, right.m22)
                && qFuzzyCompare(left.dx, right.dx)
                && qFuzzyCompare(left.dy, right.dy);
        }

        bool displaysEqual(const scopeone::core::LayerDisplayState& left,
                           const scopeone::core::LayerDisplayState& right)
        {
            return left.visible == right.visible
                && left.opacityPercent == right.opacityPercent
                && qFuzzyCompare(left.gamma, right.gamma)
                && left.colormap == right.colormap
                && left.blending == right.blending
                && left.levelMin == right.levelMin
                && left.levelMax == right.levelMax
                && left.levelDomainMax == right.levelDomainMax;
        }

        QString canonicalName(const QString& value, const QStringList& supportedValues)
        {
            const QString trimmedValue = value.trimmed();
            for (const QString& supportedValue : supportedValues)
            {
                if (supportedValue.compare(trimmedValue, Qt::CaseInsensitive) == 0)
                {
                    return supportedValue;
                }
            }
            return {};
        }

        bool sourceTransformsEqual(const ImageSceneModel::SourceDisplayTransform& left,
                                   const ImageSceneModel::SourceDisplayTransform& right)
        {
            return left.offsetX == right.offsetX
                && left.offsetY == right.offsetY
                && left.flipX == right.flipX
                && left.flipY == right.flipY
                && left.zoomPercent == right.zoomPercent;
        }

        ImageSceneModel::Markup markupFromDocument(
            const scopeone::core::DocumentMarkup& documentMarkup,
            const ImageSceneModel& sceneModel)
        {
            ImageSceneModel::Markup markup;
            markup.id = documentMarkup.id;
            markup.type = documentMarkup.type;
            markup.role = documentMarkup.role;
            markup.layerKey = documentMarkup.layerId;
            scopeone::core::DocumentLayer layer;
            if (sceneModel.findLayer(markup.layerKey, layer))
            {
                markup.layerKind = layer.kind;
                markup.sourceId = layer.sourceId;
            }
            markup.start = documentMarkup.start.toPoint();
            markup.end = documentMarkup.end.toPoint();
            markup.rect = documentMarkup.rect.toRect();
            markup.label = documentMarkup.label;
            markup.visible = documentMarkup.visible;
            markup.selected = documentMarkup.selected;
            return markup;
        }
    }

    ImageSceneModel::ImageSceneModel(QObject* parent)
        : QObject(parent)
    {
    }

    void ImageSceneModel::reset()
    {
        m_document = ExperimentDocument{};
        m_sourceDisplayTransforms.clear();
        m_autoStretchLayerIds.clear();
        m_nextMarkupId = 1;
        emit layersChanged();
        emit markupsChanged();
    }

    const scopeone::core::ExperimentDocument& ImageSceneModel::document() const
    {
        return m_document;
    }

    bool ImageSceneModel::setDocument(const scopeone::core::ExperimentDocument& document,
                                      QString* errorMessage)
    {
        if (!scopeone::core::validateExperimentDocument(document, errorMessage))
        {
            return false;
        }
        applyValidatedDocument(document);
        return true;
    }

    // Applies a trusted document without repeating schema validation
    void ImageSceneModel::applyValidatedDocument(
        const scopeone::core::ExperimentDocument& document)
    {
        m_document = document;
        m_sourceDisplayTransforms.clear();
        m_autoStretchLayerIds.clear();
        m_nextMarkupId = 1;
        emit layersChanged();
        emit markupsChanged();
    }

    QStringList ImageSceneModel::layerIds() const
    {
        QStringList ids;
        ids.reserve(m_document.layers.size());
        for (const scopeone::core::DocumentLayer& layer : m_document.layers)
        {
            ids.append(layer.id);
        }
        return ids;
    }

    QStringList ImageSceneModel::visibleLayerIds() const
    {
        QStringList ids;
        for (const scopeone::core::DocumentLayer& layer : m_document.layers)
        {
            if (layer.display.visible)
            {
                ids.append(layer.id);
            }
        }
        return ids;
    }

    bool ImageSceneModel::hasSource(const QString& sourceId) const
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (trimmedSourceId.isEmpty())
        {
            return false;
        }
        return std::any_of(m_document.layers.cbegin(), m_document.layers.cend(),
                           [&trimmedSourceId](const scopeone::core::DocumentLayer& layer)
                           {
                               return layer.sourceId == trimmedSourceId;
                           });
    }

    bool ImageSceneModel::findLayer(const QString& layerId,
                                    scopeone::core::DocumentLayer& outLayer) const
    {
        const int index = layerIndex(layerId);
        if (index < 0)
        {
            return false;
        }
        outLayer = m_document.layers.at(index);
        return true;
    }

    bool ImageSceneModel::ensureLayer(const scopeone::core::DocumentLayer& layer)
    {
        scopeone::core::DocumentLayer normalizedLayer = layer;
        normalizedLayer.id = normalizedLayer.id.trimmed();
        normalizedLayer.sourceId = normalizedLayer.sourceId.trimmed();
        normalizedLayer.name = normalizedLayer.name.trimmed();
        if (normalizedLayer.id.isEmpty()
            || normalizedLayer.sourceId.isEmpty()
            || normalizedLayer.name.isEmpty()
            || layerIndex(normalizedLayer.id) >= 0)
        {
            return false;
        }
        m_document.layers.append(normalizedLayer);
        emit layersChanged();
        return true;
    }

    bool ImageSceneModel::setLayerName(const QString& layerId, const QString& name)
    {
        const int index = layerIndex(layerId);
        const QString trimmedName = name.trimmed();
        if (index < 0 || trimmedName.isEmpty())
        {
            return false;
        }
        scopeone::core::DocumentLayer& layer = m_document.layers[index];
        if (layer.name == trimmedName)
        {
            return true;
        }
        layer.name = trimmedName;
        emit layersChanged();
        return true;
    }

    bool ImageSceneModel::setLayerDisplay(
        const QString& layerId,
        const scopeone::core::LayerDisplayState& display)
    {
        const int index = layerIndex(layerId);
        if (index < 0)
        {
            return false;
        }
        scopeone::core::DocumentLayer& layer = m_document.layers[index];
        if (displaysEqual(layer.display, display))
        {
            return true;
        }
        layer.display = display;
        emit layerDisplayChanged(layer.id);
        return true;
    }

    bool ImageSceneModel::setVisibleLayers(const QStringList& layerIds)
    {
        QSet<QString> visibleIds;
        for (const QString& layerId : layerIds)
        {
            const QString trimmedLayerId = layerId.trimmed();
            if (layerIndex(trimmedLayerId) < 0)
            {
                return false;
            }
            visibleIds.insert(trimmedLayerId);
        }

        QStringList changedIds;
        for (scopeone::core::DocumentLayer& layer : m_document.layers)
        {
            const bool visible = visibleIds.contains(layer.id);
            if (layer.display.visible != visible)
            {
                layer.display.visible = visible;
                changedIds.append(layer.id);
            }
        }
        if (changedIds.isEmpty())
        {
            return true;
        }
        for (const QString& layerId : changedIds)
        {
            emit layerDisplayChanged(layerId);
        }
        return true;
    }

    bool ImageSceneModel::setLayerVisible(const QString& layerId, bool visible)
    {
        const int index = layerIndex(layerId);
        if (index < 0)
        {
            return false;
        }
        scopeone::core::LayerDisplayState display = m_document.layers.at(index).display;
        display.visible = visible;
        return setLayerDisplay(layerId, display);
    }

    bool ImageSceneModel::setLayerOpacityPercent(const QString& layerId, int percent)
    {
        const int index = layerIndex(layerId);
        if (index < 0)
        {
            return false;
        }
        scopeone::core::LayerDisplayState display = m_document.layers.at(index).display;
        display.opacityPercent = qBound(0, percent, 100);
        return setLayerDisplay(layerId, display);
    }

    bool ImageSceneModel::setLayerGamma(const QString& layerId, double gamma)
    {
        const int index = layerIndex(layerId);
        if (index < 0)
        {
            return false;
        }
        scopeone::core::LayerDisplayState display = m_document.layers.at(index).display;
        display.gamma = std::clamp(gamma, 0.2, 2.0);
        return setLayerDisplay(layerId, display);
    }

    bool ImageSceneModel::setLayerColormap(const QString& layerId, const QString& colormap)
    {
        const int index = layerIndex(layerId);
        const QString name = canonicalName(colormap, supportedColormaps());
        if (index < 0 || name.isEmpty())
        {
            return false;
        }
        scopeone::core::LayerDisplayState display = m_document.layers.at(index).display;
        display.colormap = name;
        return setLayerDisplay(layerId, display);
    }

    bool ImageSceneModel::setLayerBlending(const QString& layerId, const QString& blending)
    {
        const int index = layerIndex(layerId);
        const QString name = canonicalName(blending, supportedBlendingModes());
        if (index < 0 || name.isEmpty())
        {
            return false;
        }
        scopeone::core::LayerDisplayState display = m_document.layers.at(index).display;
        display.blending = name;
        return setLayerDisplay(layerId, display);
    }

    bool ImageSceneModel::setLayerDisplayLevels(const QString& layerId,
                                                int minLevel,
                                                int maxLevel,
                                                int domainMax)
    {
        const int index = layerIndex(layerId);
        if (index < 0)
        {
            return false;
        }
        scopeone::core::LayerDisplayState display = m_document.layers.at(index).display;
        display.levelDomainMax = qMax(1, domainMax);
        display.levelMin = qBound(0, minLevel, display.levelDomainMax - 1);
        display.levelMax = qBound(display.levelMin + 1, maxLevel, display.levelDomainMax);
        return setLayerDisplay(layerId, display);
    }

    bool ImageSceneModel::updateLayerFrame(const QString& layerId,
                                           const scopeone::core::ImageFrame& frame)
    {
        const int index = layerIndex(layerId);
        if (index < 0 || !frame.isValid())
        {
            return false;
        }
        scopeone::core::DocumentLayer& layer = m_document.layers[index];
        const scopeone::core::ImageTransform2D pixelToSensor =
            scopeone::core::imagePixelToSensorTransform(frame);
        const bool metadataChanged = layer.width != frame.width
                                     || layer.height != frame.height
                                     || !transformsEqual(layer.pixelToSensor, pixelToSensor);
        const int domainMax = qMax(1, frame.maxValue());
        const bool displayChanged = layer.display.levelDomainMax != domainMax;
        if (!metadataChanged && !displayChanged)
        {
            return true;
        }
        layer.width = frame.width;
        layer.height = frame.height;
        layer.pixelToSensor = pixelToSensor;
        if (displayChanged)
        {
            const int previousDomainMax = qMax(1, layer.display.levelDomainMax);
            const bool usedFullRange = layer.display.levelMin == 0
                                       && layer.display.levelMax == previousDomainMax;
            layer.display.levelDomainMax = domainMax;
            if (usedFullRange)
            {
                layer.display.levelMin = 0;
                layer.display.levelMax = domainMax;
            }
            else
            {
                layer.display.levelMin = qBound(0, layer.display.levelMin, domainMax - 1);
                layer.display.levelMax = qBound(layer.display.levelMin + 1,
                                                layer.display.levelMax,
                                                domainMax);
            }
        }
        if (metadataChanged)
        {
            emit layersChanged();
        }
        if (displayChanged)
        {
            emit layerDisplayChanged(layer.id);
        }
        return true;
    }

    bool ImageSceneModel::moveLayer(const QString& layerId, int offset)
    {
        const int currentIndex = layerIndex(layerId);
        if (currentIndex < 0 || offset == 0)
        {
            return false;
        }
        const int nextIndex = qBound(0,
                                     currentIndex + offset,
                                     static_cast<int>(m_document.layers.size()) - 1);
        if (currentIndex == nextIndex)
        {
            return false;
        }
        m_document.layers.move(currentIndex, nextIndex);
        emit layersChanged();
        return true;
    }

    bool ImageSceneModel::removeLayer(const QString& layerId)
    {
        const QString trimmedLayerId = layerId.trimmed();
        const int index = layerIndex(trimmedLayerId);
        if (index < 0)
        {
            return false;
        }
        const QString sourceId = m_document.layers.at(index).sourceId;
        m_autoStretchLayerIds.remove(trimmedLayerId);
        m_document.layers.removeAt(index);
        if (!hasSource(sourceId))
        {
            m_sourceDisplayTransforms.remove(sourceId);
        }
        bool removedMarkup = false;
        for (int markupIndex = m_document.markups.size() - 1; markupIndex >= 0; --markupIndex)
        {
            if (m_document.markups.at(markupIndex).layerId == trimmedLayerId)
            {
                m_document.markups.removeAt(markupIndex);
                removedMarkup = true;
            }
        }
        emit layersChanged();
        if (removedMarkup)
        {
            emit markupsChanged();
        }
        return true;
    }

    bool ImageSceneModel::layerAutoStretchEnabled(const QString& layerId) const
    {
        return m_autoStretchLayerIds.contains(layerId.trimmed());
    }

    bool ImageSceneModel::setLayerAutoStretchEnabled(const QString& layerId, bool enabled)
    {
        const QString trimmedLayerId = layerId.trimmed();
        if (layerIndex(trimmedLayerId) < 0)
        {
            return false;
        }
        const bool current = m_autoStretchLayerIds.contains(trimmedLayerId);
        if (current == enabled)
        {
            return true;
        }
        if (enabled)
        {
            m_autoStretchLayerIds.insert(trimmedLayerId);
        }
        else
        {
            m_autoStretchLayerIds.remove(trimmedLayerId);
        }
        emit layerAutoStretchChanged(trimmedLayerId, enabled);
        return true;
    }

    ImageSceneModel::SourceDisplayTransform ImageSceneModel::sourceDisplayTransform(
        const QString& sourceId) const
    {
        return m_sourceDisplayTransforms.value(sourceId.trimmed());
    }

    bool ImageSceneModel::setSourceDisplayTransform(
        const QString& sourceId,
        const SourceDisplayTransform& transform)
    {
        const QString trimmedSourceId = sourceId.trimmed();
        if (!hasSource(trimmedSourceId))
        {
            return false;
        }
        SourceDisplayTransform normalized = transform;
        normalized.zoomPercent = qBound(10, normalized.zoomPercent, 500);
        const SourceDisplayTransform current = m_sourceDisplayTransforms.value(trimmedSourceId);
        if (sourceTransformsEqual(current, normalized))
        {
            return true;
        }
        m_sourceDisplayTransforms.insert(trimmedSourceId, normalized);
        emit sourceDisplayTransformChanged(trimmedSourceId);
        return true;
    }

    bool ImageSceneModel::resetSourceDisplayTransform(const QString& sourceId)
    {
        return setSourceDisplayTransform(sourceId, SourceDisplayTransform{});
    }

    QStringList ImageSceneModel::supportedColormaps()
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
            QStringLiteral("Inverted Gray"),
            QStringLiteral("Ice"),
            QStringLiteral("Viridis"),
            QStringLiteral("Inferno"),
            QStringLiteral("Magma"),
            QStringLiteral("Cividis"),
            QStringLiteral("Jet"),
            QStringLiteral("Rainbow"),
            QStringLiteral("Turbo"),
        };
    }

    QStringList ImageSceneModel::supportedBlendingModes()
    {
        return {
            QStringLiteral("Translucent"),
            QStringLiteral("Additive"),
            QStringLiteral("Minimum"),
            QStringLiteral("Opaque"),
            QStringLiteral("Multiplicative"),
        };
    }

    QString ImageSceneModel::createLine(const QString& layerKey,
                                        const QPoint& start,
                                        const QPoint& end,
                                        const QString& label,
                                        MarkupRole role)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty() || start == end)
        {
            return {};
        }
        Markup markup;
        markup.type = MarkupType::Line;
        markup.role = role;
        markup.layerKey = trimmedLayerKey;
        markup.start = start;
        markup.end = end;
        markup.label = label.trimmed();
        return addMarkup(markup);
    }

    QString ImageSceneModel::createRect(const QString& layerKey,
                                        const QRect& rect,
                                        const QString& label,
                                        MarkupRole role)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        const QRect normalizedRect = rect.normalized();
        if (trimmedLayerKey.isEmpty()
            || normalizedRect.width() <= 0
            || normalizedRect.height() <= 0)
        {
            return {};
        }
        Markup markup;
        markup.type = MarkupType::Rect;
        markup.role = role;
        markup.layerKey = trimmedLayerKey;
        markup.rect = normalizedRect;
        markup.label = label.trimmed();
        return addMarkup(markup);
    }

    QString ImageSceneModel::addMarkup(Markup markup)
    {
        scopeone::core::DocumentMarkup documentMarkup;
        documentMarkup.id = markup.id.trimmed();
        documentMarkup.type = markup.type;
        documentMarkup.role = markup.role;
        documentMarkup.layerId = markup.layerKey.trimmed();
        documentMarkup.label = markup.label.trimmed();
        documentMarkup.start = markup.start;
        documentMarkup.end = markup.end;
        documentMarkup.rect = markup.rect;
        documentMarkup.visible = markup.visible;
        documentMarkup.selected = markup.selected;
        if (documentMarkup.layerId.isEmpty() || layerIndex(documentMarkup.layerId) < 0)
        {
            return {};
        }
        if (documentMarkup.id.isEmpty())
        {
            do
            {
                documentMarkup.id = QStringLiteral("markup_%1").arg(m_nextMarkupId++);
            }
            while (markupIndex(documentMarkup.id) >= 0);
        }
        else if (markupIndex(documentMarkup.id) >= 0)
        {
            return {};
        }
        if (documentMarkup.role == DocumentMarkupRole::CrossSection)
        {
            for (int index = m_document.markups.size() - 1; index >= 0; --index)
            {
                if (m_document.markups.at(index).role == DocumentMarkupRole::CrossSection)
                {
                    m_document.markups.removeAt(index);
                }
            }
        }
        m_document.markups.append(documentMarkup);
        emit markupsChanged();
        return documentMarkup.id;
    }

    QList<ImageSceneModel::Markup> ImageSceneModel::markups(const QString& layerKey) const
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        QList<Markup> items;
        items.reserve(m_document.markups.size());
        for (const scopeone::core::DocumentMarkup& markup : m_document.markups)
        {
            if (trimmedLayerKey.isEmpty() || markup.layerId == trimmedLayerKey)
            {
                items.append(markupFromDocument(markup, *this));
            }
        }
        return items;
    }

    bool ImageSceneModel::hasMarkups() const
    {
        return !m_document.markups.isEmpty();
    }

    bool ImageSceneModel::findMarkup(const QString& id, Markup& outMarkup) const
    {
        const int index = markupIndex(id);
        if (index < 0)
        {
            return false;
        }
        outMarkup = markupFromDocument(m_document.markups.at(index), *this);
        return true;
    }

    bool ImageSceneModel::setLabel(const QString& id, const QString& label)
    {
        const int index = markupIndex(id);
        if (index < 0)
        {
            return false;
        }
        const QString trimmedLabel = label.trimmed();
        if (m_document.markups[index].label == trimmedLabel)
        {
            return true;
        }
        m_document.markups[index].label = trimmedLabel;
        emit markupsChanged();
        return true;
    }

    bool ImageSceneModel::setVisible(const QString& id, bool visible)
    {
        const int index = markupIndex(id);
        if (index < 0)
        {
            return false;
        }
        if (m_document.markups[index].visible == visible)
        {
            return true;
        }
        m_document.markups[index].visible = visible;
        emit markupsChanged();
        return true;
    }

    bool ImageSceneModel::setSelected(const QString& id, bool selected)
    {
        const int index = markupIndex(id);
        if (index < 0)
        {
            return false;
        }
        if (m_document.markups[index].selected == selected)
        {
            return true;
        }
        m_document.markups[index].selected = selected;
        emit markupsChanged();
        return true;
    }

    bool ImageSceneModel::selectOnly(const QString& id)
    {
        const QString trimmedId = id.trimmed();
        if (!trimmedId.isEmpty() && markupIndex(trimmedId) < 0)
        {
            return false;
        }
        bool changed = false;
        for (scopeone::core::DocumentMarkup& markup : m_document.markups)
        {
            const bool selected = !trimmedId.isEmpty() && markup.id == trimmedId;
            if (markup.selected != selected)
            {
                markup.selected = selected;
                changed = true;
            }
        }
        if (changed)
        {
            emit markupsChanged();
        }
        return true;
    }

    bool ImageSceneModel::updateLine(const QString& id,
                                     const QPoint& start,
                                     const QPoint& end)
    {
        const int index = markupIndex(id);
        if (index < 0
            || m_document.markups.at(index).type != scopeone::core::DocumentMarkupType::Line
            || start == end)
        {
            return false;
        }
        scopeone::core::DocumentMarkup& markup = m_document.markups[index];
        if (markup.start == start && markup.end == end)
        {
            return true;
        }
        markup.start = start;
        markup.end = end;
        emit markupsChanged();
        return true;
    }

    bool ImageSceneModel::updateRect(const QString& id, const QRect& rect)
    {
        const int index = markupIndex(id);
        const QRect normalizedRect = rect.normalized();
        if (index < 0
            || m_document.markups.at(index).type != scopeone::core::DocumentMarkupType::Rect
            || normalizedRect.width() <= 0
            || normalizedRect.height() <= 0)
        {
            return false;
        }
        scopeone::core::DocumentMarkup& markup = m_document.markups[index];
        if (markup.rect == normalizedRect)
        {
            return true;
        }
        markup.rect = normalizedRect;
        emit markupsChanged();
        return true;
    }

    bool ImageSceneModel::remove(const QString& id)
    {
        const int index = markupIndex(id);
        if (index < 0)
        {
            return false;
        }
        m_document.markups.removeAt(index);
        emit markupsChanged();
        return true;
    }

    void ImageSceneModel::clear(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        bool removed = false;
        for (int index = m_document.markups.size() - 1; index >= 0; --index)
        {
            if (trimmedLayerKey.isEmpty()
                || m_document.markups.at(index).layerId == trimmedLayerKey)
            {
                m_document.markups.removeAt(index);
                removed = true;
            }
        }
        if (removed)
        {
            emit markupsChanged();
        }
    }

    void ImageSceneModel::clearRole(MarkupRole role, const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        bool removed = false;
        for (int index = m_document.markups.size() - 1; index >= 0; --index)
        {
            const scopeone::core::DocumentMarkup& markup = m_document.markups.at(index);
            if (markup.role == role
                && (trimmedLayerKey.isEmpty() || markup.layerId == trimmedLayerKey))
            {
                m_document.markups.removeAt(index);
                removed = true;
            }
        }
        if (removed)
        {
            emit markupsChanged();
        }
    }

    QString ImageSceneModel::typeName(MarkupType type)
    {
        return documentMarkupTypeName(type);
    }

    QString ImageSceneModel::roleName(MarkupRole role)
    {
        return documentMarkupRoleName(role);
    }

    QString ImageSceneModel::layerKindName(LayerKind layerKind)
    {
        return documentLayerKindName(layerKind);
    }

    int ImageSceneModel::layerIndex(const QString& layerId) const
    {
        const QString trimmedLayerId = layerId.trimmed();
        for (int index = 0; index < m_document.layers.size(); ++index)
        {
            if (m_document.layers.at(index).id == trimmedLayerId)
            {
                return index;
            }
        }
        return -1;
    }

    int ImageSceneModel::markupIndex(const QString& markupId) const
    {
        const QString trimmedMarkupId = markupId.trimmed();
        for (int index = 0; index < m_document.markups.size(); ++index)
        {
            if (m_document.markups.at(index).id == trimmedMarkupId)
            {
                return index;
            }
        }
        return -1;
    }
} // namespace scopeone::core
