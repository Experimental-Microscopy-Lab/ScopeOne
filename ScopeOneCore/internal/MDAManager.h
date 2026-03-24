#pragma once

#include <QObject>
#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QtGlobal>
#include <QMetaType>
#include <atomic>
#include <memory>
#include <vector>

class CMMCore;

namespace scopeone::core::internal {

class MultiProcessCameraManager;

struct MDAEvent {
    int tIndex{0};
    int zIndex{0};
    int positionIndex{0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
    bool hasXY{false};
    bool hasZ{false};
    double exposureMs{0.0};
    qint64 minStartTimeMs{0};
};

struct CameraFrame {
    QString cameraId;
    QByteArray raw;
    int width{0};
    int height{0};
    int bytesPerPixel{0};
    int bitsPerSample{0};
};

struct MDAOutput {
    MDAEvent event;
    QMap<QString, CameraFrame> frames;
    QByteArray raw;
    int width{0};
    int height{0};
    int bytesPerPixel{0};
    int bitsPerSample{0};
    qint64 timestampMs{0};
};

enum class MDAAxis {
    Time,
    Z,
    XY
};

using MDAOrder = std::vector<MDAAxis>;

class MDAManager : public QObject
{
    Q_OBJECT

public:
    explicit MDAManager(std::shared_ptr<CMMCore> core, QObject* parent = nullptr);

    bool isRunning() const { return m_running.load(); }

    void setCameras(const QStringList& cameraIds);
    void setMultiProcessCameraManager(MultiProcessCameraManager* mpcm);

    void start(int timePoints,
               double timeIntervalMs,
               double exposureMs,
               const std::vector<QPointF>& positions,
               const std::vector<double>& zPositions,
               const MDAOrder& order,
               bool block = false);
    void requestCancel();

signals:
    void frameReady(const scopeone::core::internal::MDAOutput& frame);
    void sequenceFinished();
    void sequenceCanceled();
    void sequenceError(const QString& message);

private:
    bool setupEvent(const MDAEvent& event, QString* errorMessage);
    bool execEvent(const MDAEvent& event, MDAOutput& outFrame, QString* errorMessage);
    bool execEventSingleCamera(const MDAEvent& event, MDAOutput& outFrame, QString* errorMessage);
    bool execEventMultiCamera(const MDAEvent& event, MDAOutput& outFrame, QString* errorMessage);
    bool setExposure(double exposureMs, QString* errorMessage);
    bool moveXY(double x, double y, QString* errorMessage);
    bool moveZ(double z, QString* errorMessage);
    void runSequence(int timePoints,
                     double timeIntervalMs,
                     double exposureMs,
                     std::vector<QPointF> positions,
                     std::vector<double> zPositions,
                     MDAOrder order);

    std::shared_ptr<CMMCore> m_mmcore;
    MultiProcessCameraManager* m_mpcm{nullptr};
    QStringList m_cameraIds;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelRequested{false};
};

}

Q_DECLARE_METATYPE(scopeone::core::internal::MDAEvent)
Q_DECLARE_METATYPE(scopeone::core::internal::CameraFrame)
Q_DECLARE_METATYPE(scopeone::core::internal::MDAOutput)
