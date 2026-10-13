#pragma once

#include "scopeone/ImageFrame.h"

#include <QDialog>
#include <QImage>
#include <QList>

class QSlider;

namespace scopeone::ui
{
    class GrayCanvas;

    // The slice of a stack on one side and the cuts through the stack at the cross hair on the
    // other two sides, like the Orthogonal Views of ImageJ
    class OrthogonalViewsDialog : public QDialog
    {
        Q_OBJECT

    public:
        OrthogonalViewsDialog(const QList<scopeone::core::ImageFrame>& frames, int slice, QWidget* parent = nullptr);

    private:
        void refresh();

        QList<scopeone::core::ImageFrame> m_frames;
        int m_x{0};
        int m_y{0};
        int m_z{0};
        double m_low{0.0};
        double m_high{1.0};
        GrayCanvas* m_xy{nullptr};
        GrayCanvas* m_xz{nullptr};
        GrayCanvas* m_yz{nullptr};
        QSlider* m_slider{nullptr};
    };
}
