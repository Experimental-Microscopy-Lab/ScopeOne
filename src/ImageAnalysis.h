#pragma once

#include "ImageOps.h"
#include "scopeone/ScopeOneCore.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class QAction;
class QMenu;
class QMenuBar;
class QWidget;

namespace scopeone::ui
{
    class ImageWorkspace;
    class ProfilePlotWindow;
    class ResultsWindow;
    class RoiManagerWindow;

    // The Image, Process and Analyze menus in the manner of ImageJ. Every command works on the
    // active image, or on all of its slices, and puts its result in a new image.
    class ImageAnalysis : public QObject
    {
        Q_OBJECT

    public:
        using Presenter = std::function<void(
            const std::shared_ptr<scopeone::core::ScopeOneCore::RecordingSessionData>&, const QString&)>;

        ImageAnalysis(scopeone::core::ScopeOneCore* core, ImageWorkspace* workspace, QWidget* window,
                      QObject* parent = nullptr);

        // Where a new image is shown
        void setPresenter(Presenter presenter) { m_presenter = std::move(presenter); }
        // Adds the menus to the menu bar in front of the given action
        void addMenus(QMenuBar* menuBar, QAction* before);

    private:
        // The image that a command works on
        struct Source
        {
            QList<scopeone::core::ImageFrame> frames;
            int index{0};
            QString title;
            QString layerKey;
            scopeone::core::ImageSceneModel* scene{nullptr};
        };
        enum class Scope { Cancel, Current, All };

        bool activeSource(Source& source) const;
        double pixelScale(const Source& source) const;
        bool selectedShape(const Source& source, scopeone::core::ImageSceneModel::Markup& shape) const;
        Scope chooseScope(const Source& source, const QString& command) const;
        // Makes a new image from the slices that a command works on
        void present(const QList<scopeone::core::ImageFrame>& frames, const QString& title);
        using FrameOperation = std::function<scopeone::core::ImageFrame(const scopeone::core::ImageFrame&)>;
        void apply(const QString& title, const FrameOperation& operation);

        void buildImageMenu(QMenu* menu);
        void buildProcessMenu(QMenu* menu);
        void buildAnalyzeMenu(QMenu* menu);
        void addFilter(QMenu* menu, const QString& text, imageops::Filter filter, double radius);
        void addMath(QMenu* menu, const QString& text, imageops::MathOperation operation, bool needsValue,
                     double value);
        void addBinary(QMenu* menu, const QString& text, imageops::BinaryOperation operation);
        void addImageCommand(QMenu* menu, const QString& text, const FrameOperation& operation);

        void duplicateImage();
        void cropImage();
        void scaleImage();
        void rotateImage();
        void thresholdImage();
        void zProject();
        void makeSubstack();
        void makeMontage();
        void plotZProfile();
        void showOrthogonalViews();
        void subtractBackground();
        void unsharpMask();
        void calculate();
        void measure();
        void analyzeParticles();
        void measureShapes(const QList<scopeone::core::ImageSceneModel::Markup>& shapes, bool allSlices);

        scopeone::core::ScopeOneCore* m_core{nullptr};
        ImageWorkspace* m_workspace{nullptr};
        QWidget* m_window{nullptr};
        Presenter m_presenter;
        QPointer<ResultsWindow> m_results;
        QPointer<RoiManagerWindow> m_roiManager;
        QPointer<ProfilePlotWindow> m_profile;
        int m_resultCount{0};
    };
}
