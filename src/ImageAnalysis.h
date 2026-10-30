#pragma once

#include "scopeone/ImageOps.h"
#include "ParameterDialog.h"
#include "StackAxes.h"
#include "scopeone/ScopeOneCore.h"

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

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

    enum class BinaryOperation { Erode, Dilate, Open, Close, FillHoles, Outline, Invert, Watershed, Skeletonize };

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
        // Where the panels of results, selections and profiles are shown, such as a dock of the window
        using PanelPresenter = std::function<void(QWidget*, const QString&)>;
        void setPanelPresenter(PanelPresenter presenter) { m_panelPresenter = std::move(presenter); }
        // Adds the menus to the menu bar in front of the given action
        void addMenus(QMenuBar* menuBar, QAction* before);
        // Shows the commands that are used most on an image at the given screen position
        void showContextMenu(const QPoint& globalPosition);
        // More commands for the end of that menu
        void setContextActions(const QList<QAction*>& actions) { m_contextActions = actions; }

    signals:
        // The user asked to put a module with these parameters into the processing pipeline
        void pipelineStepRequested(const QString& moduleId, const QVariantMap& parameters);

    private:
        // The image that a command works on
        struct Source
        {
            QList<scopeone::core::ImageFrame> frames;
            int index{0};
            QString title;
            QString layerKey;
            scopeone::core::ImageSceneModel* scene{nullptr};
            // How the slices lie along the channel, Z and time axes. A stack that has no such shape is a
            // single series along the time axis, and hyper is false.
            scopeone::core::StackShape shape;
            bool hyper{false};
            // The file or the recording says how its slices are arranged, which can be along one axis only
            bool known{false};
        };
        enum class Scope { Cancel, Current, All };

        bool activeSource(Source& source) const;
        double pixelScale(const Source& source) const;
        bool selectedShape(const Source& source, scopeone::core::ImageSceneModel::Markup& shape) const;
        Scope chooseScope(const Source& source, const QString& command) const;
        // Makes a new image from the slices that a command works on
        void present(const QList<scopeone::core::ImageFrame>& frames, const QString& title,
                     const scopeone::core::StackShape* shape = nullptr);
        // The axes a command can work along, for a dialog, and the planes along one axis
        static QStringList axisChoices(const Source& source, QList<QChar>& letters);
        static QList<scopeone::core::ImageFrame> framesAlong(const Source& source, QChar axis, int* position = nullptr);
        using FrameOperation = std::function<scopeone::core::ImageFrame(const scopeone::core::ImageFrame&)>;
        void apply(const QString& title, const FrameOperation& operation);

        void bindShortcuts();
        void showPanel(QWidget* panel, const QString& title);
        ResultsWindow* results();
        static QAction* findAction(QMenu* menu, const QString& name);
        void buildEditMenu(QMenu* menu);
        void buildImageMenu(QMenu* menu);
        void buildProcessMenu(QMenu* menu);
        void buildAnalyzeMenu(QMenu* menu);
        using PreviewRenderer = std::function<QImage(const QList<ParameterField>&)>;
        PreviewRenderer makePreview(
            const Source& source,
            const std::function<scopeone::core::ImageFrame(const scopeone::core::ImageFrame&, const QList<ParameterField>&)>& operation) const;
        bool askWithPreview(
            const QString& title, QList<ParameterField>& fields,
            const std::function<scopeone::core::ImageFrame(const scopeone::core::ImageFrame&, const QList<ParameterField>&)>& operation);
        void runModule(const QString& menuText, const QString& moduleId, const QVariantMap& preset, const QStringList& askKeys);
        void addModuleCommand(QMenu* menu, const QString& text, const QString& moduleId, const QVariantMap& preset,
                              const QStringList& askKeys);
        void addFilter(QMenu* menu, const QString& text, int kind, double radius);
        void addMath(QMenu* menu, const QString& text, int operation, bool needsValue,
                     double value);
        void addBinary(QMenu* menu, const QString& text, BinaryOperation operation);
        void addImageCommand(QMenu* menu, const QString& text, const FrameOperation& operation);

        void duplicateImage();
        void cropImage();
        void scaleImage();
        void rotateImage();
        void thresholdImage();
        void batchProcess();
        void zProject();
        void makeSubstack();
        void makeMontage();
        void plotZProfile();
        void showOrthogonalViews();
        void subtractBackground();
        void calculate();
        void measure();
        // The selection that a command changes, which has to be drawn first
        bool requireSelection(Source& source, scopeone::core::ImageSceneModel::Markup& shape);
        // Takes the place of the selection with a polygon, a rectangle or nothing
        void replaceWithPolygon(const Source& source, const scopeone::core::ImageSceneModel::Markup& old,
                                const QList<QPoint>& corners);
        void selectAll();
        void specifySelection();
        void enlargeSelection();
        void rotateSelection();
        void scaleSelection();
        void createMask();
        void createSelection();
        void imageProperties();
        void enhanceContrast();
        void enhanceLocalContrast();
        void applyDisplayRange();
        void addNoise(bool saltAndPepper);
        void wandTolerance();
        void analyzeParticles();
        void measureShapes(const QList<scopeone::core::ImageSceneModel::Markup>& shapes, bool allSlices);

        scopeone::core::ScopeOneCore* m_core{nullptr};
        ImageWorkspace* m_workspace{nullptr};
        QWidget* m_window{nullptr};
        Presenter m_presenter;
        PanelPresenter m_panelPresenter;
        QList<QMenu*> m_menus;
        QList<QAction*> m_contextActions;
        QPointer<ResultsWindow> m_results;
        QPointer<RoiManagerWindow> m_roiManager;
        QPointer<ProfilePlotWindow> m_profile;
        int m_resultCount{0};
    };
}
