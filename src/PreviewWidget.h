#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QMutex>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVector>
#include <vector>
#include "scopeone/ImageFrame.h"

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPointF;
class QWheelEvent;

namespace scopeone::ui
{
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
            QSize imageSize;
            QRect itemArea;
            QRect displayRect;
            bool processed{false};
        };

        explicit PreviewWidget(QWidget* parent = nullptr);
        ~PreviewWidget() override;

        static QString rawLayerKey(const QString& cameraId);
        static QString processedLayerKey(const QString& cameraId);
        static QString staticLayerKey(const QString& layerId);
        static QString sourceIdFromLayerKey(const QString& layerKey);
        static bool isRawLayerKey(const QString& layerKey);
        static bool isProcessedLayerKey(const QString& layerKey);
        static bool isStaticLayerKey(const QString& layerKey);

        void setProcessedFrame(const scopeone::core::ImageFrame& frame);
        void setRawFrame(const scopeone::core::ImageFrame& frame);
        void setLayerLayoutMode(LayerLayoutMode mode);
        LayerLayoutMode layerLayoutMode() const;
        void setAvailableCameraIds(const QStringList& cameraIds);
        void setSelectedLayerKeys(const QStringList& layerKeys);
        void setLayerVisible(const QString& layerKey, bool visible);
        void setLayerOpacityPercent(const QString& layerKey, int percent);
        void setLayerGamma(const QString& layerKey, double gamma);
        void setLayerColormap(const QString& layerKey, const QString& colormap);
        void setLayerBlending(const QString& layerKey, const QString& blending);
        void setLayerDisplayLevels(const QString& layerKey,
                                   int minLevel,
                                   int maxLevel,
                                   int maxPossible);
        void moveLayer(const QString& layerKey, int offset);
        QString setStaticLayerFrame(const QString& layerId,
                                    const QString& displayName,
                                    const scopeone::core::ImageFrame& frame);
        bool removeStaticLayer(const QString& layerKey);
        void clearStaticLayers();
        QStringList availableCameraIds() const;
        QStringList availableLayerKeys() const;
        QStringList selectedLayerKeys() const;
        int layerOpacityPercent(const QString& layerKey) const;
        double layerGamma(const QString& layerKey) const;
        QString layerColormap(const QString& layerKey) const;
        QStringList supportedLayerColormaps() const;
        QString layerBlending(const QString& layerKey) const;
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
        bool sourceDisplayTransform(const QString& sourceId,
                                    int& offsetX,
                                    int& offsetY,
                                    int& zoomPercent,
                                    bool& flipX,
                                    bool& flipY) const;
        void setSourceOffset(const QString& sourceId, int offsetX, int offsetY);
        void setSourceFlip(const QString& sourceId, bool flipX, bool flipY);
        void setSourceZoomPercent(const QString& sourceId, int percent);

        void startROIDrawing(const QString& cameraId);
        void startLineDrawingForLayer(const QString& layerKey);
        void clearLine();

        bool interactionTargetAt(const QPoint& widgetPos,
                                 PreviewInteractionTarget& outTarget,
                                 const QString& sourceId = QString(),
                                 bool rawOnly = false) const;
        bool getPixelValue(const QString& sourceId,
                           const QPoint& imagePos,
                           bool processed,
                           int& outValue) const;
        bool lineProfile(const QString& sourceId,
                         const QPoint& start,
                         const QPoint& end,
                         bool processed,
                         QVector<int>& outValues) const;

    signals:
        void availableCameraIdsChanged(const QStringList& cameraIds);
        void availableLayerKeysChanged(const QStringList& layerKeys);
        void selectedLayerKeysChanged(const QStringList& layerKeys);
        void layerLayoutModeChanged(LayerLayoutMode mode);
        void layerInfoTextChanged(const QString& text);
        void zoomLevelChanged(int zoomPercent);
        void fitToWindowChanged(bool enabled);
        void staticLayerFrameChanged(const QString& layerKey, const scopeone::core::ImageFrame& frame);
        void mousePositionChanged(const QPoint& widgetPos);
        void roiDrawn(const QString& cameraId,
                      int x,
                      int y,
                      int width,
                      int height,
                      int sourceRoiX,
                      int sourceRoiY);
        void lineDrawn(const QString& layerKey,
                       const QString& sourceId,
                       int startX,
                       int startY,
                       int endX,
                       int endY,
                       bool processed);

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
        struct LayerInfo
        {
            int width{0};
            int height{0};
            double fps{0.0};
        };

        struct FpsState
        {
            quint64 acquisitionStartFrameIndex{0};
            quint64 acquisitionStartTimestampNs{0};
            double lastFps{0.0};
        };

        struct FpsUpdate
        {
            double fps{0.0};
            bool changed{false};
        };

        enum class Colormap { Gray = 0, Green, Magenta, Cyan, Red, Blue, Yellow, Fire };
        enum class Blending { Translucent = 0, Additive, Minimum, Opaque, Multiplicative };

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
        QMap<QString, LayerDisplaySettings> m_layers;
        QStringList m_layerOrder;
        QSet<QString> m_staticSourceIds;
        QMap<QString, QString> m_layerNames;
        LayerLayoutMode m_layerLayoutMode{LayerLayoutMode::SideBySide};
        QMap<QString, LayerInfo> m_layerInfos;
        QString m_layerInfoText{QStringLiteral("No image loaded")};
        QMap<QString, FpsState> m_fpsStates;

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
        bool m_lineDrawingMode{false};
        QString m_lineTargetSourceId;
        QString m_lineTargetLayerKey;
        QPoint m_lineStart;
        QPoint m_lineEnd;
        QPoint m_lineStartImage;
        QPoint m_lineEndImage;
        bool m_lineDragging{false};
        bool m_lineProcessed{false};
        bool m_lineVisible{false};
        void updateImageDisplay();
        FpsUpdate updateFpsOnFrame(const QString& layerKey, const scopeone::core::ImageFrame& frame);
        void updateLayerInfoDisplay();
        bool registerAvailableCamera(const QString& cameraId);
        LayerDisplaySettings defaultLayerDisplaySettings(bool processed) const;
        LayerDisplaySettings layerDisplaySettings(const QString& layerKey) const;
        Colormap colormapFromName(const QString& name) const;
        QString colormapName(Colormap colormap) const;
        Blending blendingFromName(const QString& name) const;
        QString blendingName(Blending blending) const;
        void ensureLayer(const QString& layerKey);
        void ensureLayersForCamera(const QString& cameraId);
        void removeStaticLayerData(const QString& sourceId);
        void removeInvalidLayers(const QSet<QString>& validKeys);
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
        void cancelLineDrawing();
    };
}
