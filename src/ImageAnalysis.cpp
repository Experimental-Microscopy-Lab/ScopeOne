#include "ImageAnalysis.h"

#include "ImageWorkspace.h"
#include "OrthogonalViewsDialog.h"
#include "ParameterDialog.h"
#include "ProfilePlotWindow.h"
#include "ResultsWindow.h"
#include "RoiManagerWindow.h"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>

namespace scopeone::ui
{
    using scopeone::core::ImageFrame;
    using scopeone::core::ImageSceneModel;

    ImageAnalysis::ImageAnalysis(scopeone::core::ScopeOneCore* core, ImageWorkspace* workspace, QWidget* window,
                                 QObject* parent)
        : QObject(parent)
        , m_core(core)
        , m_workspace(workspace)
        , m_window(window)
    {
    }

    void ImageAnalysis::addMenus(QMenuBar* menuBar, QAction* before)
    {
        QMenu* edit = new QMenu(tr("&Edit"), menuBar);
        QMenu* image = new QMenu(tr("&Image"), menuBar);
        QMenu* process = new QMenu(tr("&Process"), menuBar);
        QMenu* analyze = new QMenu(tr("&Analyze"), menuBar);
        menuBar->insertMenu(before, edit);
        menuBar->insertMenu(before, image);
        menuBar->insertMenu(before, process);
        menuBar->insertMenu(before, analyze);
        buildEditMenu(edit);
        buildImageMenu(image);
        buildProcessMenu(process);
        buildAnalyzeMenu(analyze);
    }

    // ---- the image a command works on

    bool ImageAnalysis::activeSource(Source& source) const
    {
        source = Source{};
        source.layerKey = m_workspace->activeLayerKey();
        source.scene = m_workspace->activeSceneModel();
        const ImageDocumentInfo document = m_workspace->document();
        if (document.isValid())
        {
            source.title = document.title;
            const auto session = m_workspace->documentSession();
            if (session && document.frameCount > 1)
            {
                for (int index = 0; index < document.frameCount; ++index)
                {
                    const ImageFrame frame = session->recordedFrame(document.cameraId, index);
                    if (frame.isValid())
                    {
                        source.frames.append(frame);
                    }
                }
                source.index = std::clamp(document.frameIndex, 0, static_cast<int>(source.frames.size()) - 1);
            }
            else
            {
                source.frames.append(m_workspace->currentFrame(document.id));
            }
        }
        else
        {
            source.title = tr("Live image");
            source.frames.append(m_workspace->frameForLayer(source.layerKey));
        }
        if (source.frames.isEmpty() || !imageops::isMono(source.frames.first()))
        {
            QMessageBox::information(m_window, tr("No image"), tr("There is no 8 bit or 16 bit gray image to work on."));
            return false;
        }
        return true;
    }

    // Size of a pixel in micrometers, which is zero when the image is not calibrated
    double ImageAnalysis::pixelScale(const Source& source) const
    {
        const double pixelSizeUm = m_workspace->pixelSizeUm(source.layerKey);
        scopeone::core::DocumentLayer layer;
        if (pixelSizeUm > 0.0 && source.scene && source.scene->findLayer(source.layerKey, layer))
        {
            const double determinant = std::abs(layer.pixelToSensor.m11 * layer.pixelToSensor.m22
                                                - layer.pixelToSensor.m12 * layer.pixelToSensor.m21);
            return std::sqrt(determinant) * pixelSizeUm;
        }
        return 0.0;
    }

    bool ImageAnalysis::selectedShape(const Source& source, ImageSceneModel::Markup& shape) const
    {
        if (!source.scene)
        {
            return false;
        }
        for (const ImageSceneModel::Markup& markup : source.scene->markups(source.layerKey))
        {
            if (markup.selected)
            {
                shape = markup;
                return true;
            }
        }
        return false;
    }

    // Like ImageJ, asks whether a stack is processed as a whole when it has more than one slice
    ImageAnalysis::Scope ImageAnalysis::chooseScope(const Source& source, const QString& command) const
    {
        if (source.frames.size() < 2)
        {
            return Scope::Current;
        }
        QMessageBox box(m_window);
        box.setWindowTitle(command);
        box.setText(tr("Process all %1 slices?").arg(source.frames.size()));
        QPushButton* all = box.addButton(tr("All slices"), QMessageBox::YesRole);
        QPushButton* current = box.addButton(tr("This slice"), QMessageBox::NoRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == all)
        {
            return Scope::All;
        }
        return box.clickedButton() == current ? Scope::Current : Scope::Cancel;
    }

    void ImageAnalysis::present(const QList<ImageFrame>& frames, const QString& title)
    {
        if (frames.isEmpty() || !m_presenter)
        {
            return;
        }
        const auto session = m_core->createFrameSession(frames, scopeone::core::ExperimentPlan{});
        if (session)
        {
            m_presenter(session, title);
        }
    }

    void ImageAnalysis::apply(const QString& menuText, const FrameOperation& operation)
    {
        // The name of the command as it reads, without the mnemonic mark and the dots of a dialog
        QString title = menuText;
        title.remove(QLatin1Char('&'));
        while (title.endsWith(QLatin1Char('.')))
        {
            title.chop(1);
        }
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const Scope scope = chooseScope(source, title);
        if (scope == Scope::Cancel)
        {
            return;
        }
        QList<ImageFrame> result;
        if (scope == Scope::All)
        {
            for (const ImageFrame& frame : source.frames)
            {
                result.append(operation(frame));
            }
        }
        else
        {
            result.append(operation(source.frames.at(source.index)));
        }
        present(result, QStringLiteral("%1 (%2)").arg(title, source.title));
    }

    // ---- menus

    void ImageAnalysis::addFilter(QMenu* menu, const QString& text, imageops::Filter filter, double radius)
    {
        connect(menu->addAction(text), &QAction::triggered, this, [this, text, filter, radius]()
        {
            QList<ParameterField> fields{ParameterField::real(tr("Radius (pixels)"), radius, 0.5, 200.0, 1)};
            if (askParameters(m_window, text, fields))
            {
                const double chosen = fields.first().value;
                apply(text, [filter, chosen](const ImageFrame& frame) { return imageops::filtered(frame, filter, chosen); });
            }
        });
    }

    void ImageAnalysis::addMath(QMenu* menu, const QString& text, imageops::MathOperation operation,
                                bool needsValue, double value)
    {
        connect(menu->addAction(text), &QAction::triggered, this, [this, text, operation, needsValue, value]()
        {
            double chosen = value;
            if (needsValue)
            {
                QList<ParameterField> fields{ParameterField::real(tr("Value"), value, -1.0e9, 1.0e9, 3)};
                if (!askParameters(m_window, text, fields))
                {
                    return;
                }
                chosen = fields.first().value;
            }
            apply(text, [operation, chosen](const ImageFrame& frame) { return imageops::math(frame, operation, chosen); });
        });
    }

    void ImageAnalysis::addBinary(QMenu* menu, const QString& text, imageops::BinaryOperation operation)
    {
        connect(menu->addAction(text), &QAction::triggered, this, [this, text, operation]()
        {
            int iterations = 1;
            double tolerance = 0.5;
            if (operation == imageops::BinaryOperation::Watershed)
            {
                QList<ParameterField> fields{ParameterField::real(tr("Tolerance"), 0.5, 0.0, 50.0, 2)};
                if (!askParameters(m_window, text, fields))
                {
                    return;
                }
                tolerance = fields.first().value;
            }
            if (operation == imageops::BinaryOperation::Erode || operation == imageops::BinaryOperation::Dilate
                || operation == imageops::BinaryOperation::Open || operation == imageops::BinaryOperation::Close)
            {
                QList<ParameterField> fields{ParameterField::integer(tr("Iterations"), 1, 1, 100)};
                if (!askParameters(m_window, text, fields))
                {
                    return;
                }
                iterations = static_cast<int>(fields.first().value);
            }
            apply(text, [operation, iterations, tolerance](const ImageFrame& frame)
            {
                return imageops::binary(frame, operation, iterations, tolerance);
            });
        });
    }

    void ImageAnalysis::addImageCommand(QMenu* menu, const QString& text, const FrameOperation& operation)
    {
        connect(menu->addAction(text), &QAction::triggered, this,
                [this, text, operation]() { apply(QString(text).remove(QLatin1Char('&')), operation); });
    }

    void ImageAnalysis::buildEditMenu(QMenu* menu)
    {
        QAction* selectAllAction = menu->addAction(tr("Select &All"));
        selectAllAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+A")));
        connect(selectAllAction, &QAction::triggered, this, [this]() { selectAll(); });
        QAction* selectNoneAction = menu->addAction(tr("Select &None"));
        selectNoneAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
        connect(selectNoneAction, &QAction::triggered, this, [this]()
        {
            if (ImageSceneModel* scene = m_workspace->activeSceneModel())
            {
                scene->selectOnly(QString());
            }
        });
        menu->addSeparator();

        QMenu* selection = menu->addMenu(tr("&Selection"));
        connect(selection->addAction(tr("To &Bounding Box")), &QAction::triggered, this, [this]()
        {
            Source source;
            ImageSceneModel::Markup shape;
            if (requireSelection(source, shape))
            {
                const QRect box = imageops::selectionBounds(shape);
                source.scene->remove(shape.id);
                const QString id = source.scene->createRect(source.layerKey, box, shape.label);
                source.scene->selectOnly(id);
            }
        });
        connect(selection->addAction(tr("&Convex Hull")), &QAction::triggered, this, [this]()
        {
            Source source;
            ImageSceneModel::Markup shape;
            if (requireSelection(source, shape))
            {
                replaceWithPolygon(source, shape, imageops::convexHull(imageops::selectionCorners(shape)));
            }
        });
        connect(selection->addAction(tr("Fit &Ellipse")), &QAction::triggered, this, [this]()
        {
            Source source;
            ImageSceneModel::Markup shape;
            if (requireSelection(source, shape))
            {
                replaceWithPolygon(source, shape, imageops::fittedEllipse(source.frames.first().width,
                                                                          source.frames.first().height, shape));
            }
        });
        connect(selection->addAction(tr("En&large...")), &QAction::triggered, this, [this]() { enlargeSelection(); });
        connect(selection->addAction(tr("&Specify...")), &QAction::triggered, this, [this]() { specifySelection(); });
        connect(selection->addAction(tr("&Rotate...")), &QAction::triggered, this, [this]() { rotateSelection(); });
        connect(selection->addAction(tr("S&cale...")), &QAction::triggered, this, [this]() { scaleSelection(); });
        selection->addSeparator();
        connect(selection->addAction(tr("Create &Mask")), &QAction::triggered, this, [this]() { createMask(); });
        connect(selection->addAction(tr("Create Se&lection")), &QAction::triggered, this, [this]() { createSelection(); });
    }

    void ImageAnalysis::buildImageMenu(QMenu* menu)
    {
        connect(menu->addAction(tr("&Duplicate...")), &QAction::triggered, this, [this]() { duplicateImage(); });
        connect(menu->addAction(tr("&Crop")), &QAction::triggered, this, [this]() { cropImage(); });
        connect(menu->addAction(tr("&Scale...")), &QAction::triggered, this, [this]() { scaleImage(); });

        QMenu* transform = menu->addMenu(tr("&Transform"));
        connect(transform->addAction(tr("Flip &Horizontally")), &QAction::triggered, this,
                [this]() { apply(tr("Flip Horizontally"), [](const ImageFrame& f) { return imageops::flipHorizontal(f); }); });
        connect(transform->addAction(tr("Flip &Vertically")), &QAction::triggered, this,
                [this]() { apply(tr("Flip Vertically"), [](const ImageFrame& f) { return imageops::flipVertical(f); }); });
        connect(transform->addAction(tr("Rotate 90 Degrees &Right")), &QAction::triggered, this,
                [this]() { apply(tr("Rotate 90 Degrees Right"), [](const ImageFrame& f) { return imageops::rotate90(f, true); }); });
        connect(transform->addAction(tr("Rotate 90 Degrees &Left")), &QAction::triggered, this,
                [this]() { apply(tr("Rotate 90 Degrees Left"), [](const ImageFrame& f) { return imageops::rotate90(f, false); }); });
        connect(transform->addAction(tr("&Rotate...")), &QAction::triggered, this, [this]() { rotateImage(); });

        connect(menu->addAction(tr("&Properties...")), &QAction::triggered, this, [this]() { imageProperties(); });

        QMenu* adjust = menu->addMenu(tr("&Adjust"));
        connect(adjust->addAction(tr("&Threshold...")), &QAction::triggered, this, [this]() { thresholdImage(); });
        connect(adjust->addAction(tr("&Apply Display Range")), &QAction::triggered, this, [this]() { applyDisplayRange(); });

        QMenu* stacks = menu->addMenu(tr("S&tacks"));
        connect(stacks->addAction(tr("&Z Project...")), &QAction::triggered, this, [this]() { zProject(); });
        connect(stacks->addAction(tr("&Substack...")), &QAction::triggered, this, [this]() { makeSubstack(); });
        connect(stacks->addAction(tr("&Montage...")), &QAction::triggered, this, [this]() { makeMontage(); });
        connect(stacks->addAction(tr("Plot Z-axis &Profile")), &QAction::triggered, this, [this]() { plotZProfile(); });
        connect(stacks->addAction(tr("&Orthogonal Views")), &QAction::triggered, this, [this]() { showOrthogonalViews(); });
    }

    void ImageAnalysis::buildProcessMenu(QMenu* menu)
    {
        QMenu* filters = menu->addMenu(tr("&Filters"));
        addFilter(filters, tr("&Gaussian Blur..."), imageops::Filter::Gaussian, 2.0);
        addFilter(filters, tr("&Mean..."), imageops::Filter::Mean, 2.0);
        addFilter(filters, tr("M&edian..."), imageops::Filter::Median, 2.0);
        addFilter(filters, tr("Mi&nimum..."), imageops::Filter::Minimum, 2.0);
        addFilter(filters, tr("Ma&ximum..."), imageops::Filter::Maximum, 2.0);
        addFilter(filters, tr("&Variance..."), imageops::Filter::Variance, 2.0);
        connect(filters->addAction(tr("&Unsharp Mask...")), &QAction::triggered, this, [this]() { unsharpMask(); });
        connect(filters->addAction(tr("&Top Hat...")), &QAction::triggered, this, [this]() { topHat(); });
        connect(menu->addAction(tr("&Sharpen")), &QAction::triggered, this,
                [this]() { apply(tr("Sharpen"), [](const ImageFrame& f) { return imageops::sharpened(f); }); });
        connect(menu->addAction(tr("Find &Edges")), &QAction::triggered, this,
                [this]() { apply(tr("Find Edges"), [](const ImageFrame& f) { return imageops::findEdges(f); }); });
        connect(menu->addAction(tr("Subtract &Background...")), &QAction::triggered, this,
                [this]() { subtractBackground(); });

        QMenu* math = menu->addMenu(tr("&Math"));
        addMath(math, tr("&Add..."), imageops::MathOperation::Add, true, 25.0);
        addMath(math, tr("&Subtract..."), imageops::MathOperation::Subtract, true, 25.0);
        addMath(math, tr("&Multiply..."), imageops::MathOperation::Multiply, true, 1.25);
        addMath(math, tr("&Divide..."), imageops::MathOperation::Divide, true, 1.25);
        addMath(math, tr("M&inimum..."), imageops::MathOperation::Min, true, 0.0);
        addMath(math, tr("Ma&ximum..."), imageops::MathOperation::Max, true, 255.0);
        addMath(math, tr("&Gamma..."), imageops::MathOperation::Gamma, true, 0.5);
        addMath(math, tr("&Log"), imageops::MathOperation::Log, false, 0.0);
        addMath(math, tr("&Exp"), imageops::MathOperation::Exp, false, 0.0);
        addMath(math, tr("S&quare"), imageops::MathOperation::Square, false, 0.0);
        addMath(math, tr("Square &Root"), imageops::MathOperation::Sqrt, false, 0.0);
        addMath(math, tr("A&bsolute Value"), imageops::MathOperation::Abs, false, 0.0);

        connect(menu->addAction(tr("Image &Calculator...")), &QAction::triggered, this, [this]() { calculate(); });

        connect(menu->addAction(tr("Enhance &Contrast...")), &QAction::triggered, this, [this]() { enhanceContrast(); });
        connect(menu->addAction(tr("Enhance &Local Contrast (CLAHE)...")), &QAction::triggered, this,
                [this]() { enhanceLocalContrast(); });
        QMenu* noise = menu->addMenu(tr("&Noise"));
        connect(noise->addAction(tr("Add &Noise...")), &QAction::triggered, this, [this]() { addNoise(false); });
        connect(noise->addAction(tr("Add &Salt and Pepper...")), &QAction::triggered, this, [this]() { addNoise(true); });

        QMenu* binary = menu->addMenu(tr("Bin&ary"));
        connect(binary->addAction(tr("&Make Binary")), &QAction::triggered, this, [this]() { thresholdImage(); });
        addBinary(binary, tr("&Erode..."), imageops::BinaryOperation::Erode);
        addBinary(binary, tr("&Dilate..."), imageops::BinaryOperation::Dilate);
        addBinary(binary, tr("&Open..."), imageops::BinaryOperation::Open);
        addBinary(binary, tr("&Close..."), imageops::BinaryOperation::Close);
        addBinary(binary, tr("&Fill Holes"), imageops::BinaryOperation::FillHoles);
        addBinary(binary, tr("Out&line"), imageops::BinaryOperation::Outline);
        addBinary(binary, tr("&Invert"), imageops::BinaryOperation::Invert);
        binary->addSeparator();
        addBinary(binary, tr("&Watershed..."), imageops::BinaryOperation::Watershed);
        addBinary(binary, tr("&Skeletonize"), imageops::BinaryOperation::Skeletonize);
        addImageCommand(binary, tr("Distance &Map"), [](const ImageFrame& frame) { return imageops::distanceMap(frame); });
    }

    void ImageAnalysis::buildAnalyzeMenu(QMenu* menu)
    {
        QAction* measureAction = menu->addAction(tr("&Measure"));
        measureAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
        connect(measureAction, &QAction::triggered, this, [this]() { measure(); });
        connect(menu->addAction(tr("&Results")), &QAction::triggered, this, [this]()
        {
            if (!m_results)
            {
                m_results = new ResultsWindow(m_window);
            }
            m_results->show();
            m_results->raise();
        });
        connect(menu->addAction(tr("&Set Measurements...")), &QAction::triggered, this, [this]()
        {
            if (!m_results)
            {
                m_results = new ResultsWindow(m_window);
            }
            m_results->chooseMeasurements();
        });
        menu->addSeparator();
        QAction* managerAction = menu->addAction(tr("&ROI Manager"));
        managerAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
        connect(managerAction, &QAction::triggered, this, [this]()
        {
            if (!m_roiManager)
            {
                m_roiManager = new RoiManagerWindow(m_workspace, m_window);
                connect(m_roiManager, &RoiManagerWindow::measureRequested, this,
                        [this](const QList<ImageSceneModel::Markup>& shapes, bool allSlices)
                        { measureShapes(shapes, allSlices); });
            }
            m_roiManager->show();
            m_roiManager->raise();
        });
        connect(menu->addAction(tr("Plot &Z-axis Profile")), &QAction::triggered, this, [this]() { plotZProfile(); });
        menu->addSeparator();
        connect(menu->addAction(tr("Analyze &Particles...")), &QAction::triggered, this, [this]() { analyzeParticles(); });
        connect(menu->addAction(tr("&Wand Tolerance...")), &QAction::triggered, this, [this]() { wandTolerance(); });
    }

    // ---- Image menu

    void ImageAnalysis::duplicateImage()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const Scope scope = chooseScope(source, tr("Duplicate"));
        if (scope == Scope::Cancel)
        {
            return;
        }
        present(scope == Scope::All ? source.frames : QList<ImageFrame>{source.frames.at(source.index)},
                tr("Copy of %1").arg(source.title));
    }

    void ImageAnalysis::cropImage()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        ImageSceneModel::Markup shape;
        if (!selectedShape(source, shape))
        {
            QMessageBox::information(m_window, tr("Crop"), tr("Draw a selection to crop to."));
            return;
        }
        const QRect area = imageops::selectionBounds(shape);
        apply(tr("Crop"), [area](const ImageFrame& frame) { return imageops::crop(frame, area); });
    }

    void ImageAnalysis::scaleImage()
    {
        QList<ParameterField> fields{ParameterField::real(tr("X scale"), 0.5, 0.01, 20.0, 3),
                                     ParameterField::real(tr("Y scale"), 0.5, 0.01, 20.0, 3),
                                     ParameterField::check(tr("Interpolate"), true)};
        if (askParameters(m_window, tr("Scale"), fields))
        {
            const double x = fields.at(0).value;
            const double y = fields.at(1).value;
            const bool smooth = fields.at(2).checked;
            apply(tr("Scale"), [x, y, smooth](const ImageFrame& frame) { return imageops::scaled(frame, x, y, smooth); });
        }
    }

    void ImageAnalysis::rotateImage()
    {
        QList<ParameterField> fields{ParameterField::real(tr("Angle (degrees)"), 15.0, -360.0, 360.0, 1),
                                     ParameterField::check(tr("Interpolate"), true),
                                     ParameterField::check(tr("Enlarge image to fit result"), false)};
        if (askParameters(m_window, tr("Rotate"), fields))
        {
            const double angle = fields.at(0).value;
            const bool smooth = fields.at(1).checked;
            const bool grow = fields.at(2).checked;
            apply(tr("Rotate"), [angle, smooth, grow](const ImageFrame& frame)
            {
                return imageops::rotated(frame, angle, smooth, grow);
            });
        }
    }

    // Makes a binary image from the pixels that are above a limit found by one of the methods
    void ImageAnalysis::thresholdImage()
    {
        QList<ParameterField> fields{
            ParameterField::options(tr("Method"), {tr("IsoData"), tr("Otsu"), tr("Mean"), tr("Triangle")}, 1),
            ParameterField::check(tr("Dark objects on a light background"), false)};
        if (!askParameters(m_window, tr("Threshold"), fields))
        {
            return;
        }
        const auto method = static_cast<imageops::ThresholdMethod>(fields.at(0).choice);
        const bool darkObjects = fields.at(1).checked;
        apply(tr("Threshold"), [method, darkObjects](const ImageFrame& frame)
        {
            const int limit = imageops::autoThreshold(frame, method);
            return darkObjects ? imageops::thresholded(frame, 0, limit)
                               : imageops::thresholded(frame, limit + 1, imageops::maxValue(frame));
        });
    }

    // ---- Stacks

    void ImageAnalysis::zProject()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const int count = static_cast<int>(source.frames.size());
        QList<ParameterField> fields{
            ParameterField::integer(tr("Start slice"), 1, 1, count),
            ParameterField::integer(tr("Stop slice"), count, 1, count),
            ParameterField::options(tr("Projection"),
                                    {tr("Maximum Intensity"), tr("Average Intensity"), tr("Sum Slices"),
                                     tr("Minimum Intensity"), tr("Standard Deviation"), tr("Median")})};
        if (!askParameters(m_window, tr("Z Project"), fields))
        {
            return;
        }
        static const imageops::Projection methods[] = {
            imageops::Projection::Max, imageops::Projection::Average, imageops::Projection::Sum,
            imageops::Projection::Min, imageops::Projection::StdDev, imageops::Projection::Median};
        const imageops::Projection method = methods[fields.at(2).choice];
        present({imageops::zProject(source.frames, static_cast<int>(fields.at(0).value) - 1,
                                    static_cast<int>(fields.at(1).value) - 1, method)},
                tr("Projection of %1").arg(source.title));
    }

    void ImageAnalysis::makeSubstack()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const int count = static_cast<int>(source.frames.size());
        QList<ParameterField> fields{ParameterField::integer(tr("First slice"), 1, 1, count),
                                     ParameterField::integer(tr("Last slice"), count, 1, count),
                                     ParameterField::integer(tr("Step"), 1, 1, std::max(1, count))};
        if (askParameters(m_window, tr("Substack"), fields))
        {
            present(imageops::substack(source.frames, static_cast<int>(fields.at(0).value) - 1,
                                       static_cast<int>(fields.at(1).value) - 1,
                                       static_cast<int>(fields.at(2).value)),
                    tr("Substack of %1").arg(source.title));
        }
    }

    void ImageAnalysis::makeMontage()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const int count = static_cast<int>(source.frames.size());
        QList<ParameterField> fields{
            ParameterField::integer(tr("Columns"), static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))), 1, std::max(1, count)),
            ParameterField::real(tr("Scale"), 0.5, 0.05, 4.0, 2),
            ParameterField::integer(tr("Border width"), 2, 0, 100)};
        if (askParameters(m_window, tr("Montage"), fields))
        {
            present({imageops::montage(source.frames, static_cast<int>(fields.at(0).value), fields.at(1).value,
                                       static_cast<int>(fields.at(2).value))},
                    tr("Montage of %1").arg(source.title));
        }
    }

    void ImageAnalysis::plotZProfile()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        ImageSceneModel::Markup shape;
        const bool hasShape = selectedShape(source, shape);
        const QVector<double> values = imageops::zProfile(source.frames, hasShape ? &shape : nullptr,
                                                          imageops::ProfileStat::Mean);
        if (!m_profile)
        {
            m_profile = new ProfilePlotWindow(m_window);
        }
        m_profile->setProfile(tr("Z-axis profile of %1").arg(source.title), tr("Slice"), tr("Mean"), values);
        m_profile->show();
        m_profile->raise();
    }

    void ImageAnalysis::showOrthogonalViews()
    {
        Source source;
        if (!activeSource(source) || source.frames.size() < 2)
        {
            return;
        }
        OrthogonalViewsDialog dialog(source.frames, source.index, m_window);
        dialog.exec();
    }

    // ---- Process menu

    void ImageAnalysis::subtractBackground()
    {
        QList<ParameterField> fields{ParameterField::real(tr("Rolling ball radius (pixels)"), 50.0, 1.0, 500.0, 1),
                                     ParameterField::check(tr("Light background"), false)};
        if (askParameters(m_window, tr("Subtract Background"), fields))
        {
            const double radius = fields.at(0).value;
            const bool light = fields.at(1).checked;
            apply(tr("Subtract Background"), [radius, light](const ImageFrame& frame)
            {
                return imageops::subtractBackground(frame, radius, light);
            });
        }
    }

    void ImageAnalysis::unsharpMask()
    {
        QList<ParameterField> fields{ParameterField::real(tr("Radius (sigma, pixels)"), 1.0, 0.5, 200.0, 1),
                                     ParameterField::real(tr("Mask weight (0.1 to 0.9)"), 0.6, 0.1, 0.9, 2)};
        if (askParameters(m_window, tr("Unsharp Mask"), fields))
        {
            const double sigma = fields.at(0).value;
            const double weight = fields.at(1).value;
            apply(tr("Unsharp Mask"), [sigma, weight](const ImageFrame& frame)
            {
                return imageops::unsharpMask(frame, sigma, weight);
            });
        }
    }

    // ---- contrast

    // Stretches the contrast. Without Normalize the display range of the image is set and its data
    // stay as they are, and with it, or with Equalize, a new image is made.
    void ImageAnalysis::enhanceContrast()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        QList<ParameterField> fields{
            ParameterField::real(tr("Saturated pixels (%)"), 0.35, 0.0, 100.0, 2),
            ParameterField::check(tr("Normalize"), false),
            ParameterField::check(tr("Equalize histogram"), false),
            ParameterField::check(tr("Use the histogram of the whole stack"), false)};
        if (!askParameters(m_window, tr("Enhance Contrast"), fields))
        {
            return;
        }
        const double saturated = fields.at(0).value;
        const bool normalize = fields.at(1).checked;
        const bool equalize = fields.at(2).checked;
        const bool wholeStack = fields.at(3).checked && source.frames.size() > 1;
        const ImageFrame& current = source.frames.at(source.index);
        const std::vector<qint64> histogram = imageops::histogramOfFrames(
            wholeStack ? source.frames : QList<ImageFrame>{current});

        if (!normalize && !equalize)
        {
            const imageops::LevelRange range = imageops::saturatedRange(histogram, saturated);
            if (source.scene)
            {
                source.scene->setLayerAutoStretchEnabled(source.layerKey, false);
                source.scene->setLayerDisplayLevels(source.layerKey, range.low, range.high, imageops::maxValue(current));
            }
            return;
        }
        const Scope scope = chooseScope(source, tr("Enhance Contrast"));
        if (scope == Scope::Cancel)
        {
            return;
        }
        const imageops::LevelRange range = imageops::saturatedRange(histogram, saturated);
        const auto operation = [&](const ImageFrame& frame)
        {
            if (equalize)
            {
                return imageops::equalized(frame, true, wholeStack ? &histogram : nullptr);
            }
            return imageops::stretched(frame, range.low, range.high);
        };
        QList<ImageFrame> result;
        if (scope == Scope::All)
        {
            for (const ImageFrame& frame : source.frames)
            {
                result.append(operation(frame));
            }
        }
        else
        {
            result.append(operation(current));
        }
        present(result, tr("%1 (%2)").arg(equalize ? tr("Equalized") : tr("Normalized"), source.title));
    }

    void ImageAnalysis::enhanceLocalContrast()
    {
        QList<ParameterField> fields{ParameterField::integer(tr("Block size (pixels)"), 127, 8, 2000),
                                     ParameterField::integer(tr("Histogram bins"), 256, 16, 4096),
                                     ParameterField::real(tr("Maximum slope"), 3.0, 1.0, 100.0, 1)};
        if (askParameters(m_window, tr("Enhance Local Contrast"), fields))
        {
            const int block = static_cast<int>(fields.at(0).value);
            const int bins = static_cast<int>(fields.at(1).value);
            const double slope = fields.at(2).value;
            apply(tr("Enhance Local Contrast"), [block, bins, slope](const ImageFrame& frame)
            {
                return imageops::localContrast(frame, block, bins, slope);
            });
        }
    }

    // Writes the display range of the image into its data, so that what is shown is what is stored
    void ImageAnalysis::applyDisplayRange()
    {
        Source source;
        if (!activeSource(source) || !source.scene)
        {
            return;
        }
        scopeone::core::DocumentLayer layer;
        if (!source.scene->findLayer(source.layerKey, layer))
        {
            return;
        }
        const int low = layer.display.levelMin;
        const int high = layer.display.levelMax;
        apply(tr("Apply Display Range"), [low, high](const ImageFrame& frame)
        {
            return imageops::stretched(frame, low, high);
        });
    }

    void ImageAnalysis::addNoise(bool saltAndPepper)
    {
        QList<ParameterField> fields{
            saltAndPepper ? ParameterField::real(tr("Percent of pixels"), 2.5, 0.0, 100.0, 2)
                          : ParameterField::real(tr("Standard deviation"), 25.0, 0.0, 100000.0, 1)};
        if (askParameters(m_window, saltAndPepper ? tr("Add Salt and Pepper") : tr("Add Noise"), fields))
        {
            const double amount = fields.first().value;
            apply(saltAndPepper ? tr("Salt and Pepper") : tr("Add Noise"), [amount, saltAndPepper](const ImageFrame& frame)
            {
                return saltAndPepper ? imageops::withSaltAndPepper(frame, amount) : imageops::withGaussianNoise(frame, amount);
            });
        }
    }

    void ImageAnalysis::topHat()
    {
        QList<ParameterField> fields{ParameterField::real(tr("Radius (pixels)"), 10.0, 1.0, 200.0, 1),
                                     ParameterField::check(tr("Light background"), false)};
        if (askParameters(m_window, tr("Top Hat"), fields))
        {
            const double radius = fields.at(0).value;
            const bool light = fields.at(1).checked;
            apply(tr("Top Hat"), [radius, light](const ImageFrame& frame) { return imageops::topHat(frame, radius, light); });
        }
    }

    void ImageAnalysis::calculate()
    {
        QStringList titles;
        QStringList ids;
        for (const ImageDocumentInfo& document : m_workspace->documents())
        {
            titles.append(document.title);
            ids.append(document.id);
        }
        if (titles.size() < 2)
        {
            QMessageBox::information(m_window, tr("Image Calculator"), tr("Two images have to be open."));
            return;
        }
        QList<ParameterField> fields{
            ParameterField::options(tr("Image 1"), titles, 0),
            ParameterField::options(tr("Operation"),
                                    {tr("Add"), tr("Subtract"), tr("Multiply"), tr("Divide"), tr("AND"), tr("OR"),
                                     tr("XOR"), tr("Min"), tr("Max"), tr("Average"), tr("Difference")}),
            ParameterField::options(tr("Image 2"), titles, 1)};
        if (!askParameters(m_window, tr("Image Calculator"), fields))
        {
            return;
        }
        const ImageFrame first = m_workspace->currentFrame(ids.at(fields.at(0).choice));
        const ImageFrame second = m_workspace->currentFrame(ids.at(fields.at(2).choice));
        if (!imageops::isMono(first) || !imageops::isMono(second))
        {
            return;
        }
        present({imageops::calculate(first, second, static_cast<imageops::CalculatorOperation>(fields.at(1).choice))},
                tr("Result of %1").arg(fields.at(1).choices.at(fields.at(1).choice)));
    }

    // ---- Analyze menu

    void ImageAnalysis::measure()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        ImageSceneModel::Markup shape;
        if (!selectedShape(source, shape))
        {
            // Without a selection the whole image is measured, as in ImageJ
            shape.type = ImageSceneModel::MarkupType::Rect;
            shape.rect = QRect(0, 0, source.frames.first().width, source.frames.first().height);
            shape.label = source.title;
        }
        measureShapes({shape}, false);
    }

    // ---- selections

    bool ImageAnalysis::requireSelection(Source& source, ImageSceneModel::Markup& shape)
    {
        if (!activeSource(source))
        {
            return false;
        }
        if (!selectedShape(source, shape))
        {
            QMessageBox::information(m_window, tr("Selection"), tr("Draw a selection first."));
            return false;
        }
        return true;
    }

    void ImageAnalysis::replaceWithPolygon(const Source& source, const ImageSceneModel::Markup& old,
                                           const QList<QPoint>& corners)
    {
        if (corners.size() < 3)
        {
            QMessageBox::information(m_window, tr("Selection"), tr("Nothing is left of the selection."));
            return;
        }
        source.scene->remove(old.id);
        const QString id = source.scene->createPolygon(source.layerKey, corners, old.label);
        source.scene->selectOnly(id);
    }

    void ImageAnalysis::selectAll()
    {
        Source source;
        if (!activeSource(source) || !source.scene)
        {
            return;
        }
        const ImageFrame& frame = source.frames.first();
        const QString id = source.scene->createRect(source.layerKey, QRect(0, 0, frame.width, frame.height));
        source.scene->selectOnly(id);
    }

    void ImageAnalysis::specifySelection()
    {
        Source source;
        if (!activeSource(source) || !source.scene)
        {
            return;
        }
        const ImageFrame& frame = source.frames.first();
        ImageSceneModel::Markup shape;
        const bool hasShape = selectedShape(source, shape)
            && (shape.type == ImageSceneModel::MarkupType::Rect || shape.type == ImageSceneModel::MarkupType::Ellipse);
        const QRect current = hasShape ? shape.rect.normalized() : QRect(0, 0, frame.width / 2, frame.height / 2);
        QList<ParameterField> fields{
            ParameterField::integer(tr("Width"), current.width(), 1, frame.width),
            ParameterField::integer(tr("Height"), current.height(), 1, frame.height),
            ParameterField::integer(tr("X coordinate"), current.x(), 0, frame.width),
            ParameterField::integer(tr("Y coordinate"), current.y(), 0, frame.height),
            ParameterField::check(tr("Oval"), hasShape && shape.type == ImageSceneModel::MarkupType::Ellipse)};
        if (!askParameters(m_window, tr("Specify"), fields))
        {
            return;
        }
        const QRect rect(static_cast<int>(fields.at(2).value), static_cast<int>(fields.at(3).value),
                         static_cast<int>(fields.at(0).value), static_cast<int>(fields.at(1).value));
        if (hasShape)
        {
            source.scene->remove(shape.id);
        }
        const QString id = fields.at(4).checked ? source.scene->createEllipse(source.layerKey, rect)
                                                : source.scene->createRect(source.layerKey, rect);
        source.scene->selectOnly(id);
    }

    void ImageAnalysis::enlargeSelection()
    {
        Source source;
        ImageSceneModel::Markup shape;
        if (!requireSelection(source, shape))
        {
            return;
        }
        QList<ParameterField> fields{ParameterField::real(tr("Enlarge by (pixels, negative to shrink)"), 15.0, -500.0, 500.0, 1)};
        if (askParameters(m_window, tr("Enlarge"), fields))
        {
            replaceWithPolygon(source, shape,
                               imageops::enlargedOutline(source.frames.first().width, source.frames.first().height,
                                                         shape, fields.first().value));
        }
    }

    void ImageAnalysis::rotateSelection()
    {
        Source source;
        ImageSceneModel::Markup shape;
        if (!requireSelection(source, shape))
        {
            return;
        }
        QList<ParameterField> fields{ParameterField::real(tr("Angle (degrees)"), 15.0, -360.0, 360.0, 1)};
        if (askParameters(m_window, tr("Rotate Selection"), fields))
        {
            replaceWithPolygon(source, shape, imageops::rotatedCorners(shape, fields.first().value));
        }
    }

    void ImageAnalysis::scaleSelection()
    {
        Source source;
        ImageSceneModel::Markup shape;
        if (!requireSelection(source, shape))
        {
            return;
        }
        QList<ParameterField> fields{ParameterField::real(tr("X scale factor"), 1.5, 0.05, 20.0, 2),
                                     ParameterField::real(tr("Y scale factor"), 1.5, 0.05, 20.0, 2)};
        if (askParameters(m_window, tr("Scale Selection"), fields))
        {
            replaceWithPolygon(source, shape, imageops::scaledCorners(shape, fields.at(0).value, fields.at(1).value));
        }
    }

    // Makes an image in which the pixels of the selection are 255 and the others 0
    void ImageAnalysis::createMask()
    {
        Source source;
        ImageSceneModel::Markup shape;
        if (!requireSelection(source, shape))
        {
            return;
        }
        ImageFrame mask = imageops::selectionMask(source.frames.first().width, source.frames.first().height, shape);
        mask.cameraId = source.frames.first().cameraId;
        present({mask}, tr("Mask of %1").arg(source.title));
    }

    // Makes a selection of every group of touching pixels above zero of a binary image
    void ImageAnalysis::createSelection()
    {
        Source source;
        if (!activeSource(source) || !source.scene)
        {
            return;
        }
        QSignalBlocker blocker(source.scene);
        int number = 0;
        for (const imageops::Particle& particle : imageops::findParticles(source.frames.at(source.index)))
        {
            source.scene->createPolygon(source.layerKey, particle.outline, tr("Selection %1").arg(++number));
        }
        blocker.unblock();
        emit source.scene->markupsChanged();
    }

    // The size of a pixel of the image
    void ImageAnalysis::imageProperties()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const ImageFrame& frame = source.frames.first();
        QList<ParameterField> fields{
            ParameterField::real(tr("Pixel size (µm)"), m_workspace->pixelSizeUm(source.layerKey), 0.0, 1.0e6, 4)};
        if (!askParameters(m_window, tr("Properties of %1").arg(source.title), fields))
        {
            return;
        }
        const double size = fields.first().value;
        const ImageDocumentInfo document = m_workspace->document();
        if (document.isValid())
        {
            if (const auto session = m_workspace->documentSession())
            {
                session->setPixelSizeUm(document.cameraId, size);
            }
        }
        else
        {
            m_core->setCameraPixelSizeUm(frame.cameraId, size);
        }
    }

    void ImageAnalysis::wandTolerance()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        QList<ParameterField> fields{
            ParameterField::integer(tr("Tolerance (gray levels)"), settings.value(QStringLiteral("Wand/tolerance"), 0).toInt(),
                                    0, 65535)};
        if (askParameters(m_window, tr("Wand Tolerance"), fields))
        {
            settings.setValue(QStringLiteral("Wand/tolerance"), static_cast<int>(fields.first().value));
        }
    }

    // Finds the particles of a binary image, or of an image above a threshold, and measures each
    // one, like Analyze Particles of ImageJ
    void ImageAnalysis::analyzeParticles()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        const ImageFrame image = source.frames.at(source.index);
        // A binary image has nothing but zero and one value above it
        bool binary = true;
        for (int y = 0; binary && y < image.height; ++y)
        {
            for (int x = 0; binary && x < image.width; ++x)
            {
                const int value = imageops::pixelAt(image, x, y);
                binary = value == 0 || value == imageops::maxValue(image) || value == 255;
            }
        }
        QList<ParameterField> fields{
            ParameterField::real(tr("Smallest size (pixels)"), 0.0, 0.0, 1.0e9, 0),
            ParameterField::real(tr("Largest size (pixels)"), 1.0e9, 0.0, 1.0e9, 0),
            ParameterField::real(tr("Smallest circularity"), 0.0, 0.0, 1.0, 2),
            ParameterField::real(tr("Largest circularity"), 1.0, 0.0, 1.0, 2),
            ParameterField::options(tr("Threshold, for an image that is not binary"),
                                    {tr("IsoData"), tr("Otsu"), tr("Mean"), tr("Triangle")}, 1),
            ParameterField::check(tr("Dark objects on a light background"), false),
            ParameterField::check(tr("Exclude particles on the edges"), false),
            ParameterField::check(tr("Include holes"), false),
            ParameterField::check(tr("Display results"), true),
            ParameterField::check(tr("Add to ROI Manager"), false),
            ParameterField::options(tr("Show"), {tr("Nothing"), tr("Masks"), tr("Count Masks")}, 0)};
        if (!askParameters(m_window, tr("Analyze Particles"), fields))
        {
            return;
        }
        ImageFrame mask = image;
        if (!binary)
        {
            const int limit = imageops::autoThreshold(image, static_cast<imageops::ThresholdMethod>(fields.at(4).choice));
            mask = fields.at(5).checked ? imageops::thresholded(image, 0, limit)
                                        : imageops::thresholded(image, limit + 1, imageops::maxValue(image));
        }
        const double minSize = fields.at(0).value;
        const double maxSize = fields.at(1).value;
        const double minCircularity = fields.at(2).value;
        const double maxCircularity = fields.at(3).value;
        const bool excludeEdges = fields.at(6).checked;
        const bool includeHoles = fields.at(7).checked;
        const bool display = fields.at(8).checked;
        const bool addToManager = fields.at(9).checked;
        const int show = fields.at(10).choice;

        const double scale = pixelScale(source);
        const double unit = scale > 0.0 ? scale : 1.0;
        const QString unitName = scale > 0.0 ? QStringLiteral("µm") : QStringLiteral("pixel");
        if (display && !m_results)
        {
            m_results = new ResultsWindow(m_window);
        }

        QList<imageops::Particle> kept;
        QSignalBlocker blocker(addToManager ? source.scene : nullptr);
        for (const imageops::Particle& particle : imageops::findParticles(mask))
        {
            if (excludeEdges && particle.touchesEdge)
            {
                continue;
            }
            const imageops::Measurement result = imageops::measureParticle(image, particle, scale, includeHoles);
            const double size = result.area / (unit * unit);
            if (size < minSize || size > maxSize || result.circularity < minCircularity
                || result.circularity > maxCircularity)
            {
                continue;
            }
            kept.append(particle);
            const QString label = tr("Particle %1").arg(kept.size());
            if (display)
            {
                m_results->addMeasurement(label, result, source.frames.size() > 1 ? source.index + 1 : 0, unitName);
            }
            if (addToManager && source.scene)
            {
                source.scene->createPolygon(source.layerKey, particle.outline, label);
            }
        }
        if (addToManager && source.scene)
        {
            blocker.unblock();
            emit source.scene->markupsChanged();
        }
        if (show != 0 && !kept.isEmpty())
        {
            ImageFrame shown = imageops::particleImage(kept, image.width, image.height, show == 2);
            shown.cameraId = image.cameraId;
            present({shown}, tr("%1 of %2").arg(show == 2 ? tr("Count Masks") : tr("Masks"), source.title));
        }
        QMessageBox::information(m_window, tr("Analyze Particles"), tr("%1 particles").arg(kept.size()));
    }

    void ImageAnalysis::measureShapes(const QList<ImageSceneModel::Markup>& shapes, bool allSlices)
    {
        Source source;
        if (shapes.isEmpty() || !activeSource(source))
        {
            return;
        }
        if (!m_results)
        {
            m_results = new ResultsWindow(m_window);
        }
        const double scale = pixelScale(source);
        const QString unit = scale > 0.0 ? QStringLiteral("µm") : QStringLiteral("pixel");
        const int first = allSlices ? 0 : source.index;
        const int last = allSlices ? static_cast<int>(source.frames.size()) - 1 : source.index;
        for (const ImageSceneModel::Markup& shape : shapes)
        {
            const QString label = shape.label.isEmpty()
                ? QStringLiteral("%1:%2").arg(source.title, ImageSceneModel::typeName(shape.type))
                : QStringLiteral("%1:%2").arg(source.title, shape.label);
            for (int index = first; index <= last; ++index)
            {
                m_results->addMeasurement(label,
                                          imageops::measure(source.frames.at(index), shape, scale),
                                          source.frames.size() > 1 ? index + 1 : 0,
                                          unit);
            }
        }
    }
}
