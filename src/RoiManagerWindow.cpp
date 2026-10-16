#include "RoiManagerWindow.h"

#include "ImageWorkspace.h"

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace scopeone::ui
{
    using scopeone::core::ImageSceneModel;

    namespace
    {
        QString typeText(ImageSceneModel::MarkupType type)
        {
            return ImageSceneModel::typeName(type);
        }

        QString describe(const ImageSceneModel::Markup& markup, int number)
        {
            const QString name = markup.label.isEmpty() ? QStringLiteral("%1 %2").arg(typeText(markup.type)).arg(number)
                                                        : markup.label;
            switch (markup.type)
            {
            case ImageSceneModel::MarkupType::Line:
                return QStringLiteral("%1   (%2, %3) to (%4, %5)").arg(name).arg(markup.start.x()).arg(markup.start.y())
                    .arg(markup.end.x()).arg(markup.end.y());
            case ImageSceneModel::MarkupType::Point:
                return QStringLiteral("%1   (%2, %3)").arg(name).arg(markup.start.x()).arg(markup.start.y());
            case ImageSceneModel::MarkupType::Polygon:
                return QStringLiteral("%1   %2 corners").arg(name).arg(markup.points.size());
            default:
                return QStringLiteral("%1   %2 x %3 at (%4, %5)").arg(name).arg(markup.rect.width()).arg(markup.rect.height())
                    .arg(markup.rect.x()).arg(markup.rect.y());
            }
        }
    }

    RoiManagerWindow::RoiManagerWindow(ImageWorkspace* workspace, QWidget* parent)
        : QWidget(parent, Qt::Window)
        , m_workspace(workspace)
    {
        setWindowTitle(tr("ROI Manager"));
        resize(380, 420);
        auto* layout = new QHBoxLayout(this);
        m_list = new QListWidget(this);
        m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
        layout->addWidget(m_list, 1);

        auto* buttons = new QVBoxLayout();
        const auto addButton = [this, buttons](const QString& text)
        {
            auto* button = new QPushButton(text, this);
            buttons->addWidget(button);
            return button;
        };
        auto* measureButton = addButton(tr("Measure"));
        auto* measureAllButton = addButton(tr("Measure All"));
        auto* stackButton = addButton(tr("Measure on Every Slice"));
        auto* renameButton = addButton(tr("Rename..."));
        auto* deleteButton = addButton(tr("Delete"));
        auto* copyButton = addButton(tr("Copy to Image..."));
        auto* saveButton = addButton(tr("Save..."));
        auto* openButton = addButton(tr("Open..."));
        m_showAll = new QCheckBox(tr("Show all"), this);
        m_showAll->setChecked(true);
        buttons->addWidget(m_showAll);
        buttons->addStretch();
        layout->addLayout(buttons);

        connect(m_list, &QListWidget::itemSelectionChanged, this, [this]()
        {
            if (m_refreshing || !m_scene)
            {
                return;
            }
            const auto items = m_list->selectedItems();
            m_scene->selectOnly(items.isEmpty() ? QString() : items.first()->data(Qt::UserRole).toString());
        });
        connect(measureButton, &QPushButton::clicked, this,
                [this]() { emit measureRequested(chosenShapes(false), false); });
        connect(measureAllButton, &QPushButton::clicked, this,
                [this]() { emit measureRequested(chosenShapes(true), false); });
        connect(stackButton, &QPushButton::clicked, this,
                [this]() { emit measureRequested(chosenShapes(true), true); });
        connect(renameButton, &QPushButton::clicked, this, [this]() { renameChosen(); });
        connect(deleteButton, &QPushButton::clicked, this, [this]() { deleteChosen(); });
        connect(copyButton, &QPushButton::clicked, this, [this]() { copyChosen(); });
        connect(saveButton, &QPushButton::clicked, this, [this]() { saveShapes(); });
        connect(openButton, &QPushButton::clicked, this, [this]() { openShapes(); });
        connect(m_showAll, &QCheckBox::toggled, this, [this](bool visible)
        {
            if (!m_scene)
            {
                return;
            }
            for (const ImageSceneModel::Markup& markup : m_scene->markups(m_layerKey))
            {
                m_scene->setVisible(markup.id, visible);
            }
        });

        connect(m_workspace, &ImageWorkspace::activeDocumentChanged, this, [this]() { attachToActiveImage(); });
        connect(m_workspace, &ImageWorkspace::activeLayerChanged, this, [this]() { attachToActiveImage(); });
        attachToActiveImage();
    }

    void RoiManagerWindow::attachToActiveImage()
    {
        if (m_scene)
        {
            disconnect(m_scene, nullptr, this, nullptr);
        }
        m_scene = m_workspace->activeSceneModel();
        m_layerKey = m_workspace->activeLayerKey();
        if (m_scene)
        {
            connect(m_scene, &ImageSceneModel::markupsChanged, this, [this]() { refresh(); });
        }
        refresh();
    }

    void RoiManagerWindow::refresh()
    {
        m_refreshing = true;
        m_list->clear();
        if (m_scene)
        {
            int number = 1;
            for (const ImageSceneModel::Markup& markup : m_scene->markups(m_layerKey))
            {
                auto* item = new QListWidgetItem(describe(markup, number++), m_list);
                item->setData(Qt::UserRole, markup.id);
                item->setSelected(markup.selected);
            }
        }
        m_refreshing = false;
    }

    QList<ImageSceneModel::Markup> RoiManagerWindow::chosenShapes(bool allWhenNothingChosen) const
    {
        QList<ImageSceneModel::Markup> shapes;
        if (!m_scene)
        {
            return shapes;
        }
        const auto items = m_list->selectedItems();
        for (const ImageSceneModel::Markup& markup : m_scene->markups(m_layerKey))
        {
            bool chosen = false;
            for (QListWidgetItem* item : items)
            {
                chosen = chosen || item->data(Qt::UserRole).toString() == markup.id;
            }
            if (chosen || (allWhenNothingChosen && items.isEmpty()))
            {
                shapes.append(markup);
            }
        }
        return shapes;
    }

    void RoiManagerWindow::renameChosen()
    {
        const auto shapes = chosenShapes(false);
        if (shapes.isEmpty())
        {
            return;
        }
        bool accepted = false;
        const QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal,
                                                   shapes.first().label, &accepted);
        if (accepted)
        {
            for (const ImageSceneModel::Markup& shape : shapes)
            {
                m_scene->setLabel(shape.id, name);
            }
        }
    }

    void RoiManagerWindow::deleteChosen()
    {
        for (const ImageSceneModel::Markup& shape : chosenShapes(false))
        {
            m_scene->remove(shape.id);
        }
    }

    // Puts copies of the chosen shapes on another open image
    void RoiManagerWindow::copyChosen()
    {
        const auto shapes = chosenShapes(true);
        QStringList titles;
        QStringList ids;
        for (const ImageDocumentInfo& document : m_workspace->documents())
        {
            if (!document.active)
            {
                titles.append(document.title);
                ids.append(document.id);
            }
        }
        if (shapes.isEmpty() || titles.isEmpty())
        {
            return;
        }
        bool accepted = false;
        const QString title = QInputDialog::getItem(this, tr("Copy to Image"), tr("Image:"), titles, 0, false, &accepted);
        if (!accepted)
        {
            return;
        }
        ImageSceneModel* target = m_workspace->sceneModel(ids.at(titles.indexOf(title)));
        if (!target || target->layerIds().isEmpty())
        {
            return;
        }
        const QString layerKey = target->layerIds().first();
        for (const ImageSceneModel::Markup& shape : shapes)
        {
            switch (shape.type)
            {
            case ImageSceneModel::MarkupType::Line: target->createLine(layerKey, shape.start, shape.end, shape.label); break;
            case ImageSceneModel::MarkupType::Rect: target->createRect(layerKey, shape.rect, shape.label); break;
            case ImageSceneModel::MarkupType::Ellipse: target->createEllipse(layerKey, shape.rect, shape.label); break;
            case ImageSceneModel::MarkupType::Polygon: target->createPolygon(layerKey, shape.points, shape.label); break;
            case ImageSceneModel::MarkupType::Point: target->createPoint(layerKey, shape.start, shape.label); break;
            }
        }
    }

    void RoiManagerWindow::saveShapes()
    {
        const auto shapes = chosenShapes(true);
        const QString path = QFileDialog::getSaveFileName(this, tr("Save ROIs"), QStringLiteral("rois.json"),
                                                          tr("ROI files (*.json)"));
        if (path.isEmpty())
        {
            return;
        }
        QJsonArray rois;
        for (const ImageSceneModel::Markup& shape : shapes)
        {
            QJsonObject roi;
            roi.insert(QStringLiteral("type"), typeText(shape.type));
            roi.insert(QStringLiteral("label"), shape.label);
            roi.insert(QStringLiteral("x1"), shape.start.x());
            roi.insert(QStringLiteral("y1"), shape.start.y());
            roi.insert(QStringLiteral("x2"), shape.end.x());
            roi.insert(QStringLiteral("y2"), shape.end.y());
            roi.insert(QStringLiteral("x"), shape.rect.x());
            roi.insert(QStringLiteral("y"), shape.rect.y());
            roi.insert(QStringLiteral("width"), shape.rect.width());
            roi.insert(QStringLiteral("height"), shape.rect.height());
            QJsonArray points;
            for (const QPoint& point : shape.points)
            {
                points.append(QJsonArray{point.x(), point.y()});
            }
            roi.insert(QStringLiteral("points"), points);
            rois.append(roi);
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly))
        {
            file.write(QJsonDocument(QJsonObject{{QStringLiteral("format"), QStringLiteral("ScopeOne ROI set")},
                                                 {QStringLiteral("rois"), rois}}).toJson());
        }
    }

    void RoiManagerWindow::openShapes()
    {
        const QString path = QFileDialog::getOpenFileName(this, tr("Open ROIs"), QString(), tr("ROI files (*.json)"));
        if (path.isEmpty() || !m_scene)
        {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return;
        }
        const QJsonArray rois = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("rois")).toArray();
        for (const QJsonValue& value : rois)
        {
            const QJsonObject roi = value.toObject();
            const QString type = roi.value(QStringLiteral("type")).toString();
            const QString label = roi.value(QStringLiteral("label")).toString();
            const QPoint start(roi.value(QStringLiteral("x1")).toInt(), roi.value(QStringLiteral("y1")).toInt());
            const QPoint end(roi.value(QStringLiteral("x2")).toInt(), roi.value(QStringLiteral("y2")).toInt());
            const QRect rect(roi.value(QStringLiteral("x")).toInt(), roi.value(QStringLiteral("y")).toInt(),
                             roi.value(QStringLiteral("width")).toInt(), roi.value(QStringLiteral("height")).toInt());
            if (type == QLatin1String("Line")) m_scene->createLine(m_layerKey, start, end, label);
            else if (type == QLatin1String("Rect")) m_scene->createRect(m_layerKey, rect, label);
            else if (type == QLatin1String("Ellipse")) m_scene->createEllipse(m_layerKey, rect, label);
            else if (type == QLatin1String("Point")) m_scene->createPoint(m_layerKey, start, label);
            else if (type == QLatin1String("Polygon"))
            {
                QList<QPoint> corners;
                for (const QJsonValue& point : roi.value(QStringLiteral("points")).toArray())
                {
                    corners.append(QPoint(point.toArray().at(0).toInt(), point.toArray().at(1).toInt()));
                }
                m_scene->createPolygon(m_layerKey, corners, label);
            }
        }
    }
}
