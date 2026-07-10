#include "ImageMarkupModel.h"

#include "scopeone/ScopeOneCore.h"

namespace scopeone::ui
{
    ImageMarkupModel::ImageMarkupModel(QObject* parent)
        : QObject(parent)
    {
    }

    // Adds one line markup in image coordinates
    QString ImageMarkupModel::createLine(const QString& layerKey,
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

    // Adds one rectangle markup in image coordinates
    QString ImageMarkupModel::createRect(const QString& layerKey,
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

    QString ImageMarkupModel::addMarkup(Markup markup)
    {
        markup.layerKey = markup.layerKey.trimmed();
        if (markup.layerKey.isEmpty())
        {
            return {};
        }

        markup.id = QStringLiteral("markup_%1").arg(m_nextMarkupId++);
        markup.layerKind = layerKindForLayerKey(markup.layerKey);
        markup.sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(markup.layerKey);
        m_markupOrder.append(markup.id);
        m_markups.insert(markup.id, markup);
        emit changed();
        return markup.id;
    }

    QList<ImageMarkupModel::Markup> ImageMarkupModel::markups(const QString& layerKey) const
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        QList<Markup> items;
        items.reserve(m_markupOrder.size());
        for (const QString& id : m_markupOrder)
        {
            const auto it = m_markups.constFind(id);
            if (it == m_markups.constEnd())
            {
                continue;
            }
            if (!trimmedLayerKey.isEmpty() && it.value().layerKey != trimmedLayerKey)
            {
                continue;
            }
            items.append(it.value());
        }
        return items;
    }

    bool ImageMarkupModel::hasMarkups() const
    {
        return !m_markups.isEmpty();
    }

    bool ImageMarkupModel::findMarkup(const QString& id, Markup& outMarkup) const
    {
        const auto it = m_markups.constFind(id.trimmed());
        if (it == m_markups.constEnd())
        {
            return false;
        }

        outMarkup = it.value();
        return true;
    }

    bool ImageMarkupModel::hasRole(MarkupRole role, const QString& layerKey) const
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        for (const QString& id : m_markupOrder)
        {
            const auto it = m_markups.constFind(id);
            if (it == m_markups.constEnd() || it.value().role != role)
            {
                continue;
            }
            if (trimmedLayerKey.isEmpty() || it.value().layerKey == trimmedLayerKey)
            {
                return true;
            }
        }
        return false;
    }

    bool ImageMarkupModel::setLabel(const QString& id, const QString& label)
    {
        auto it = m_markups.find(id.trimmed());
        if (it == m_markups.end())
        {
            return false;
        }

        const QString trimmedLabel = label.trimmed();
        if (it.value().label == trimmedLabel)
        {
            return true;
        }
        it.value().label = trimmedLabel;
        emit changed();
        return true;
    }

    bool ImageMarkupModel::setVisible(const QString& id, bool visible)
    {
        auto it = m_markups.find(id.trimmed());
        if (it == m_markups.end())
        {
            return false;
        }

        if (it.value().visible == visible)
        {
            return true;
        }
        it.value().visible = visible;
        emit changed();
        return true;
    }

    bool ImageMarkupModel::setSelected(const QString& id, bool selected)
    {
        auto it = m_markups.find(id.trimmed());
        if (it == m_markups.end())
        {
            return false;
        }

        if (it.value().selected == selected)
        {
            return true;
        }
        it.value().selected = selected;
        emit changed();
        return true;
    }

    bool ImageMarkupModel::selectOnly(const QString& id)
    {
        const QString trimmedId = id.trimmed();
        if (!trimmedId.isEmpty() && !m_markups.contains(trimmedId))
        {
            return false;
        }

        bool selectionChanged = false;
        for (auto it = m_markups.begin(); it != m_markups.end(); ++it)
        {
            const bool selected = !trimmedId.isEmpty() && it.key() == trimmedId;
            if (it.value().selected != selected)
            {
                it.value().selected = selected;
                selectionChanged = true;
            }
        }
        if (selectionChanged)
        {
            emit changed();
        }
        return true;
    }

    bool ImageMarkupModel::updateLine(const QString& id, const QPoint& start, const QPoint& end)
    {
        auto it = m_markups.find(id.trimmed());
        if (it == m_markups.end() || it.value().type != MarkupType::Line || start == end)
        {
            return false;
        }

        if (it.value().start == start && it.value().end == end)
        {
            return true;
        }
        it.value().start = start;
        it.value().end = end;
        emit changed();
        return true;
    }

    bool ImageMarkupModel::updateRect(const QString& id, const QRect& rect)
    {
        auto it = m_markups.find(id.trimmed());
        const QRect normalizedRect = rect.normalized();
        if (it == m_markups.end()
            || it.value().type != MarkupType::Rect
            || normalizedRect.width() <= 0
            || normalizedRect.height() <= 0)
        {
            return false;
        }

        if (it.value().rect == normalizedRect)
        {
            return true;
        }
        it.value().rect = normalizedRect;
        emit changed();
        return true;
    }

    QString ImageMarkupModel::typeName(MarkupType type)
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

    QString ImageMarkupModel::roleName(MarkupRole role)
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

    QString ImageMarkupModel::layerKindName(LayerKind layerKind)
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

    ImageMarkupModel::LayerKind ImageMarkupModel::layerKindForLayerKey(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (scopeone::core::ScopeOneCore::isRawLayerKey(trimmedLayerKey))
        {
            return LayerKind::Raw;
        }
        if (scopeone::core::ScopeOneCore::isProcessedLayerKey(trimmedLayerKey))
        {
            return LayerKind::Processed;
        }
        if (scopeone::core::ScopeOneCore::isStaticLayerKey(trimmedLayerKey))
        {
            const QString sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(trimmedLayerKey);
            return sourceId.startsWith(QStringLiteral("gallery:")) ? LayerKind::Gallery : LayerKind::Static;
        }
        return LayerKind::Unknown;
    }

    bool ImageMarkupModel::remove(const QString& id)
    {
        const QString trimmedId = id.trimmed();
        const auto it = m_markups.constFind(trimmedId);
        if (it == m_markups.constEnd())
        {
            return false;
        }

        m_markups.remove(trimmedId);
        m_markupOrder.removeAll(trimmedId);
        emit changed();
        return true;
    }

    void ImageMarkupModel::clear(const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        if (trimmedLayerKey.isEmpty())
        {
            if (m_markups.isEmpty())
            {
                return;
            }
            m_markups.clear();
            m_markupOrder.clear();
            emit changed();
            return;
        }

        bool anyRemoved = false;
        for (int i = m_markupOrder.size() - 1; i >= 0; --i)
        {
            const QString id = m_markupOrder.at(i);
            const auto it = m_markups.constFind(id);
            if (it != m_markups.constEnd() && it.value().layerKey == trimmedLayerKey)
            {
                m_markups.remove(id);
                m_markupOrder.removeAt(i);
                anyRemoved = true;
            }
        }
        if (anyRemoved)
        {
            emit changed();
        }
    }

    void ImageMarkupModel::clearRole(MarkupRole role, const QString& layerKey)
    {
        const QString trimmedLayerKey = layerKey.trimmed();
        bool anyRemoved = false;
        for (int i = m_markupOrder.size() - 1; i >= 0; --i)
        {
            const QString id = m_markupOrder.at(i);
            const auto it = m_markups.constFind(id);
            if (it == m_markups.constEnd() || it.value().role != role)
            {
                continue;
            }
            if (!trimmedLayerKey.isEmpty() && it.value().layerKey != trimmedLayerKey)
            {
                continue;
            }
            m_markups.remove(id);
            m_markupOrder.removeAt(i);
            anyRemoved = true;
        }

        if (anyRemoved)
        {
            emit changed();
        }
    }
} // namespace scopeone::ui
