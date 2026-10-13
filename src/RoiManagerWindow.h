#pragma once

#include "scopeone/ImageSceneModel.h"

#include <QList>
#include <QPointer>
#include <QWidget>

class QCheckBox;
class QListWidget;

namespace scopeone::ui
{
    class ImageWorkspace;

    // The list of the shapes drawn on the active image, like the ROI Manager of ImageJ. Shapes
    // stay on the image as long as they are in the list, can be named, measured, saved to a
    // file and opened again.
    class RoiManagerWindow : public QWidget
    {
        Q_OBJECT

    public:
        explicit RoiManagerWindow(ImageWorkspace* workspace, QWidget* parent = nullptr);

    signals:
        // Measure these shapes on the current slice, or on every slice of the stack
        void measureRequested(const QList<scopeone::core::ImageSceneModel::Markup>& shapes, bool allSlices);

    private:
        void attachToActiveImage();
        void refresh();
        QList<scopeone::core::ImageSceneModel::Markup> chosenShapes(bool allWhenNothingChosen) const;
        void renameChosen();
        void deleteChosen();
        void copyChosen();
        void saveShapes();
        void openShapes();

        ImageWorkspace* m_workspace{nullptr};
        QPointer<scopeone::core::ImageSceneModel> m_scene;
        QString m_layerKey;
        QListWidget* m_list{nullptr};
        QCheckBox* m_showAll{nullptr};
        bool m_refreshing{false};
    };
}
