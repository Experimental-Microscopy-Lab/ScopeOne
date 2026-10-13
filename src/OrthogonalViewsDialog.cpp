#include "OrthogonalViewsDialog.h"

#include "ImageOps.h"

#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>

#include <algorithm>

namespace scopeone::ui
{
    // Shows a gray image fitted into the widget, with a cross hair at a pixel. A click moves it.
    class GrayCanvas : public QWidget
    {
        Q_OBJECT

    public:
        explicit GrayCanvas(QWidget* parent = nullptr) : QWidget(parent)
        {
            setMinimumSize(200, 120);
            setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        }
        void setImage(const QImage& image, const QPoint& cross)
        {
            m_image = image;
            m_cross = cross;
            update();
        }

    signals:
        void clicked(const QPoint& imagePosition);

    protected:
        void paintEvent(QPaintEvent*) override
        {
            QPainter painter(this);
            painter.fillRect(rect(), Qt::black);
            if (m_image.isNull())
            {
                return;
            }
            const QRect target = fitted();
            painter.drawImage(target, m_image);
            painter.setPen(QPen(QColor(255, 210, 0), 1));
            const double scale = static_cast<double>(target.width()) / m_image.width();
            const int x = target.left() + static_cast<int>((m_cross.x() + 0.5) * scale);
            const int y = target.top() + static_cast<int>((m_cross.y() + 0.5) * scale);
            painter.drawLine(x, target.top(), x, target.bottom());
            painter.drawLine(target.left(), y, target.right(), y);
        }
        void mousePressEvent(QMouseEvent* event) override
        {
            mouseMoveEvent(event);
        }
        void mouseMoveEvent(QMouseEvent* event) override
        {
            if (m_image.isNull() || !(event->buttons() & Qt::LeftButton))
            {
                return;
            }
            const QRect target = fitted();
            const double scale = static_cast<double>(m_image.width()) / target.width();
            const int x = std::clamp(static_cast<int>((event->pos().x() - target.left()) * scale), 0, m_image.width() - 1);
            const int y = std::clamp(static_cast<int>((event->pos().y() - target.top()) * scale), 0, m_image.height() - 1);
            emit clicked(QPoint(x, y));
        }

    private:
        QRect fitted() const
        {
            const double scale = std::min(static_cast<double>(width()) / m_image.width(),
                                          static_cast<double>(height()) / m_image.height());
            const int w = std::max(1, static_cast<int>(m_image.width() * scale));
            const int h = std::max(1, static_cast<int>(m_image.height() * scale));
            return QRect((width() - w) / 2, (height() - h) / 2, w, h);
        }

        QImage m_image;
        QPoint m_cross;
    };

    OrthogonalViewsDialog::OrthogonalViewsDialog(const QList<scopeone::core::ImageFrame>& frames, int slice,
                                                 QWidget* parent)
        : QDialog(parent)
        , m_frames(frames)
        , m_z(std::clamp(slice, 0, static_cast<int>(frames.size()) - 1))
    {
        setWindowTitle(tr("Orthogonal Views"));
        resize(760, 640);
        const scopeone::core::ImageFrame& first = m_frames.first();
        m_x = first.width / 2;
        m_y = first.height / 2;

        // One display range for the whole stack, so the cuts have the same brightness as the slices
        bool seen = false;
        for (const scopeone::core::ImageFrame& frame : m_frames)
        {
            const int step = std::max(1, std::max(frame.width, frame.height) / 128);
            for (int y = 0; y < frame.height; y += step)
            {
                for (int x = 0; x < frame.width; x += step)
                {
                    const double value = imageops::pixelAt(frame, x, y);
                    m_low = seen ? std::min(m_low, value) : value;
                    m_high = seen ? std::max(m_high, value) : value;
                    seen = true;
                }
            }
        }
        if (m_high <= m_low)
        {
            m_high = m_low + 1.0;
        }

        auto* layout = new QGridLayout(this);
        m_xy = new GrayCanvas(this);
        m_yz = new GrayCanvas(this);
        m_xz = new GrayCanvas(this);
        m_slider = new QSlider(Qt::Horizontal, this);
        m_slider->setRange(1, static_cast<int>(m_frames.size()));
        m_slider->setValue(m_z + 1);
        layout->addWidget(m_xy, 0, 0);
        layout->addWidget(m_yz, 0, 1);
        layout->addWidget(m_xz, 1, 0);
        layout->addWidget(new QLabel(tr("Slice"), this), 1, 1, Qt::AlignTop);
        layout->addWidget(m_slider, 2, 0, 1, 2);
        layout->setColumnStretch(0, 3);
        layout->setColumnStretch(1, 1);
        layout->setRowStretch(0, 3);
        layout->setRowStretch(1, 1);

        connect(m_slider, &QSlider::valueChanged, this, [this](int value)
        {
            m_z = value - 1;
            refresh();
        });
        connect(m_xy, &GrayCanvas::clicked, this, [this](const QPoint& position)
        {
            m_x = position.x();
            m_y = position.y();
            refresh();
        });
        // A click in a cut moves the cross hair in the other directions
        connect(m_xz, &GrayCanvas::clicked, this, [this](const QPoint& position)
        {
            m_x = position.x();
            m_z = position.y();
            m_slider->setValue(m_z + 1);
            refresh();
        });
        connect(m_yz, &GrayCanvas::clicked, this, [this](const QPoint& position)
        {
            m_z = position.x();
            m_y = position.y();
            m_slider->setValue(m_z + 1);
            refresh();
        });
        refresh();
    }

    void OrthogonalViewsDialog::refresh()
    {
        const scopeone::core::ImageFrame& first = m_frames.first();
        const int width = first.width;
        const int height = first.height;
        const int depth = static_cast<int>(m_frames.size());
        const auto gray = [this](int value)
        {
            return static_cast<uchar>(std::clamp((value - m_low) / (m_high - m_low), 0.0, 1.0) * 255.0);
        };

        QImage xy(width, height, QImage::Format_Grayscale8);
        const scopeone::core::ImageFrame& slice = m_frames.at(m_z);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                xy.scanLine(y)[x] = gray(imageops::pixelAt(slice, x, y));
            }
        }
        // Slices on the vertical axis, along the row of the cross hair
        QImage xz(width, depth, QImage::Format_Grayscale8);
        for (int z = 0; z < depth; ++z)
        {
            for (int x = 0; x < width; ++x)
            {
                xz.scanLine(z)[x] = gray(imageops::pixelAt(m_frames.at(z), x, m_y));
            }
        }
        // Slices on the horizontal axis, along the column of the cross hair
        QImage yz(depth, height, QImage::Format_Grayscale8);
        for (int y = 0; y < height; ++y)
        {
            for (int z = 0; z < depth; ++z)
            {
                yz.scanLine(y)[z] = gray(imageops::pixelAt(m_frames.at(z), m_x, y));
            }
        }
        m_xy->setImage(xy, QPoint(m_x, m_y));
        m_xz->setImage(xz, QPoint(m_x, m_z));
        m_yz->setImage(yz, QPoint(m_z, m_y));
    }
}

#include "OrthogonalViewsDialog.moc"
