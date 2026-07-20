#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QElapsedTimer>
#include <QMutex>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <QVector>
#include <vector>
#include "scopeone/ImageSceneModel.h"
#include "scopeone/ImageFrame.h"

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPainter;
class QPointF;
class QWheelEvent;

namespace scopeone::ui
{
    using ImageSceneModel = scopeone::core::ImageSceneModel;

    class PreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions
    {
        Q_OBJECT

    public:
        enum class LayerLayoutMode { SideBySide, Overlay };

        struct PreviewInteractionTarget
        {
            QString layerKey;
            QString sourceId;
            QPoint imagePos;
            QRect itemArea;
            QRect displayRect;
            bool processed{false};
        };

        PreviewWidget(ImageSceneModel* sceneModel, QWidget* parent);
        ~PreviewWidget() override;

        void setGraphProcessedFrame(const scopeone::core::ImageFrame& frame);
        void setGraphRawFrame(const scopeone::core::ImageFrame& frame);
        void trackProcessedFrameRate(const scopeone::core::ImageFrame& frame);
        void trackRawFrameRate(const scopeone::core::ImageFrame& frame);
        void resetLiveFrameRates();
        void setLayerLayoutMode(LayerLayoutMode mode);
        LayerLayoutMode layerLayoutMode() const;
        void setAvailableCameraIds(const QStringList& cameraIds);
        QString setGraphStaticLayerFrame(const QString& layerId,
                                         const scopeone::core::ImageFrame& frame);
        bool removeStaticLayer(const QString& layerKey);
        void clearStaticLayers();
        QStringList availableCameraIds() const;
        QStringList availableLayerKeys() const;
        QStringList visibleLayerKeys() const;
        QStringList supportedLayerColormaps() const;
        QStringList supportedLayerBlendingModes() const;
        QString layerName(const QString& layerKey) const;
        QString layerInfoText(const QString& layerKey) const;
        QString layerInfoSummaryText() const;
        void clearSourceFrames(const QString& sourceId);
        void clearProcessedFrames();
        void setZoomPercent(int percent);
        int zoomPercent() const;
        void setFitToWindow(bool enabled);
        bool isFitToWindow() const;
        void startROIDrawing(const QString& cameraId);
        void startCrossSectionDrawingForLayer(const QString& layerKey);
        void clearCrossSection();

        bool interactionTargetAt(const QPoint& widgetPos,
                                 PreviewInteractionTarget& outTarget,
                                 const QString& sourceId = QString(),
                                 bool rawOnly = false) const;
signals:
        void availableCameraIdsChanged(const QStringList& cameraIds);
        void availableLayerKeysChanged(const QStringList& layerKeys);
        void visibleLayerKeysChanged(const QStringList& layerKeys);
        void layerLayoutModeChanged(LayerLayoutMode mode);
        void layerInfoTextChanged(const QString& text);
        void zoomLevelChanged(int zoomPercent);
        void fitToWindowChanged(bool enabled);
        void mousePositionChanged(const QPoint& widgetPos);
        void roiDrawn(const QString& cameraId,
                      int x,
                      int y,
                      int width,
                      int height,
                      int sourceRoiX,
                      int sourceRoiY);

    protected:
        void initializeGL() override;
        void resizeGL(int, int) override;
        void paintGL() override;
        void mousePressEvent(QMouseEvent* event) override;
        void mouseMoveEvent(QMouseEvent* event) override;
        void mouseReleaseEvent(QMouseEvent* event) override;
        void leaveEvent(QEvent* event) override;
        void wheelEvent(QWheelEvent* event) override;
        void keyPressEvent(QKeyEvent* event) override;

    private:
        struct FpsState
        {
            QElapsedTimer intervalTimer;
            quint64 framesSinceUpdate{0};
        };

        enum class Colormap { Gray = 0, Green, Magenta, Cyan, Red, Blue, Yellow, Fire };
        enum class Blending { Translucent = 0, Additive, Minimum, Opaque, Multiplicative };
        enum class FrameRole { Raw, Processed };
        enum class MarkupEditMode
        {
            None,
            Move,
            LineStart,
            LineEnd,
            RectTopLeft,
            RectTopRight,
            RectBottomLeft,
            RectBottomRight,
        };

        struct LayerDisplaySettings
        {
            bool visible{false};
            int opacityPercent{100};
            double gamma{1.0};
            Colormap colormap{Colormap::Gray};
            Blending blending{Blending::Translucent};
            int levelMin{0};
            int levelMax{255};
            int levelDomainMax{255};
        };

        struct FrameSourceState
        {
            scopeone::core::ImageFrame processedFrame;
            scopeone::core::ImageFrame rawFrame;
            int offsetX{0};
            int offsetY{0};
            bool flipX{false};
            bool flipY{false};
            int zoomPercent{100};
        };

        struct FrameSourceRenderInfo
        {
            QString sourceId;
            const FrameSourceState* frameState{nullptr};
            bool hasProcessedFrame{false};
            bool hasRawFrame{false};
        };

        struct LayerRenderItem
        {
            const FrameSourceRenderInfo* info{nullptr};
            bool processed{false};
            QString layerKey;
        };

        struct RenderItem
        {
            const FrameSourceRenderInfo* info{nullptr};
            bool processed{false};
            QString layerKey;
            QRect area;
            LayerDisplaySettings display;
            bool firstVisibleInArea{false};
        };

    private:
        QStringList m_availableCameraIds;
        QSet<QString> m_staticSourceIds;
        LayerLayoutMode m_layerLayoutMode{LayerLayoutMode::SideBySide};
        QMap<QString, double> m_layerFps;
        QString m_layerInfoText{QStringLiteral("No image loaded")};
        QMap<QString, FpsState> m_fpsStates;
        QTimer m_fpsUpdateTimer;
        ImageSceneModel* m_sceneModel{nullptr};

        mutable QMutex m_mutex;
        QMap<QString, FrameSourceState> m_frameSources;
        int m_zoomPercent{100};
        bool m_fitToWindow{true};
        QPoint m_viewOffset;
        QString m_placeholderText{QStringLiteral("No image loaded")};

        bool m_glInited{false};
        QOpenGLVertexArrayObject m_vao;
        GLuint m_vbo{0};
        QOpenGLShaderProgram m_prog;
        GLint m_uTex{-1}, m_uMinNorm{-1}, m_uMaxNorm{-1}, m_uTexNormScale{-1}, m_uAlpha{-1};
        GLint m_uGamma{-1}, m_uColormap{-1};
        GLint m_uUvScale{-1}, m_uUvOffset{-1};

        struct CachedTexture
        {
            GLuint texId{0};
            int width{0};
            int height{0};
            GLenum internalFormat{0};
        };

        QMap<QString, CachedTexture> m_textureCache;


        bool m_roiDrawingMode{false};
        QString m_roiTargetCameraId;
        QString m_roiTargetLayerKey;
        QPoint m_roiStart;
        QPoint m_roiEnd;
        bool m_roiDragging{false};
        bool m_crossSectionDrawingMode{false};
        QString m_crossSectionTargetSourceId;
        QString m_crossSectionTargetLayerKey;
        QPoint m_crossSectionStart;
        QPoint m_crossSectionEnd;
        bool m_crossSectionDragging{false};
        QString m_dragMarkupId;
        ImageSceneModel::Markup m_dragMarkupOriginal;
        QPoint m_dragMarkupStartImagePos;
        MarkupEditMode m_dragMarkupEditMode{MarkupEditMode::None};
        bool m_markupDragging{false};
        void updateImageDisplay();
        void updateLayerFps(const QString& layerKey);
        void updateFrameRates();
        bool storeSourceFrame(const QString& sourceId,
                              FrameRole role,
                              const scopeone::core::ImageFrame& frame,
                              bool* replacedFrame = nullptr);
        void initializeLayerInfo(const QString& layerKey);
        void updateLayerInfoDisplay();
        bool registerAvailableCamera(const QString& cameraId);
        LayerDisplaySettings defaultLayerDisplaySettings(bool processed) const;
        LayerDisplaySettings layerDisplaySettings(const QString& layerKey) const;
        Colormap colormapFromName(const QString& name) const;
        QString colormapName(Colormap colormap) const;
        Blending blendingFromName(const QString& name) const;
        QString blendingName(Blending blending) const;
        void removeStaticLayerData(const QString& sourceId);
        QSet<QString> validLayerKeys() const;
        bool hasRawFrame(const FrameSourceState& frameState) const;
        QMap<QString, FrameSourceState> snapshotFrameSources() const;
        std::vector<FrameSourceRenderInfo> buildFrameSourceRenderInfos(const QMap<QString, FrameSourceState>& frameSources) const;
        void buildRenderSnapshot(QMap<QString, FrameSourceState>& frameSources,
                                 std::vector<FrameSourceRenderInfo>& frameSourceRenderInfos,
                                 std::vector<RenderItem>& renderItems) const;
        bool resolveDisplayGeometry(const FrameSourceState& frameState,
                                    bool processed,
                                    const QRect& area,
                                    QRect& displayRect,
                                    QSize& imageSize) const;
        bool resolveLayerDisplayGeometry(const QString& layerKey,
                                         FrameSourceState& frameState,
                                         bool& processed,
                                         QRect& itemArea,
                                         QRect& displayRect,
                                         QSize& imageSize) const;
        bool resolveInteractionTarget(const QPoint& widgetPos,
                                      PreviewInteractionTarget& outTarget,
                                      const QString& sourceId,
                                      bool rawOnly,
                                      const QString& layerKey) const;
        bool mapWidgetPositionToImage(const FrameSourceState& frameState,
                                      bool processed,
                                      const QRect& area,
                                      const QPoint& widgetPos,
                                      QPoint& imagePos) const;
        bool mapWidgetRectToImage(const FrameSourceState& frameState,
                                  bool processed,
                                  const QRect& area,
                                  const QRect& widgetRect,
                                  QRect& imageRect) const;
        bool mapImagePositionToWidget(const FrameSourceState& frameState,
                                      bool processed,
                                      const QRect& area,
                                      const QPoint& imagePos,
                                      QPoint& widgetPos) const;
        void paintPlaceholder(const QString& text);
        bool drawMarkup(QPainter& painter,
                        const ImageSceneModel::Markup& markup,
                        const RenderItem& item) const;
        void drawMarkups(QPainter& painter, const std::vector<RenderItem>& renderItems) const;
        void drawActiveInteractionMarkup(QPainter& painter, const std::vector<RenderItem>& renderItems) const;
        bool markupAtWidgetPosition(const QPoint& widgetPos,
                                    ImageSceneModel::Markup& outMarkup,
                                    PreviewInteractionTarget& outTarget,
                                    MarkupEditMode& outEditMode) const;
        void clearSelectedMarkups();
        void drawRenderItem(const RenderItem& item);
        void ensureGlPipeline();
        void drawFrameInRect(const QString& textureKey,
                             const scopeone::core::ImageFrame& frame,
                             const QRect& r,
                             bool flipX,
                             bool flipY,
                             const LayerDisplaySettings& display,
                             bool firstVisibleInArea);
        QRect targetRectForImageSize(const QSize& imageSize,
                                     const FrameSourceState& frameState,
                                     const QRect& avail) const;
        void setUvTransform(bool flipX, bool flipY);
        void applyViewportForRect(const QRect& logicalRect);
        std::vector<QRect> computeLayout(int count) const;
        std::vector<RenderItem> buildRenderItems(const std::vector<FrameSourceRenderInfo>& frameSourceRenderInfos) const;

        GLuint getOrCreateTexture(const QString& key, int width, int height, GLenum internalFormat);
        void cleanupTextureCache();
        void cancelROIDrawing();
        void cancelCrossSectionDrawing();
    };
}
