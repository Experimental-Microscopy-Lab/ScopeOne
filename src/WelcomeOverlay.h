#pragma once

#include <QWidget>

class QVBoxLayout;

namespace scopeone::ui
{
    // What the viewer shows while nothing is open: the ways to start and the recent files
    class WelcomeOverlay : public QWidget
    {
        Q_OBJECT

    public:
        explicit WelcomeOverlay(QWidget* parent);

        // Lists the recent images and configurations again
        void setRecent(const QStringList& images, const QStringList& configurations);

    signals:
        void openImageRequested();
        void loadConfigurationRequested();
        void imageChosen(const QString& path);
        void configurationChosen(const QString& path);

    private:
        QWidget* m_recentHost{nullptr};
        QVBoxLayout* m_recentLayout{nullptr};
    };
}
