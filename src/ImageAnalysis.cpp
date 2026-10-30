#include "ImageAnalysis.h"

#include "ImageWorkspace.h"
#include "OrthogonalViewsDialog.h"
#include "ParameterDialog.h"
#include "ProfilePlotWindow.h"
#include "ResultsWindow.h"
#include "RoiManagerWindow.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QProgressDialog>
#include <QImage>
#include <QPainter>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSignalBlocker>

#include <limits>
#include <memory>

namespace scopeone::ui
{
    namespace
    {
        // A gray picture of a frame, with the values from low to high spread over the gray levels
        QImage grayPicture(const scopeone::core::ImageFrame& frame, int low, int high)
        {
            QImage picture(frame.width, frame.height, QImage::Format_Grayscale8);
            const double scale = high > low ? 255.0 / (high - low) : 1.0;
            for (int y = 0; y < frame.height; ++y)
            {
                uchar* row = picture.scanLine(y);
                for (int x = 0; x < frame.width; ++x)
                {
                    row[x] = static_cast<uchar>(std::clamp((core::imageops::pixelAt(frame, x, y) - low) * scale, 0.0, 255.0));
                }
            }
            return picture;
        }

        // The middle part of the frame, which is what the preview of a command works on
        scopeone::core::ImageFrame previewPart(const scopeone::core::ImageFrame& frame)
        {
            const int width = std::min(frame.width, 420);
            const int height = std::min(frame.height, 320);
            return core::imageops::crop(frame, QRect((frame.width - width) / 2, (frame.height - height) / 2, width, height));
        }
    } // namespace

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

    // Shows a panel in the place the window gives it, or as a window of its own without one
    void ImageAnalysis::showPanel(QWidget* panel, const QString& title)
    {
        if (m_panelPresenter)
        {
            m_panelPresenter(panel, title);
            return;
        }
        panel->show();
        panel->raise();
    }

    ResultsWindow* ImageAnalysis::results()
    {
        if (!m_results)
        {
            m_results = new ResultsWindow(m_window);
            connect(m_results, &ResultsWindow::attentionRequested, this,
                    [this]() { showPanel(m_results, tr("Results")); });
        }
        return m_results;
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
        m_menus = {edit, image, process, analyze};
        bindShortcuts();
    }

    // Gives the commands the keys that ImageJ gives them
    void ImageAnalysis::bindShortcuts()
    {
        struct Binding
        {
            int menu;
            const char* name;
            const char* keys;
        };
        const Binding bindings[] = {
            {1, "Duplicate...", "Ctrl+Shift+D"},
            {1, "Crop", "Ctrl+Shift+X"},
            {1, "Scale...", "Ctrl+E"},
            {1, "Properties...", "Ctrl+Shift+P"},
            {1, "Threshold...", "Ctrl+Shift+T"},
            {1, "Orthogonal Views", "Ctrl+Shift+H"},
        };
        for (const Binding& binding : bindings)
        {
            if (QAction* action = findAction(m_menus.at(binding.menu), QString::fromLatin1(binding.name)))
            {
                action->setShortcut(QKeySequence(QString::fromLatin1(binding.keys)));
            }
        }
    }

    QAction* ImageAnalysis::findAction(QMenu* menu, const QString& name)
    {
        for (QAction* action : menu->actions())
        {
            QString text = action->text();
            text.remove(QLatin1Char('&'));
            if (text == name)
            {
                return action;
            }
            if (action->menu())
            {
                if (QAction* found = findAction(action->menu(), name))
                {
                    return found;
                }
            }
        }
        return nullptr;
    }

    // The commands that are used most on an image, in a menu where the user clicked it
    void ImageAnalysis::showContextMenu(const QPoint& globalPosition)
    {
        QMenu menu(m_window);
        const QStringList names{QStringLiteral("Duplicate..."), QStringLiteral("Crop"), QStringLiteral("Properties..."),
                                QString(), QStringLiteral("Measure"), QStringLiteral("Threshold..."),
                                QStringLiteral("Analyze Particles..."), QStringLiteral("ROI Manager")};
        for (const QString& name : names)
        {
            if (name.isEmpty())
            {
                menu.addSeparator();
                continue;
            }
            for (QMenu* source : m_menus)
            {
                if (QAction* action = findAction(source, name))
                {
                    menu.addAction(action);
                    break;
                }
            }
        }
        if (!m_contextActions.isEmpty())
        {
            menu.addSeparator();
            for (QAction* action : m_contextActions)
            {
                menu.addAction(action);
            }
        }
        menu.exec(globalPosition);
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
        if (source.frames.isEmpty() || !core::imageops::isMono(source.frames.first()))
        {
            QMessageBox::information(m_window, tr("No image"), tr("There is no 8 bit or 16 bit gray image to work on."));
            return false;
        }
        const int planes = static_cast<int>(source.frames.size());
        const scopeone::core::StackShape shape = m_workspace->layerStackShape(source.layerKey);
        if (shape.channels * shape.slices * shape.frames == planes && planes > 1)
        {
            // The file says which axes it has, one or several
            source.shape = shape;
            source.known = true;
            source.hyper = shape.isHyperstack();
        }
        else
        {
            source.shape = scopeone::core::StackShape{};
            source.shape.frames = planes;
        }
        return true;
    }

    // The names of the axes that have more than one step, and their letters
    QStringList ImageAnalysis::axisChoices(const Source& source, QList<QChar>& letters)
    {
        letters = stack::activeAxes(source.shape);
        if (letters.isEmpty())
        {
            letters = {QLatin1Char('T')};
        }
        QStringList names;
        for (const QChar letter : letters)
        {
            names << stack::axisName(letter, !source.known);
        }
        return names;
    }

    // The slices along an axis, the other axes being where the current slice has them
    QList<scopeone::core::ImageFrame> ImageAnalysis::framesAlong(const Source& source, QChar axis, int* position)
    {
        QList<scopeone::core::ImageFrame> frames;
        const QList<int> planes = stack::planesAlong(source.shape, axis, source.index);
        for (const int plane : planes)
        {
            frames.append(source.frames.at(plane));
        }
        if (position)
        {
            *position = std::max(0, static_cast<int>(planes.indexOf(source.index)));
        }
        return frames;
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
        box.setText(source.hyper
                        ? tr("Process all %1 planes (%2 channels, %3 slices, %4 frames)?")
                              .arg(source.frames.size()).arg(source.shape.channels).arg(source.shape.slices)
                              .arg(source.shape.frames)
                        : tr("Process all %1 slices?").arg(source.frames.size()));
        QPushButton* all = box.addButton(source.hyper ? tr("All planes") : tr("All slices"), QMessageBox::YesRole);
        QPushButton* current = box.addButton(source.hyper ? tr("This plane") : tr("This slice"), QMessageBox::NoRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() == all)
        {
            return Scope::All;
        }
        return box.clickedButton() == current ? Scope::Current : Scope::Cancel;
    }

    void ImageAnalysis::present(const QList<ImageFrame>& frames, const QString& title,
                                const scopeone::core::StackShape* shape)
    {
        if (frames.isEmpty() || !m_presenter)
        {
            return;
        }
        scopeone::core::ExperimentPlan plan;
        if (shape && shape->isHyperstack()
            && shape->channels * shape->slices * shape->frames == static_cast<int>(frames.size()))
        {
            plan.stackChannels = shape->channels;
            plan.stackSlices = shape->slices;
            plan.stackFrames = shape->frames;
            plan.stackOrder = shape->order;
        }
        const auto session = m_core->createFrameSession(frames, plan);
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
        present(result, QStringLiteral("%1 (%2)").arg(title, source.title), scope == Scope::All ? &source.shape : nullptr);
    }

    // ---- menus

    // The preview of a dialog: the middle of the current slice before and after the operation
    ImageAnalysis::PreviewRenderer ImageAnalysis::makePreview(
        const Source& source,
        const std::function<ImageFrame(const ImageFrame&, const QList<ParameterField>&)>& operation) const
    {
        const ImageFrame part = previewPart(source.frames.at(source.index));
        int low = std::numeric_limits<int>::max();
        int high = 0;
        for (int y = 0; y < part.height; ++y)
        {
            for (int x = 0; x < part.width; ++x)
            {
                const int value = core::imageops::pixelAt(part, x, y);
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
        // Small images are enlarged, with sharp pixels, so that the preview is not tiny
        const int zoom = std::max(1, std::min(3, std::min(400 / std::max(1, part.width), 300 / std::max(1, part.height))));
        const auto enlarge = [zoom](const QImage& picture)
        {
            return zoom > 1 ? picture.scaled(picture.size() * zoom, Qt::KeepAspectRatio, Qt::FastTransformation) : picture;
        };
        const QImage before = enlarge(grayPicture(part, low, high));
        return [operation, before, part, low, high, enlarge](const QList<ParameterField>& values)
        {
            const ImageFrame after = operation(part, values);
            if (!after.isValid())
            {
                return before;
            }
            const bool sameRange = after.isMono16() == part.isMono16();
            const QImage changed = enlarge(sameRange ? grayPicture(after, low, high) : grayPicture(after, 0, 255));
            QImage both(before.width() + changed.width() + 8, std::max(before.height(), changed.height()),
                        QImage::Format_Grayscale8);
            both.fill(64);
            QPainter painter(&both);
            painter.drawImage(0, 0, before);
            painter.drawImage(before.width() + 8, 0, changed);
            return both;
        };
    }

    // Asks for the settings of an operation on one slice, with a preview of what it does
    bool ImageAnalysis::askWithPreview(
        const QString& title, QList<ParameterField>& fields,
        const std::function<ImageFrame(const ImageFrame&, const QList<ParameterField>&)>& operation)
    {
        Source source;
        if (!activeSource(source))
        {
            return false;
        }
        return askParameters(m_window, title, fields, makePreview(source, operation));
    }

    // Runs a processing module once on the image or on every slice, with the parameters in preset and
    // the ones in askKeys asked for in a dialog that follows the descriptor of the module
    void ImageAnalysis::runModule(const QString& menuText, const QString& moduleId, const QVariantMap& preset,
                                  const QStringList& askKeys)
    {
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
        QVariantMap parameters = preset;
        if (!askKeys.isEmpty())
        {
            scopeone::core::ProcessingModuleDescriptor descriptor;
            for (const auto& candidate : m_core->availableProcessingModules())
            {
                if (candidate.id == moduleId)
                {
                    descriptor = candidate;
                }
            }
            QList<ParameterField> fields;
            QList<scopeone::core::ProcessingParameterDescriptor> asked;
            for (const QString& key : askKeys)
            {
                for (const auto& parameter : descriptor.parameters)
                {
                    if (parameter.key != key)
                    {
                        continue;
                    }
                    const QVariant start = preset.contains(key) ? preset.value(key) : parameter.defaultValue;
                    switch (parameter.type)
                    {
                    case scopeone::core::ProcessingParameterType::Integer:
                        fields.append(ParameterField::integer(parameter.name, start.toInt(),
                                                              parameter.minimum.toInt(), parameter.maximum.toInt()));
                        break;
                    case scopeone::core::ProcessingParameterType::Real:
                        fields.append(ParameterField::real(parameter.name, start.toDouble(), parameter.minimum.toDouble(),
                                                           parameter.maximum.toDouble(), parameter.decimals));
                        break;
                    case scopeone::core::ProcessingParameterType::Choice:
                    {
                        QStringList names;
                        for (const auto& choice : parameter.choices)
                        {
                            names << choice.name;
                        }
                        fields.append(ParameterField::options(parameter.name, names, start.toInt()));
                        break;
                    }
                    default:
                        fields.append(ParameterField::check(parameter.name, start.toBool()));
                        break;
                    }
                    asked.append(parameter);
                }
            }
            const auto collect = [&preset, &asked](const QList<ParameterField>& values)
            {
                QVariantMap result = preset;
                for (int i = 0; i < asked.size(); ++i)
                {
                    const ParameterField& field = values.at(i);
                    switch (field.kind)
                    {
                    case ParameterField::Kind::Integer: result.insert(asked.at(i).key, static_cast<int>(field.value)); break;
                    case ParameterField::Kind::Real: result.insert(asked.at(i).key, field.value); break;
                    case ParameterField::Kind::Choice: result.insert(asked.at(i).key, field.choice); break;
                    case ParameterField::Kind::Check: result.insert(asked.at(i).key, field.checked); break;
                    }
                }
                return result;
            };
            auto previewModule = std::shared_ptr<scopeone::core::ProcessingModule>(
                m_core->createProcessingModule(moduleId).release());
            const PreviewRenderer renderer = makePreview(
                source, [&collect, previewModule](const ImageFrame& part, const QList<ParameterField>& values)
                {
                    previewModule->setParameters(collect(values));
                    return previewModule->process(part, 16).frame;
                });
            const FieldsAction toPipeline = [this, &collect, moduleId](const QList<ParameterField>& values)
            {
                emit pipelineStepRequested(moduleId, collect(values));
            };
            if (!askParameters(m_window, title, fields, renderer, toPipeline))
            {
                return;
            }
            parameters = collect(fields);
        }
        std::shared_ptr<scopeone::core::ProcessingModule> module(m_core->createProcessingModule(moduleId).release());
        module->setParameters(parameters);
        apply(title, [module](const ImageFrame& frame) { return module->process(frame, 16).frame; });
    }

    void ImageAnalysis::addFilter(QMenu* menu, const QString& text, int kind, double radius)
    {
        connect(menu->addAction(text), &QAction::triggered, this, [this, text, kind, radius]()
        {
            runModule(text, QStringLiteral("filter"), {{"kind", kind}, {"radius", radius}}, {QStringLiteral("radius")});
        });
    }

    void ImageAnalysis::addMath(QMenu* menu, const QString& text, int operation, bool needsValue, double value)
    {
        connect(menu->addAction(text), &QAction::triggered, this, [this, text, operation, needsValue, value]()
        {
            runModule(text, QStringLiteral("math"), {{"operation", operation}, {"value", value}},
                      needsValue ? QStringList{QStringLiteral("value")} : QStringList{});
        });
    }

    void ImageAnalysis::addModuleCommand(QMenu* menu, const QString& text, const QString& moduleId,
                                         const QVariantMap& preset, const QStringList& askKeys)
    {
        connect(menu->addAction(text), &QAction::triggered, this,
                [this, text, moduleId, preset, askKeys]() { runModule(text, moduleId, preset, askKeys); });
    }

    void ImageAnalysis::addBinary(QMenu* menu, const QString& text, BinaryOperation operation)
    {
        connect(menu->addAction(text), &QAction::triggered, this, [this, text, operation]()
        {
            using Operation = BinaryOperation;
            if (operation == Operation::Watershed)
            {
                runModule(text, QStringLiteral("watershed"), {}, {QStringLiteral("tolerance")});
                return;
            }
            // The number of the operation in the binary module
            int number = 0;
            switch (operation)
            {
            case Operation::Erode: number = 0; break;
            case Operation::Dilate: number = 1; break;
            case Operation::Open: number = 2; break;
            case Operation::Close: number = 3; break;
            case Operation::Outline: number = 4; break;
            case Operation::Invert: number = 5; break;
            case Operation::FillHoles: number = 6; break;
            default: number = 7; break;
            }
            const bool repeats = number <= 3;
            runModule(text, QStringLiteral("binary"), {{"operation", number}, {"iterations", 1}},
                      repeats ? QStringList{QStringLiteral("iterations")} : QStringList{});
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
                const QRect box = core::imageops::selectionBounds(shape);
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
                replaceWithPolygon(source, shape, core::imageops::convexHull(core::imageops::selectionCorners(shape)));
            }
        });
        connect(selection->addAction(tr("Fit &Ellipse")), &QAction::triggered, this, [this]()
        {
            Source source;
            ImageSceneModel::Markup shape;
            if (requireSelection(source, shape))
            {
                replaceWithPolygon(source, shape, core::imageops::fittedEllipse(source.frames.first().width,
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
        addModuleCommand(transform, tr("Flip &Horizontally"), QStringLiteral("flip"), {{"direction", 0}}, {});
        addModuleCommand(transform, tr("Flip &Vertically"), QStringLiteral("flip"), {{"direction", 1}}, {});
        addModuleCommand(transform, tr("Rotate 90 Degrees &Right"), QStringLiteral("rotate_90"), {{"direction", 0}}, {});
        addModuleCommand(transform, tr("Rotate 90 Degrees &Left"), QStringLiteral("rotate_90"), {{"direction", 1}}, {});
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
        addFilter(filters, tr("&Gaussian Blur..."), 4, 2.0);
        addFilter(filters, tr("&Mean..."), 0, 2.0);
        addFilter(filters, tr("M&edian..."), 1, 2.0);
        addFilter(filters, tr("Mi&nimum..."), 2, 2.0);
        addFilter(filters, tr("Ma&ximum..."), 3, 2.0);
        addFilter(filters, tr("&Variance..."), 5, 2.0);
        addModuleCommand(filters, tr("&Unsharp Mask..."), QStringLiteral("unsharp_mask"), {},
                         {QStringLiteral("sigma"), QStringLiteral("weight")});
        addModuleCommand(filters, tr("&Top Hat..."), QStringLiteral("top_hat"), {},
                         {QStringLiteral("radius"), QStringLiteral("light_background")});
        addModuleCommand(menu, tr("&Sharpen"), QStringLiteral("sharpen"), {}, {});
        addModuleCommand(menu, tr("Find &Edges"), QStringLiteral("find_edges"), {}, {});
        connect(menu->addAction(tr("Subtract &Background...")), &QAction::triggered, this,
                [this]() { subtractBackground(); });

        QMenu* math = menu->addMenu(tr("&Math"));
        addMath(math, tr("&Add..."), 0, true, 25.0);
        addMath(math, tr("&Subtract..."), 1, true, 25.0);
        addMath(math, tr("&Multiply..."), 2, true, 1.25);
        addMath(math, tr("&Divide..."), 3, true, 1.25);
        addMath(math, tr("M&inimum..."), 4, true, 0.0);
        addMath(math, tr("Ma&ximum..."), 5, true, 255.0);
        addMath(math, tr("&Gamma..."), 6, true, 0.5);
        addMath(math, tr("&Log"), 7, false, 0.0);
        addMath(math, tr("&Exp"), 8, false, 0.0);
        addMath(math, tr("S&quare"), 9, false, 0.0);
        addMath(math, tr("Square &Root"), 10, false, 0.0);

        connect(menu->addAction(tr("Image &Calculator...")), &QAction::triggered, this, [this]() { calculate(); });

        connect(menu->addAction(tr("Enhance &Contrast...")), &QAction::triggered, this, [this]() { enhanceContrast(); });
        connect(menu->addAction(tr("Enhance &Local Contrast (CLAHE)...")), &QAction::triggered, this,
                [this]() { enhanceLocalContrast(); });
        QMenu* noise = menu->addMenu(tr("&Noise"));
        connect(noise->addAction(tr("Add &Noise...")), &QAction::triggered, this, [this]() { addNoise(false); });
        connect(noise->addAction(tr("Add &Salt and Pepper...")), &QAction::triggered, this, [this]() { addNoise(true); });

        QMenu* binary = menu->addMenu(tr("Bin&ary"));
        connect(binary->addAction(tr("&Make Binary")), &QAction::triggered, this, [this]() { thresholdImage(); });
        addBinary(binary, tr("&Erode..."), BinaryOperation::Erode);
        addBinary(binary, tr("&Dilate..."), BinaryOperation::Dilate);
        addBinary(binary, tr("&Open..."), BinaryOperation::Open);
        addBinary(binary, tr("&Close..."), BinaryOperation::Close);
        addBinary(binary, tr("&Fill Holes"), BinaryOperation::FillHoles);
        addBinary(binary, tr("Out&line"), BinaryOperation::Outline);
        addBinary(binary, tr("&Invert"), BinaryOperation::Invert);
        binary->addSeparator();
        addBinary(binary, tr("&Watershed..."), BinaryOperation::Watershed);
        addBinary(binary, tr("&Skeletonize"), BinaryOperation::Skeletonize);
        addModuleCommand(binary, tr("Distance &Map"), QStringLiteral("distance_map"), {}, {});

        menu->addSeparator();
        connect(menu->addAction(tr("Run Pipeline on a &Folder...")), &QAction::triggered, this, [this]() { batchProcess(); });
    }

    // Runs the steps of the Process panel on every image file of a folder and saves the results
    void ImageAnalysis::batchProcess()
    {
        QSettings settings(QStringLiteral("ScopeOne"), QStringLiteral("ScopeOne"));
        if (m_core->processingModules().isEmpty())
        {
            QMessageBox::information(m_window, tr("Run Pipeline on a Folder"),
                                     tr("The Process panel has no steps. Add the steps to run first."));
            return;
        }
        const QString input = QFileDialog::getExistingDirectory(
            m_window, tr("Folder with the images"), settings.value(QStringLiteral("Batch/input")).toString());
        if (input.isEmpty())
        {
            return;
        }
        const QString output = QFileDialog::getExistingDirectory(
            m_window, tr("Folder for the results"), settings.value(QStringLiteral("Batch/output"), input).toString());
        if (output.isEmpty())
        {
            return;
        }
        settings.setValue(QStringLiteral("Batch/input"), input);
        settings.setValue(QStringLiteral("Batch/output"), output);
        QList<ParameterField> fields{ParameterField::check(tr("Include subfolders"), false)};
        if (!askParameters(m_window, tr("Run Pipeline on a Folder"), fields))
        {
            return;
        }
        QStringList files;
        QDirIterator iterator(input, {QStringLiteral("*.tif"), QStringLiteral("*.tiff"), QStringLiteral("*.png"),
                                      QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")},
                              QDir::Files, fields.first().checked ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (iterator.hasNext())
        {
            files.append(iterator.next());
        }
        files.sort();
        if (files.isEmpty())
        {
            QMessageBox::information(m_window, tr("Run Pipeline on a Folder"), tr("There are no image files in that folder."));
            return;
        }

        QProgressDialog progress(tr("Processing images..."), tr("Cancel"), 0, static_cast<int>(files.size()), m_window);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        QStringList failed;
        int done = 0;
        for (const QString& path : files)
        {
            progress.setValue(done);
            progress.setLabelText(tr("%1 (%2 of %3)").arg(QFileInfo(path).fileName()).arg(done + 1).arg(files.size()));
            QCoreApplication::processEvents();
            if (progress.wasCanceled())
            {
                break;
            }
            ++done;
            QString error;
            const QList<ImageFrame> frames = scopeone::core::ScopeOneCore::readImageFrames(path, &error);
            // A new pipeline for each file, so that steps that remember frames start afresh
            const auto pipeline = m_core->createProcessingPipeline();
            QList<ImageFrame> results;
            for (const ImageFrame& frame : frames)
            {
                const scopeone::core::ProcessingResult result = pipeline->process(frame, 16);
                if (result.hasImage())
                {
                    results.append(result.frame);
                }
            }
            const QString relative = QDir(input).relativeFilePath(path);
            QString target = QDir(output).filePath(QFileInfo(relative).path() == QLatin1String(".")
                                                       ? QFileInfo(relative).completeBaseName()
                                                       : QFileInfo(relative).path() + QLatin1Char('/') + QFileInfo(relative).completeBaseName());
            target += QStringLiteral("_processed.tif");
            QDir().mkpath(QFileInfo(target).path());
            if (frames.isEmpty() || results.isEmpty()
                || !scopeone::core::ScopeOneCore::writeImageFrames(results, target, &error))
            {
                failed.append(QFileInfo(path).fileName());
            }
        }
        progress.setValue(static_cast<int>(files.size()));
        QString summary = tr("Processed %1 of %2 images into %3.").arg(done - failed.size()).arg(files.size()).arg(QDir::toNativeSeparators(output));
        if (!failed.isEmpty())
        {
            summary += tr("\n\nThese could not be processed:\n%1").arg(failed.join(QLatin1Char('\n')));
        }
        QMessageBox::information(m_window, tr("Run Pipeline on a Folder"), summary);
    }

    void ImageAnalysis::buildAnalyzeMenu(QMenu* menu)
    {
        QAction* measureAction = menu->addAction(tr("&Measure"));
        measureAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
        connect(measureAction, &QAction::triggered, this, [this]() { measure(); });
        connect(menu->addAction(tr("&Results")), &QAction::triggered, this, [this]()
        {
            showPanel(results(), tr("Results"));
        });
        connect(menu->addAction(tr("&Set Measurements...")), &QAction::triggered, this, [this]()
        {
            results()->chooseMeasurements();
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
            showPanel(m_roiManager, tr("ROI Manager"));
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
                tr("Copy of %1").arg(source.title), scope == Scope::All ? &source.shape : nullptr);
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
        const QRect area = core::imageops::selectionBounds(shape);
        apply(tr("Crop"), [area](const ImageFrame& frame) { return core::imageops::crop(frame, area); });
    }

    void ImageAnalysis::scaleImage()
    {
        QList<ParameterField> fields{ParameterField::real(tr("X scale"), 0.5, 0.01, 20.0, 3),
                                     ParameterField::real(tr("Y scale"), 0.5, 0.01, 20.0, 3),
                                     ParameterField::check(tr("Interpolate"), true)};
        if (askWithPreview(tr("Scale"), fields, [](const ImageFrame& frame, const QList<ParameterField>& v)
                           { return core::imageops::scaled(frame, v.at(0).value, v.at(1).value, v.at(2).checked); }))
        {
            const double x = fields.at(0).value;
            const double y = fields.at(1).value;
            const bool smooth = fields.at(2).checked;
            apply(tr("Scale"), [x, y, smooth](const ImageFrame& frame) { return core::imageops::scaled(frame, x, y, smooth); });
        }
    }

    void ImageAnalysis::rotateImage()
    {
        QList<ParameterField> fields{ParameterField::real(tr("Angle (degrees)"), 15.0, -360.0, 360.0, 1),
                                     ParameterField::check(tr("Interpolate"), true),
                                     ParameterField::check(tr("Enlarge image to fit result"), false)};
        if (askWithPreview(tr("Rotate"), fields, [](const ImageFrame& frame, const QList<ParameterField>& v)
                           { return core::imageops::rotated(frame, v.at(0).value, v.at(1).checked, v.at(2).checked); }))
        {
            const double angle = fields.at(0).value;
            const bool smooth = fields.at(1).checked;
            const bool grow = fields.at(2).checked;
            apply(tr("Rotate"), [angle, smooth, grow](const ImageFrame& frame)
            {
                return core::imageops::rotated(frame, angle, smooth, grow);
            });
        }
    }

    // Makes a binary image from the pixels that are above a limit found by one of the methods
    void ImageAnalysis::thresholdImage()
    {
        runModule(tr("Threshold"), QStringLiteral("threshold"), {},
                  {QStringLiteral("method"), QStringLiteral("select"), QStringLiteral("low"), QStringLiteral("high")});
    }

    // ---- Stacks

    void ImageAnalysis::zProject()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        QList<QChar> letters;
        const QStringList names = axisChoices(source, letters);
        // Z when the image has it, as in Fiji
        const int zIndex = static_cast<int>(letters.indexOf(QLatin1Char('Z')));
        int largest = 1;
        for (const QChar letter : letters)
        {
            largest = std::max(largest, source.shape.axisSize(letter));
        }
        QList<ParameterField> fields{
            ParameterField::options(tr("Project along"), names, std::max(0, zIndex)),
            ParameterField::integer(tr("Start"), 1, 1, largest),
            ParameterField::integer(tr("Stop"), largest, 1, largest),
            ParameterField::options(tr("Projection"),
                                    {tr("Maximum Intensity"), tr("Average Intensity"), tr("Sum Slices"),
                                     tr("Minimum Intensity"), tr("Standard Deviation"), tr("Median")})};
        if (!askParameters(m_window, tr("Z Project"), fields))
        {
            return;
        }
        static const core::imageops::Projection methods[] = {
            core::imageops::Projection::Max, core::imageops::Projection::Average, core::imageops::Projection::Sum,
            core::imageops::Projection::Min, core::imageops::Projection::StdDev, core::imageops::Projection::Median};
        const core::imageops::Projection method = methods[fields.at(3).choice];
        const QChar axis = letters.at(fields.at(0).choice);
        const int size = source.shape.axisSize(axis);
        const int start = std::clamp(static_cast<int>(fields.at(1).value) - 1, 0, size - 1);
        const int stop = std::clamp(static_cast<int>(fields.at(2).value) - 1, start, size - 1);

        // Every position on the other axes gets its own projection, so a hyperstack stays one
        scopeone::core::StackShape result = source.shape;
        (axis == QLatin1Char('C') ? result.channels : (axis == QLatin1Char('Z') ? result.slices : result.frames)) = 1;
        const int outputCount = result.channels * result.slices * result.frames;
        QList<ImageFrame> projections;
        for (int i = 0; i < outputCount; ++i)
        {
            int c, z, t;
            stack::coordinates(result, i, c, z, t);
            QList<ImageFrame> line;
            for (int step = start; step <= stop; ++step)
            {
                stack::coordinate(c, z, t, axis) = step;
                line.append(source.frames.at(stack::planeIndex(source.shape, c, z, t)));
            }
            projections.append(core::imageops::zProject(line, 0, static_cast<int>(line.size()) - 1, method));
        }
        present(projections, tr("Projection of %1").arg(source.title), &result);
    }

    void ImageAnalysis::makeSubstack()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        QList<QChar> letters;
        const QStringList names = axisChoices(source, letters);
        QList<ParameterField> fields;
        for (int i = 0; i < letters.size(); ++i)
        {
            const int size = source.shape.axisSize(letters.at(i));
            const QString name = names.at(i);
            fields.append(ParameterField::integer(tr("%1: first").arg(name), 1, 1, size));
            fields.append(ParameterField::integer(tr("%1: last").arg(name), size, 1, size));
            fields.append(ParameterField::integer(tr("%1: step").arg(name), 1, 1, std::max(1, size)));
        }
        if (!askParameters(m_window, tr("Substack"), fields))
        {
            return;
        }
        // The positions that are kept on each axis
        scopeone::core::StackShape result = source.shape;
        QList<int> kept[3];
        for (int i = 0; i < letters.size(); ++i)
        {
            const QChar axis = letters.at(i);
            const int size = source.shape.axisSize(axis);
            const int first = std::clamp(static_cast<int>(fields.at(3 * i).value) - 1, 0, size - 1);
            const int last = std::clamp(static_cast<int>(fields.at(3 * i + 1).value) - 1, first, size - 1);
            const int step = std::max(1, static_cast<int>(fields.at(3 * i + 2).value));
            QList<int>& positions = kept[axis == QLatin1Char('C') ? 0 : (axis == QLatin1Char('Z') ? 1 : 2)];
            for (int position = first; position <= last; position += step)
            {
                positions.append(position);
            }
            (axis == QLatin1Char('C') ? result.channels : (axis == QLatin1Char('Z') ? result.slices : result.frames)) =
                static_cast<int>(positions.size());
        }
        const auto positionsOf = [&kept, &source](QChar axis, int step)
        {
            const QList<int>& positions = kept[axis == QLatin1Char('C') ? 0 : (axis == QLatin1Char('Z') ? 1 : 2)];
            return positions.isEmpty() ? step : positions.at(step);
        };
        QList<ImageFrame> frames;
        const int outputCount = result.channels * result.slices * result.frames;
        for (int i = 0; i < outputCount; ++i)
        {
            int c, z, t;
            stack::coordinates(result, i, c, z, t);
            frames.append(source.frames.at(stack::planeIndex(source.shape, positionsOf(QLatin1Char('C'), c),
                                                             positionsOf(QLatin1Char('Z'), z), positionsOf(QLatin1Char('T'), t))));
        }
        present(frames, tr("Substack of %1").arg(source.title), &result);
    }

    void ImageAnalysis::makeMontage()
    {
        Source source;
        if (!activeSource(source))
        {
            return;
        }
        QList<QChar> letters;
        const QStringList names = axisChoices(source, letters);
        const int zIndex = static_cast<int>(letters.indexOf(QLatin1Char('Z')));
        const int count = static_cast<int>(source.frames.size());
        QList<ParameterField> fields{
            ParameterField::options(tr("Tile along"), names, std::max(0, zIndex)),
            ParameterField::integer(tr("Columns"), static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count)))), 1, std::max(1, count)),
            ParameterField::real(tr("Scale"), 0.5, 0.05, 4.0, 2),
            ParameterField::integer(tr("Border width"), 2, 0, 100)};
        if (askParameters(m_window, tr("Montage"), fields))
        {
            const QList<ImageFrame> frames = framesAlong(source, letters.at(fields.at(0).choice));
            present({core::imageops::montage(frames, static_cast<int>(fields.at(1).value), fields.at(2).value,
                                       static_cast<int>(fields.at(3).value))},
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
        QList<QChar> letters;
        const QStringList names = axisChoices(source, letters);
        int axisIndex = std::max(0, static_cast<int>(letters.indexOf(QLatin1Char('Z'))));
        if (letters.size() > 1)
        {
            QList<ParameterField> fields{ParameterField::options(tr("Plot along"), names, axisIndex)};
            if (!askParameters(m_window, tr("Plot Z-axis Profile"), fields))
            {
                return;
            }
            axisIndex = fields.first().choice;
        }
        ImageSceneModel::Markup shape;
        const bool hasShape = selectedShape(source, shape);
        const QVector<double> values = core::imageops::zProfile(framesAlong(source, letters.at(axisIndex)),
                                                          hasShape ? &shape : nullptr, core::imageops::ProfileStat::Mean);
        if (!m_profile)
        {
            m_profile = new ProfilePlotWindow(m_window);
        }
        const QString axisTitle = source.known ? names.at(axisIndex) : tr("Slice");
        m_profile->setProfile(tr("Profile of %1 along %2").arg(source.title, axisTitle), axisTitle, tr("Mean"), values);
        showPanel(m_profile, tr("Profile"));
    }

    void ImageAnalysis::showOrthogonalViews()
    {
        Source source;
        if (!activeSource(source) || source.frames.size() < 2)
        {
            return;
        }
        QList<QChar> letters;
        const QStringList names = axisChoices(source, letters);
        int axisIndex = std::max(0, static_cast<int>(letters.indexOf(QLatin1Char('Z'))));
        if (letters.size() > 1)
        {
            QList<ParameterField> fields{ParameterField::options(tr("Views along"), names, axisIndex)};
            if (!askParameters(m_window, tr("Orthogonal Views"), fields))
            {
                return;
            }
            axisIndex = fields.first().choice;
        }
        int position = 0;
        const QList<ImageFrame> frames = framesAlong(source, letters.at(axisIndex), &position);
        OrthogonalViewsDialog dialog(frames, position, m_window);
        dialog.exec();
    }

    // ---- Process menu

    void ImageAnalysis::subtractBackground()
    {
        QList<ParameterField> fields{ParameterField::real(tr("Rolling ball radius (pixels)"), 50.0, 1.0, 500.0, 1),
                                     ParameterField::check(tr("Light background"), false)};
        if (askWithPreview(tr("Subtract Background"), fields, [](const ImageFrame& frame, const QList<ParameterField>& v)
                           { return core::imageops::subtractBackground(frame, v.at(0).value, v.at(1).checked); }))
        {
            const double radius = fields.at(0).value;
            const bool light = fields.at(1).checked;
            apply(tr("Subtract Background"), [radius, light](const ImageFrame& frame)
            {
                return core::imageops::subtractBackground(frame, radius, light);
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
        const std::vector<qint64> histogram = core::imageops::histogramOfFrames(
            wholeStack ? source.frames : QList<ImageFrame>{current});

        if (!normalize && !equalize)
        {
            const core::imageops::LevelRange range = core::imageops::saturatedRange(histogram, saturated);
            if (source.scene)
            {
                source.scene->setLayerAutoStretchEnabled(source.layerKey, false);
                source.scene->setLayerDisplayLevels(source.layerKey, range.low, range.high, core::imageops::maxValue(current));
            }
            return;
        }
        const Scope scope = chooseScope(source, tr("Enhance Contrast"));
        if (scope == Scope::Cancel)
        {
            return;
        }
        const core::imageops::LevelRange range = core::imageops::saturatedRange(histogram, saturated);
        const auto operation = [&](const ImageFrame& frame)
        {
            if (equalize)
            {
                return core::imageops::equalized(frame, true, wholeStack ? &histogram : nullptr);
            }
            return core::imageops::stretched(frame, range.low, range.high);
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
        runModule(tr("Enhance Local Contrast"), QStringLiteral("clahe"), {},
                  {QStringLiteral("block_size"), QStringLiteral("max_slope")});
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
            return core::imageops::stretched(frame, low, high);
        });
    }

    void ImageAnalysis::addNoise(bool saltAndPepper)
    {
        QList<ParameterField> fields{
            saltAndPepper ? ParameterField::real(tr("Percent of pixels"), 2.5, 0.0, 100.0, 2)
                          : ParameterField::real(tr("Standard deviation"), 25.0, 0.0, 100000.0, 1)};
        if (askWithPreview(saltAndPepper ? tr("Add Salt and Pepper") : tr("Add Noise"), fields,
                           [saltAndPepper](const ImageFrame& frame, const QList<ParameterField>& v)
                           {
                               return saltAndPepper ? core::imageops::withSaltAndPepper(frame, v.first().value)
                                                    : core::imageops::withGaussianNoise(frame, v.first().value);
                           }))
        {
            const double amount = fields.first().value;
            apply(saltAndPepper ? tr("Salt and Pepper") : tr("Add Noise"), [amount, saltAndPepper](const ImageFrame& frame)
            {
                return saltAndPepper ? core::imageops::withSaltAndPepper(frame, amount) : core::imageops::withGaussianNoise(frame, amount);
            });
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
        if (!core::imageops::isMono(first) || !core::imageops::isMono(second))
        {
            return;
        }
        present({core::imageops::calculate(first, second, static_cast<core::imageops::CalculatorOperation>(fields.at(1).choice))},
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
                               core::imageops::enlargedOutline(source.frames.first().width, source.frames.first().height,
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
            replaceWithPolygon(source, shape, core::imageops::rotatedCorners(shape, fields.first().value));
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
            replaceWithPolygon(source, shape, core::imageops::scaledCorners(shape, fields.at(0).value, fields.at(1).value));
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
        ImageFrame mask = core::imageops::selectionMask(source.frames.first().width, source.frames.first().height, shape);
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
        for (const core::imageops::Particle& particle : core::imageops::findParticles(source.frames.at(source.index)))
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
        const int planes = static_cast<int>(source.frames.size());
        QList<ParameterField> fields{
            ParameterField::real(tr("Pixel size (µm)"), m_workspace->pixelSizeUm(source.layerKey), 0.0, 1.0e6, 4)};
        // How the slices of a stack are arranged, when the file does not say so
        static const QStringList orders{QStringLiteral("TZC"), QStringLiteral("TCZ"), QStringLiteral("ZTC"),
                                        QStringLiteral("ZCT"), QStringLiteral("CTZ"), QStringLiteral("CZT")};
        const bool canArrange = planes > 1 && m_workspace->document().isValid();
        if (canArrange)
        {
            fields.append(ParameterField::integer(tr("Channels"), source.shape.channels, 1, planes));
            fields.append(ParameterField::integer(tr("Slices (Z)"), source.shape.slices, 1, planes));
            fields.append(ParameterField::integer(tr("Frames (T)"), source.shape.frames, 1, planes));
            QStringList names;
            for (const QString& order : orders)
            {
                names << tr("%1 (%2 slowest, %3 fastest)").arg(order, order.first(1), order.last(1));
            }
            fields.append(ParameterField::options(tr("Order"), names, std::max(0, static_cast<int>(orders.indexOf(source.shape.order)))));
        }
        if (!askParameters(m_window, tr("Properties of %1").arg(source.title), fields))
        {
            return;
        }
        if (canArrange)
        {
            const int channels = static_cast<int>(fields.at(1).value);
            const int slices = static_cast<int>(fields.at(2).value);
            const int frames = static_cast<int>(fields.at(3).value);
            if (channels * slices * frames != planes)
            {
                QMessageBox::warning(m_window, tr("Properties"),
                                     tr("Channels times slices times frames must be %1, the number of images.").arg(planes));
            }
            else if (const auto session = m_workspace->documentSession())
            {
                session->setStackShape(channels, slices, frames, orders.at(fields.at(4).choice));
                m_workspace->refreshStackShape(source.layerKey);
            }
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
                const int value = core::imageops::pixelAt(image, x, y);
                binary = value == 0 || value == core::imageops::maxValue(image) || value == 255;
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
            const int limit = core::imageops::autoThreshold(image, static_cast<core::imageops::ThresholdMethod>(fields.at(4).choice));
            mask = fields.at(5).checked ? core::imageops::thresholded(image, 0, limit)
                                        : core::imageops::thresholded(image, limit + 1, core::imageops::maxValue(image));
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
        if (display)
        {
            results();
        }

        QList<core::imageops::Particle> kept;
        QSignalBlocker blocker(addToManager ? source.scene : nullptr);
        for (const core::imageops::Particle& particle : core::imageops::findParticles(mask))
        {
            if (excludeEdges && particle.touchesEdge)
            {
                continue;
            }
            const core::imageops::Measurement result = core::imageops::measureParticle(image, particle, scale, includeHoles);
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
                m_results->addMeasurement(label, result, source.frames.size() > 1 ? source.index + 1 : 0, unitName,
                                          source.hyper ? stack::positionText(source.shape, source.index) : QString());
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
            ImageFrame shown = core::imageops::particleImage(kept, image.width, image.height, show == 2);
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
        results();
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
                                          core::imageops::measure(source.frames.at(index), shape, scale),
                                          source.frames.size() > 1 ? index + 1 : 0,
                                          unit,
                                          source.hyper ? stack::positionText(source.shape, index) : QString());
            }
        }
    }
}
