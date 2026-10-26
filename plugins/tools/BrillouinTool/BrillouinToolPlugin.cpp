#include "scopeone/ToolFrameStream.h"
#include "scopeone/ToolPlugin.h"
#include "scopeone/ScopeOneCore.h"
#include "scopeone/ImageSceneModel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    using scopeone::core::ImageFrame;

    struct BrillouinParameters
    {
        int bandRows{32};
        int binColumns{8};
        double ghzPerPixel{0.5};
        double minGhz{4.5};
        double maxGhz{6.5};
    };

    // Position of a peak in a spectrum, as the centroid of the light around the strongest sample
    double peakPosition(const std::vector<float>& spectrum, int first, int last, float background)
    {
        int best = first;
        for (int row = first; row < last; ++row)
        {
            if (spectrum[row] > spectrum[best])
            {
                best = row;
            }
        }
        double weight = 0.0;
        double moment = 0.0;
        for (int row = (std::max)(first, best - 3); row <= (std::min)(last - 1, best + 3); ++row)
        {
            const double value = (std::max)(0.0f, spectrum[row] - background);
            weight += value;
            moment += value * row;
        }
        return weight > 0.0 ? moment / weight : best;
    }

    // The image of the spectrometer holds one line of the sample after another, each in a band of rows with the
    // frequency across the band. The distance of the Stokes and the anti-Stokes peak of a position is twice its
    // Brillouin shift, which is what the map shows, in the range given and as 16 bit.
    ImageFrame brillouinMap(const ImageFrame& input, const BrillouinParameters& params)
    {
        if (!input.isValid() || params.bandRows < 8 || input.height < params.bandRows)
        {
            return {};
        }
        const int depth = input.isMono16() ? CV_16UC1 : CV_8UC1;
        const cv::Mat source(input.height, input.width, depth,
                             const_cast<char*>(input.bytes.constData()), static_cast<size_t>(input.stride));
        cv::Mat image;
        source.convertTo(image, CV_32F);

        const int lines = input.height / params.bandRows;
        const int columns = input.width / (std::max)(1, params.binColumns);
        const int half = params.bandRows / 2;
        const double span = params.maxGhz - params.minGhz;
        cv::Mat map(lines, columns, CV_16UC1);
        std::vector<float> spectrum(static_cast<size_t>(params.bandRows));
        for (int line = 0; line < lines; ++line)
        {
            for (int column = 0; column < columns; ++column)
            {
                const cv::Rect cell(column * params.binColumns, line * params.bandRows,
                                    params.binColumns, params.bandRows);
                cv::Mat profile;
                cv::reduce(image(cell), profile, 1, cv::REDUCE_AVG, CV_32F);
                for (int row = 0; row < params.bandRows; ++row)
                {
                    spectrum[static_cast<size_t>(row)] = profile.at<float>(row, 0);
                }
                const float background = *std::min_element(spectrum.begin(), spectrum.end());
                const double stokes = peakPosition(spectrum, 0, half, background);
                const double antiStokes = peakPosition(spectrum, half, params.bandRows, background);
                const double ghz = 0.5 * (antiStokes - stokes) * params.ghzPerPixel;
                const double level = span > 0.0 ? (ghz - params.minGhz) / span : 0.0;
                map.at<ushort>(line, column) = static_cast<ushort>(std::clamp(level, 0.0, 1.0) * 65535.0 + 0.5);
            }
        }

        ImageFrame frame;
        frame.cameraId = input.cameraId;
        frame.width = columns;
        frame.height = lines;
        frame.stride = columns * 2;
        frame.bitsPerSample = 16;
        frame.pixelFormat = scopeone::core::ImagePixelFormat::Mono16;
        frame.frameIndex = input.frameIndex;
        frame.timestampNs = input.timestampNs;
        frame.sourceRoiWidth = columns;
        frame.sourceRoiHeight = lines;
        frame.bytes = QByteArray(reinterpret_cast<const char*>(map.data), static_cast<qsizetype>(map.total() * 2));
        return frame;
    }

    class BrillouinTool final : public QWidget
    {
    public:
        BrillouinTool(scopeone::ui::ScopeOneToolContext& context, QWidget* parent)
            : QWidget(parent)
              , m_context(context)
        {
            setWindowFlag(Qt::Window, true);
            setWindowTitle(QStringLiteral("Brillouin Map"));
            resize(1000, 640);

            auto* layout = new QVBoxLayout(this);
            auto* previews = new QHBoxLayout();
            m_inputPreview = createPreview(QStringLiteral("Spectra"));
            m_resultPreview = createPreview(QStringLiteral("Brillouin shift"));
            previews->addWidget(m_inputPreview, 2);
            previews->addWidget(m_resultPreview, 2);
            layout->addLayout(previews, 1);

            auto* form = new QFormLayout();
            m_layerComboBox = new QComboBox(this);
            m_bandRows = createSpinBox(8, 256, m_parameters.bandRows);
            m_binColumns = createSpinBox(1, 64, m_parameters.binColumns);
            m_ghzPerPixel = createDoubleSpinBox(0.001, 10.0, m_parameters.ghzPerPixel, 3);
            m_minGhz = createDoubleSpinBox(0.0, 50.0, m_parameters.minGhz, 2);
            m_maxGhz = createDoubleSpinBox(0.0, 50.0, m_parameters.maxGhz, 2);
            form->addRow(QStringLiteral("Input"), m_layerComboBox);
            form->addRow(QStringLiteral("Rows per line"), m_bandRows);
            form->addRow(QStringLiteral("Columns per point"), m_binColumns);
            form->addRow(QStringLiteral("GHz per pixel"), m_ghzPerPixel);
            form->addRow(QStringLiteral("Map from (GHz)"), m_minGhz);
            form->addRow(QStringLiteral("Map to (GHz)"), m_maxGhz);
            layout->addLayout(form);

            m_liveCheckBox = new QCheckBox(QStringLiteral("Live"), this);
            m_liveCheckBox->setChecked(true);
            m_status = new QLabel(QStringLiteral("Ready"), this);
            auto* bottom = new QHBoxLayout();
            bottom->addWidget(m_liveCheckBox);
            bottom->addStretch();
            bottom->addWidget(m_status);
            layout->addLayout(bottom);

            m_stream = new scopeone::ui::ScopeOneToolFrameStream(m_context.core(), this);

            refreshLayerComboBox();
            connect(m_context.core().imageSceneModel(),
                    &scopeone::core::ImageSceneModel::layersChanged,
                    this,
                    [this]()
                    {
                        const QString previous = m_layerComboBox->currentData().toString();
                        refreshLayerComboBox();
                        if (previous != m_layerComboBox->currentData().toString())
                        {
                            selectSource();
                        }
                    });
            connect(m_layerComboBox, &QComboBox::currentIndexChanged, this, [this]() { selectSource(); });
            for (QSpinBox* box : {m_bandRows, m_binColumns})
            {
                connect(box, &QSpinBox::valueChanged, this, [this]() { rerun(); });
            }
            for (QDoubleSpinBox* box : {m_ghzPerPixel, m_minGhz, m_maxGhz})
            {
                connect(box, &QDoubleSpinBox::valueChanged, this, [this]() { rerun(); });
            }
            connect(m_liveCheckBox, &QCheckBox::toggled, this,
                    [this](bool enabled)
                    {
                        m_stream->setEnabled(enabled);
                        m_status->setText(enabled ? QStringLiteral("Live") : QStringLiteral("Paused"));
                        if (enabled)
                        {
                            rerun();
                        }
                    });
            connect(m_stream, &scopeone::ui::ScopeOneToolFrameStream::frameReady, this,
                    [this](const ImageFrame& frame)
                    {
                        if (m_liveCheckBox->isChecked())
                        {
                            process(frame);
                        }
                    });
            selectSource();
        }

    private:
        static QSpinBox* createSpinBox(int minimum, int maximum, int value)
        {
            auto* box = new QSpinBox();
            box->setRange(minimum, maximum);
            box->setValue(value);
            return box;
        }

        static QDoubleSpinBox* createDoubleSpinBox(double minimum, double maximum, double value, int decimals)
        {
            auto* box = new QDoubleSpinBox();
            box->setRange(minimum, maximum);
            box->setDecimals(decimals);
            box->setValue(value);
            return box;
        }

        static QLabel* createPreview(const QString& title)
        {
            auto* preview = new QLabel(title);
            preview->setAlignment(Qt::AlignCenter);
            preview->setMinimumSize(240, 200);
            preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            preview->setFrameShape(QFrame::Box);
            return preview;
        }

        static void showPreview(QLabel* preview, const ImageFrame& frame)
        {
            const QImage image(reinterpret_cast<const uchar*>(frame.bytes.constData()),
                               frame.width,
                               frame.height,
                               frame.stride,
                               frame.isMono16() ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8);
            preview->setPixmap(QPixmap::fromImage(image).scaled(
                preview->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
        }

        BrillouinParameters parameters() const
        {
            BrillouinParameters params;
            params.bandRows = m_bandRows->value();
            params.binColumns = m_binColumns->value();
            params.ghzPerPixel = m_ghzPerPixel->value();
            params.minGhz = m_minGhz->value();
            params.maxGhz = m_maxGhz->value();
            return params;
        }

        void refreshLayerComboBox()
        {
            const QString current = m_layerComboBox->currentData().toString();
            const QString active = m_context.currentLayerKey();
            const QSignalBlocker blocker(m_layerComboBox);
            m_layerComboBox->clear();
            for (const QString& layerKey : m_context.core().imageSceneModel()->layerIds())
            {
                QString name = layerKey;
                scopeone::core::DocumentLayer layer;
                if (m_context.core().imageSceneModel()->findLayer(layerKey, layer) && !layer.name.isEmpty())
                {
                    name = layer.name;
                }
                m_layerComboBox->addItem(name, layerKey);
            }
            int index = m_layerComboBox->findData(current);
            if (index < 0 && !active.isEmpty())
            {
                index = m_layerComboBox->findData(active);
            }
            if (index >= 0)
            {
                m_layerComboBox->setCurrentIndex(index);
            }
        }

        QString layerKey() const
        {
            const QString key = m_layerComboBox->currentData().toString();
            return key.isEmpty() ? m_context.currentLayerKey() : key;
        }

        void selectSource()
        {
            m_sourceId = scopeone::core::ScopeOneCore::sourceIdFromLayerKey(layerKey());
            m_stream->setSourceId(m_sourceId);
            rerun();
        }

        void rerun()
        {
            const ImageFrame frame = m_context.core().graphFrame(layerKey());
            if (frame.isValid())
            {
                process(frame);
            }
            else
            {
                m_status->setText(QStringLiteral("No spectra layer available"));
            }
        }

        void process(const ImageFrame& input)
        {
            showPreview(m_inputPreview, input);
            const ImageFrame map = brillouinMap(input, parameters());
            if (!map.isValid())
            {
                m_status->setText(QStringLiteral("The image holds no line of spectra"));
                return;
            }
            showPreview(m_resultPreview, map);
            const ImageFrame stored = m_context.core().publishToolStreamFrame(
                map.cameraId, map, QStringLiteral("Brillouin Shift"));
            if (!stored.isValid())
            {
                m_status->setText(QStringLiteral("Failed to publish the map"));
                return;
            }
            const QString outputLayer = scopeone::core::ScopeOneCore::toolLayerKey(stored.cameraId);
            if (m_shownSourceId != m_sourceId || m_shownOutputLayerKey != outputLayer)
            {
                QStringList layers;
                const QString rawLayer = scopeone::core::ScopeOneCore::rawLayerKey(m_sourceId);
                if (m_context.core().graphFrame(rawLayer).isValid())
                {
                    layers.append(rawLayer);
                }
                layers.append(outputLayer);
                m_context.showLayers(layers, layers.size() > 1);
                m_shownSourceId = m_sourceId;
                m_shownOutputLayerKey = outputLayer;
            }
            m_status->setText(m_liveCheckBox->isChecked() ? QStringLiteral("Live") : QStringLiteral("Map ready"));
        }

        scopeone::ui::ScopeOneToolContext& m_context;
        BrillouinParameters m_parameters;
        QLabel* m_inputPreview{nullptr};
        QLabel* m_resultPreview{nullptr};
        QComboBox* m_layerComboBox{nullptr};
        QSpinBox* m_bandRows{nullptr};
        QSpinBox* m_binColumns{nullptr};
        QDoubleSpinBox* m_ghzPerPixel{nullptr};
        QDoubleSpinBox* m_minGhz{nullptr};
        QDoubleSpinBox* m_maxGhz{nullptr};
        QCheckBox* m_liveCheckBox{nullptr};
        QLabel* m_status{nullptr};
        scopeone::ui::ScopeOneToolFrameStream* m_stream{nullptr};
        QString m_sourceId;
        QString m_shownSourceId;
        QString m_shownOutputLayerKey;
    };

    class BrillouinToolPlugin final : public QObject,
                                      public scopeone::ui::ScopeOneToolPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneToolPlugin_iid FILE "plugin.json")
        Q_INTERFACES(scopeone::ui::ScopeOneToolPlugin)

    public:
        QList<scopeone::ui::ToolDescriptor> tools() const override
        {
            return {{QStringLiteral("scopeone.brillouin_map"),
                     QStringLiteral("Brillouin Map"),
                     QStringLiteral("Reconstruction"),
                     scopeone::ui::ToolWindowMode::ModelessSingleton,
                     false}};
        }

        QWidget* createTool(const QString& toolId,
                            scopeone::ui::ScopeOneToolContext& context,
                            QWidget* parent) override
        {
            return toolId == QStringLiteral("scopeone.brillouin_map")
                       ? new BrillouinTool(context, parent)
                       : nullptr;
        }
    };
}

#include "BrillouinToolPlugin.moc"
