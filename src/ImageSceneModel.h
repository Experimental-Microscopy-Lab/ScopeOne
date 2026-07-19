#pragma once

#include "scopeone/ExperimentDocument.h"

#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>

namespace scopeone::ui
{
    class ImageSceneModel : public QObject
    {
        Q_OBJECT

    public:
        enum class MarkupType
        {
            Line,
            Rect,
        };

        enum class MarkupRole
        {
            Generic,
            CrossSection,
            Roi,
            Measurement,
        };

        enum class LayerKind
        {
            Unknown,
            Raw,
            Processed,
            Static,
            Gallery,
        };

        struct Markup
        {
            QString id;
            MarkupType type{MarkupType::Line};
            MarkupRole role{MarkupRole::Generic};
            LayerKind layerKind{LayerKind::Unknown};
            QString layerKey;
            QString sourceId;
            QPoint start;
            QPoint end;
            QRect rect;
            QString label;
            bool visible{true};
            bool selected{false};
        };

        explicit ImageSceneModel(QObject* parent = nullptr);

        const scopeone::core::ExperimentDocument& document() const;
        bool setDocument(const scopeone::core::ExperimentDocument& document,
                         QString* errorMessage = nullptr);

        QStringList layerIds() const;
        bool findLayer(const QString& layerId,
                       scopeone::core::DocumentLayer& outLayer) const;
        bool ensureLayer(const scopeone::core::DocumentLayer& layer);
        bool setLayerName(const QString& layerId, const QString& name);
        bool setLayerDisplay(const QString& layerId,
                             const scopeone::core::LayerDisplayState& display);
        bool updateLayerFrame(const QString& layerId,
                              const scopeone::core::ImageFrame& frame);
        bool moveLayer(const QString& layerId, int offset);
        bool removeLayer(const QString& layerId);
        void removeLayersNotIn(const QSet<QString>& validLayerIds);

        QString createLine(const QString& layerKey,
                           const QPoint& start,
                           const QPoint& end,
                           const QString& label = QString(),
                           MarkupRole role = MarkupRole::Generic);
        QString createRect(const QString& layerKey,
                           const QRect& rect,
                           const QString& label = QString(),
                           MarkupRole role = MarkupRole::Generic);
        QList<Markup> markups(const QString& layerKey = QString()) const;
        bool hasMarkups() const;
        bool findMarkup(const QString& id, Markup& outMarkup) const;
        bool hasRole(MarkupRole role, const QString& layerKey = QString()) const;
        bool setLabel(const QString& id, const QString& label);
        bool setVisible(const QString& id, bool visible);
        bool setSelected(const QString& id, bool selected);
        bool selectOnly(const QString& id);
        bool updateLine(const QString& id, const QPoint& start, const QPoint& end);
        bool updateRect(const QString& id, const QRect& rect);
        bool remove(const QString& id);
        void clear(const QString& layerKey = QString());
        void clearRole(MarkupRole role, const QString& layerKey = QString());

        static QString typeName(MarkupType type);
        static QString roleName(MarkupRole role);
        static QString layerKindName(LayerKind layerKind);

    signals:
        void documentReplaced();
        void layersChanged();
        void markupsChanged();

    private:
        QString addMarkup(Markup markup);
        int layerIndex(const QString& layerId) const;
        int markupIndex(const QString& markupId) const;

        scopeone::core::ExperimentDocument m_document;
        int m_nextMarkupId{1};
    };
} // namespace scopeone::ui
