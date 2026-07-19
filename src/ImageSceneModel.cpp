#include "ImageSceneModel.h"

#include <QtGlobal>

namespace scopeone::ui
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

        scopeone::core::DocumentMarkupType documentMarkupType(ImageSceneModel::MarkupType type)
        {
            return type == ImageSceneModel::MarkupType::Rect
                       ? scopeone::core::DocumentMarkupType::Rect
                       : scopeone::core::DocumentMarkupType::Line;
        }

        ImageSceneModel::MarkupType markupType(scopeone::core::DocumentMarkupType type)
        {
            return type == scopeone::core::DocumentMarkupType::Rect
                       ? ImageSceneModel::MarkupType::Rect
                       : ImageSceneModel::MarkupType::Line;
        }

        scopeone::core::DocumentMarkupRole documentMarkupRole(ImageSceneModel::MarkupRole role)
        {
            switch (role)
            {
            case ImageSceneModel::MarkupRole::CrossSection:
                return scopeone::core::DocumentMarkupRole::CrossSection;
            case ImageSceneModel::MarkupRole::Roi:
                return scopeone::core::DocumentMarkupRole::Roi;
            case ImageSceneModel::MarkupRole::Measurement:
                return scopeone::core::DocumentMarkupRole::Measurement;
            case ImageSceneModel::MarkupRole::Generic:
                return scopeone::core::DocumentMarkupRole::Generic;
            }
            return scopeone::core::DocumentMarkupRole::Generic;
        }

        ImageSceneModel::MarkupRole markupRole(scopeone::core::DocumentMarkupRole role)
        {
            switch (role)
            {
            case scopeone::core::DocumentMarkupRole::CrossSection:
                return ImageSceneModel::MarkupRole::CrossSection;
            case scopeone::core::DocumentMarkupRole::Roi:
                return ImageSceneModel::MarkupRole::Roi;
            case scopeone::core::DocumentMarkupRole::Measurement:
                return ImageSceneModel::MarkupRole::Measurement;
            case scopeone::core::DocumentMarkupRole::Generic:
                return ImageSceneModel::MarkupRole::Generic;
            }
            return ImageSceneModel::MarkupRole::Generic;
        }

        ImageSceneModel::LayerKind sceneLayerKind(scopeone::core::DocumentLayerKind kind)
        {
            switch (kind)
            {
            case scopeone::core::DocumentLayerKind::Raw:
                return ImageSceneModel::LayerKind::Raw;
            case scopeone::core::DocumentLayerKind::Processed:
                return ImageSceneModel::LayerKind::Processed;
            case scopeone::core::DocumentLayerKind::Gallery:
                return ImageSceneModel::LayerKind::Gallery;
            case scopeone::core::DocumentLayerKind::Static:
                return ImageSceneModel::LayerKind::Static;
            }
            return ImageSceneModel::LayerKind::Unknown;
        }

        ImageSceneModel::Markup markupFromDocument(
            const scopeone::core::DocumentMarkup& documentMarkup,
            const ImageSceneModel& sceneModel)
        {
            ImageSceneModel::Markup markup;
            markup.id = documentMarkup.id;
            markup.type = markupType(documentMarkup.type);
            markup.role = markupRole(documentMarkup.role);
            markup.layerKey = documentMarkup.layerId;
            scopeone::core::DocumentLayer layer;
            if (sceneModel.findLayer(markup.layerKey, layer))
            {
                markup.layerKind = sceneLayerKind(layer.kind);
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
        m_document = document;
        m_nextMarkupId = 1;
        emit documentReplaced();
        emit layersChanged();
        emit markupsChanged();
        return true;
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
        emit layersChanged();
        return true;
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
        if (layer.width == frame.width
            && layer.height == frame.height
            && transformsEqual(layer.pixelToSensor, pixelToSensor))
        {
            return true;
        }
        layer.width = frame.width;
        layer.height = frame.height;
        layer.pixelToSensor = pixelToSensor;
        emit layersChanged();
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
        m_document.layers.removeAt(index);
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

    void ImageSceneModel::removeLayersNotIn(const QSet<QString>& validLayerIds)
    {
        QSet<QString> removedLayerIds;
        for (int index = m_document.layers.size() - 1; index >= 0; --index)
        {
            const QString layerId = m_document.layers.at(index).id;
            if (!validLayerIds.contains(layerId))
            {
                removedLayerIds.insert(layerId);
                m_document.layers.removeAt(index);
            }
        }
        if (removedLayerIds.isEmpty())
        {
            return;
        }
        bool removedMarkup = false;
        for (int index = m_document.markups.size() - 1; index >= 0; --index)
        {
            if (removedLayerIds.contains(m_document.markups.at(index).layerId))
            {
                m_document.markups.removeAt(index);
                removedMarkup = true;
            }
        }
        emit layersChanged();
        if (removedMarkup)
        {
            emit markupsChanged();
        }
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
        documentMarkup.type = documentMarkupType(markup.type);
        documentMarkup.role = documentMarkupRole(markup.role);
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

    bool ImageSceneModel::hasRole(MarkupRole role, const QString& layerKey) const
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        const scopeone::core::DocumentMarkupRole requestedRole = documentMarkupRole(role);
        for (const scopeone::core::DocumentMarkup& markup : m_document.markups)
        {
            if (markup.role == requestedRole
                && (trimmedLayerKey.isEmpty() || markup.layerId == trimmedLayerKey))
            {
                return true;
            }
        }
        return false;
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
        const scopeone::core::DocumentMarkupRole requestedRole = documentMarkupRole(role);
        bool removed = false;
        for (int index = m_document.markups.size() - 1; index >= 0; --index)
        {
            const scopeone::core::DocumentMarkup& markup = m_document.markups.at(index);
            if (markup.role == requestedRole
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
        switch (type)
        {
        case MarkupType::Line:
            return QStringLiteral("line");
        case MarkupType::Rect:
            return QStringLiteral("rect");
        }
        return {};
    }

    QString ImageSceneModel::roleName(MarkupRole role)
    {
        switch (role)
        {
        case MarkupRole::Generic:
            return QStringLiteral("generic");
        case MarkupRole::CrossSection:
            return QStringLiteral("cross_section");
        case MarkupRole::Roi:
            return QStringLiteral("roi");
        case MarkupRole::Measurement:
            return QStringLiteral("measurement");
        }
        return {};
    }

    QString ImageSceneModel::layerKindName(LayerKind layerKind)
    {
        switch (layerKind)
        {
        case LayerKind::Raw:
            return QStringLiteral("raw");
        case LayerKind::Processed:
            return QStringLiteral("processed");
        case LayerKind::Static:
            return QStringLiteral("static");
        case LayerKind::Gallery:
            return QStringLiteral("gallery");
        case LayerKind::Unknown:
            return QStringLiteral("unknown");
        }
        return QStringLiteral("unknown");
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
} // namespace scopeone::ui
