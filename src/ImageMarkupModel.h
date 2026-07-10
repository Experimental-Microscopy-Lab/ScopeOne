#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>

namespace scopeone::ui
{
    class ImageMarkupModel : public QObject
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

        explicit ImageMarkupModel(QObject* parent = nullptr);

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
        static QString typeName(MarkupType type);
        static QString roleName(MarkupRole role);
        static QString layerKindName(LayerKind layerKind);
        static LayerKind layerKindForLayerKey(const QString& layerKey);
        bool remove(const QString& id);
        void clear(const QString& layerKey = QString());
        void clearRole(MarkupRole role, const QString& layerKey = QString());

    signals:
        void changed();

    private:
        QString addMarkup(Markup markup);

        QMap<QString, Markup> m_markups;
        QStringList m_markupOrder;
        int m_nextMarkupId{1};
    };
} // namespace scopeone::ui
