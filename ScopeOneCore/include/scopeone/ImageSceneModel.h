#pragma once

#include "scopeone/ExperimentDocument.h"
#include "scopeone/scopeone_core_export.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT ImageSceneModel : public QObject
    {
        Q_OBJECT

    public:
        using MarkupType = DocumentMarkupType;
        using MarkupRole = DocumentMarkupRole;
        using LayerKind = DocumentLayerKind;

        struct Markup
        {
            QString id;
            MarkupType type{MarkupType::Line};
            MarkupRole role{MarkupRole::Generic};
            LayerKind layerKind{LayerKind::Raw};
            QString layerKey;
            QString sourceId;
            QPoint start;
            QPoint end;
            QRect rect;
            QString label;
            bool visible{true};
            bool selected{false};
        };

        struct SourceDisplayTransform
        {
            int offsetX{0};
            int offsetY{0};
            bool flipX{false};
            bool flipY{false};
            int zoomPercent{100};
        };

        explicit ImageSceneModel(QObject* parent = nullptr);

        const ExperimentDocument& document() const;
        bool setDocument(const ExperimentDocument& document,
                         QString* errorMessage = nullptr);
        void reset();

        QStringList layerIds() const;
        QStringList visibleLayerIds() const;
        bool hasSource(const QString& sourceId) const;
        bool findLayer(const QString& layerId,
                       DocumentLayer& outLayer) const;
        bool ensureLayer(const DocumentLayer& layer);
        bool setLayerName(const QString& layerId, const QString& name);
        bool setVisibleLayers(const QStringList& layerIds);
        bool setLayerVisible(const QString& layerId, bool visible);
        bool setLayerOpacityPercent(const QString& layerId, int percent);
        bool setLayerGamma(const QString& layerId, double gamma);
        bool setLayerColormap(const QString& layerId, const QString& colormap);
        bool setLayerBlending(const QString& layerId, const QString& blending);
        bool setLayerDisplayLevels(const QString& layerId,
                                   int minLevel,
                                   int maxLevel,
                                   int domainMax);
        bool updateLayerFrame(const QString& layerId,
                              const scopeone::core::ImageFrame& frame);
        bool moveLayer(const QString& layerId, int offset);
        bool removeLayer(const QString& layerId);

        bool layerAutoStretchEnabled(const QString& layerId) const;
        bool setLayerAutoStretchEnabled(const QString& layerId, bool enabled);
        SourceDisplayTransform sourceDisplayTransform(const QString& sourceId) const;
        bool setSourceDisplayTransform(const QString& sourceId,
                                       const SourceDisplayTransform& transform);
        bool resetSourceDisplayTransform(const QString& sourceId);

        static QStringList supportedColormaps();
        static QStringList supportedBlendingModes();

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
        void layersChanged();
        void layerDisplayChanged(const QString& layerId);
        void layerAutoStretchChanged(const QString& layerId, bool enabled);
        void sourceDisplayTransformChanged(const QString& sourceId);
        void markupsChanged();

    private:
        bool setLayerDisplay(const QString& layerId,
                             const LayerDisplayState& display);
        QString addMarkup(Markup markup);
        int layerIndex(const QString& layerId) const;
        int markupIndex(const QString& markupId) const;

        ExperimentDocument m_document;
        QHash<QString, SourceDisplayTransform> m_sourceDisplayTransforms;
        QSet<QString> m_autoStretchLayerIds;
        int m_nextMarkupId{1};
    };
} // namespace scopeone::core
