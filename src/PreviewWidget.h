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
#include <QElapsedTimer>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <vector>
#include "scopeone/ImageFrame.h"

class QKeyEvent;
class QMouseEvent;
class QPointF;
class QWheelEvent;

namespace scopeone::ui {

class PreviewWidget : public QOpenGLWidget, protected QOpenGLFunctions
{
    Q_OBJECT
public:
    enum class StreamLayoutMode { SideBySide, Overlay };

    explicit PreviewWidget(QWidget* parent = nullptr);
    ~PreviewWidget() override;

    void setProcessedFrame(const QString& cameraId, const scopeone::core::ImageFrame& frame);
    void setRawFrame(const scopeone::core::ImageFrame& frame);
    void setStreamLayoutMode(StreamLayoutMode mode);
    void setOverlayAlphaPercent(int percent);
    int overlayAlphaPercent() const;
    StreamLayoutMode streamLayoutMode() const;
    void setAvailableCameraIds(const QStringList& cameraIds);
    void setSelectedStreams(const QStringList& streamKeys);
    QStringList availableCameraIds() const;
    QStringList selectedStreams() const;
    QString cameraInfoText() const;
    void clearCameraFrames(const QString& cameraId);
    void setZoomPercent(int percent);
    int zoomPercent() const;
    void setFitToWindow(bool enabled);
    bool isFitToWindow() const;
    void setStreamDisplayLevels(const QString& cameraId,
                                bool processed,
                                int minLevel,
                                int maxLevel,
                                int maxPossible);
    void setCameraOffset(const QString& cameraId, int offsetX, int offsetY);
    void setCameraFlip(const QString& cameraId, bool flipX, bool flipY);
    void setCameraZoomPercent(const QString& cameraId, int percent);

    void startROIDrawing(const QString& cameraId);
    void startLineDrawing(const QString& cameraId = QString());
    void clearLine();

    bool widgetToImageCoords(const QPoint& widgetPos,
                             QString& outCameraId,
                             QPoint& outImagePos,
                             bool& outProcessed) const;
    bool widgetToImageCoordsForCamera(const QString& cameraId,
                                      const QPoint& widgetPos,
                                      QPoint& outImagePos,
                                      bool& outProcessed) const;
    bool getPixelValue(const QString& cameraId,
                       const QPoint& imagePos,
                       bool processed,
                       int& outValue) const;

signals:
    void availableCameraIdsChanged(const QStringList& cameraIds);
    void selectedStreamsChanged(const QStringList& streamKeys);
    void streamLayoutModeChanged(StreamLayoutMode mode);
    void cameraInfoTextChanged(const QString& text);
    void zoomLevelChanged(int zoomPercent);
    void fitToWindowChanged(bool enabled);
    void mousePositionChanged(const QPoint& widgetPos);
    void roiDrawn(const QString& cameraId, int x, int y, int width, int height);
    void lineDrawn(const QString& cameraId,
                   int startX,
                   int startY,
                   int endX,
                   int endY,
                   bool processed);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct CameraInfo {
        QString cameraId;
        int width{0};
        int height{0};
        double fps{0.0};
    };
    struct CameraFrameState {
        scopeone::core::ImageFrame processedFrame;
        scopeone::core::ImageFrame rawFrame;
        int rawLevelMin{0};
        int rawLevelMax{255};
        int rawLevelDomainMax{255};
        int processedLevelMin{0};
        int processedLevelMax{255};
        int processedLevelDomainMax{255};
        int offsetX{0};
        int offsetY{0};
        bool flipX{false};
        bool flipY{false};
        int zoomPercent{100};
    };

    struct CameraRenderInfo {
        QString cameraId;
        const CameraFrameState* frameState{nullptr};
        bool hasProcessedFrame{false};
        bool hasRawFrame{false};
    };

    struct StreamItem {
        const CameraRenderInfo* info{nullptr};
        bool processed{false};
    };

    struct RenderItem {
        const CameraRenderInfo* info{nullptr};
        bool processed{false};
        QRect area;
        float alpha{1.0f};
    };

private:
    QStringList m_availableCameraIds;
    QSet<QString> m_selectedStreams;
    int m_overlayAlphaPercent{50};
    StreamLayoutMode m_streamLayoutMode{StreamLayoutMode::SideBySide};
    QMap<QString, CameraInfo> m_cameraInfos;
    QString m_cameraInfoText{QStringLiteral("No image loaded")};
    QElapsedTimer m_fpsTimer;
    int m_fpsFrameCounter{0};
    double m_lastFps{0.0};

    mutable QMutex m_mutex;
    QMap<QString, CameraFrameState> m_cameraFrames;
    int m_zoomPercent{100};
    bool m_fitToWindow{true};
    QPoint m_viewOffset;
    QString m_placeholderText{QStringLiteral("No image loaded")};

    bool m_glInited{false};
    QOpenGLVertexArrayObject m_vao;
    GLuint m_vbo{0};
    QOpenGLShaderProgram m_prog;
    GLint m_uTex{-1}, m_uMinNorm{-1}, m_uMaxNorm{-1}, m_uTexNormScale{-1}, m_uAlpha{-1};
    GLint m_uUvScale{-1}, m_uUvOffset{-1};

    struct CachedTexture {
        GLuint texId{0};
        int width{0};
        int height{0};
        GLenum internalFormat{0};
    };
    QMap<QString, CachedTexture> m_textureCache;


    bool m_roiDrawingMode{false};
    QString m_roiTargetCameraId;
    QPoint m_roiStart;
    QPoint m_roiEnd;
    bool m_roiDragging{false};
    bool m_lineDrawingMode{false};
    QString m_lineTargetCameraId;
    QPoint m_lineStart;
    QPoint m_lineEnd;
    bool m_lineDragging{false};
    bool m_lineProcessed{false};
    bool m_lineVisible{false};
    void updateImageDisplay();
    void updateFpsOnFrame();
    void updateCameraInfoDisplay();
    bool registerAvailableCamera(const QString& cameraId);
    bool hasRawFrame(const CameraFrameState& frameState) const;
    QSize rawFrameSize(const CameraFrameState& frameState) const;
    QMap<QString, CameraFrameState> snapshotCameraFrames() const;
    std::vector<CameraRenderInfo> buildCameraRenderInfos(const QMap<QString, CameraFrameState>& cameraFrames) const;
    void buildRenderSnapshot(QMap<QString, CameraFrameState>& cameraFrames,
                             std::vector<CameraRenderInfo>& cameraRenderInfos,
                             std::vector<RenderItem>& renderItems) const;
    bool resolveDisplayGeometry(const CameraFrameState& frameState,
                                bool processed,
                                const QRect& area,
                                QRect& displayRect,
                                QSize& imageSize) const;
    bool mapWidgetPositionToImage(const CameraFrameState& frameState,
                                  bool processed,
                                  const QRect& area,
                                  const QPoint& widgetPos,
                                  QPoint& imagePos) const;
    void paintPlaceholder(const QString& text);
    void drawRenderItem(const RenderItem& item);
    void ensureGlPipeline();
    void drawRawInRect(const QString& cameraId, const CameraFrameState& frameState, const QRect& r, float alpha);
    void drawFrameInRect(const QString& textureKey,
                         const scopeone::core::ImageFrame& frame,
                         const QRect& r,
                         float alpha,
                         bool flipX,
                         bool flipY,
                         int levelMin,
                         int levelMax,
                         int levelDomainMax);
    QRect targetRectForImageSize(const QSize& imageSize,
                                 const CameraFrameState& frameState,
                                 const QRect& avail) const;
    void setUvTransform(bool flipX, bool flipY);
    void applyViewportForRect(const QRect& logicalRect);
    std::vector<QRect> computeLayout(int count) const;
    std::vector<RenderItem> buildRenderItems(const std::vector<CameraRenderInfo>& cameraRenderInfos) const;
    bool locateRenderTarget(const QPoint& widgetPos,
                            QString& cameraId,
                            bool& processed,
                            QRect& itemArea,
                            QPointF& relativePos) const;

    GLuint getOrCreateTexture(const QString& key, int width, int height, GLenum internalFormat);
    void cleanupTextureCache();
    void cancelROIDrawing();
    void cancelLineDrawing();
};

}
