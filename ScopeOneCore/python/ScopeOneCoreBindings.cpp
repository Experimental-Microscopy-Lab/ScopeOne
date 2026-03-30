#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QString>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "scopeone/ScopeOneCore.h"

namespace py = pybind11;

namespace {

using ScopeOneCore = scopeone::core::ScopeOneCore;
using RecordingFileManifest = scopeone::core::ScopeOneCore::RecordingFileManifest;
using RecordingFormat = scopeone::core::RecordingFormat;
using RecordingFrame = scopeone::core::ScopeOneCore::RecordingFrame;
using RecordingSessionData = scopeone::core::ScopeOneCore::RecordingSessionData;

void ensureQtCoreApp()
{
    if (QCoreApplication::instance()) {
        return;
    }

    static int argc = 1;
    static char appName[] = "scopeone_core";
    static char* argv[] = { appName, nullptr };
    static QCoreApplication app(argc, argv);
    (void)app;
}

std::vector<std::string> toStdVector(const QStringList& values)
{
    std::vector<std::string> out;
    out.reserve(static_cast<size_t>(values.size()));
    for (const QString& value : values) {
        out.push_back(value.toStdString());
    }
    return out;
}

QString toLowerQString(const std::string& value)
{
    return QString::fromStdString(value).trimmed().toLower();
}

RecordingFormat parseRecordingFormat(const std::string& formatName)
{
    const QString normalized = toLowerQString(formatName);
    if (normalized == QStringLiteral("tiff") || normalized == QStringLiteral("tif")) {
        return RecordingFormat::Tiff;
    }
    if (normalized == QStringLiteral("binary") || normalized == QStringLiteral("bin")) {
        return RecordingFormat::Binary;
    }
    throw std::runtime_error("format must be 'tiff' or 'binary'");
}

QStringList resolveCameraIds(const std::shared_ptr<ScopeOneCore>& core, const std::string& cameraIdOrAll)
{
    const QStringList availableCameraIds = core->cameraIds();
    if (availableCameraIds.isEmpty()) {
        throw std::runtime_error("No cameras available");
    }

    const QString target = QString::fromStdString(cameraIdOrAll).trimmed();
    if (target.isEmpty()) {
        throw std::runtime_error("camera must be 'All' or a concrete camera ID");
    }
    if (target.compare(QStringLiteral("All"), Qt::CaseInsensitive) == 0) {
        return availableCameraIds;
    }
    if (!availableCameraIds.contains(target)) {
        throw std::runtime_error(QString("Unknown camera ID: %1").arg(target).toStdString());
    }

    return QStringList{target};
}

std::vector<std::string> collectSavedPaths(const std::shared_ptr<RecordingSessionData>& session)
{
    std::vector<std::string> paths;
    if (!session) {
        return paths;
    }

    const auto& files = session->outputFiles();
    const QStringList cameraIds = session->recordedCameraIds();
    paths.reserve(static_cast<size_t>(cameraIds.size()));
    for (const QString& cameraId : cameraIds) {
        const auto it = files.constFind(cameraId);
        if (it == files.constEnd()) {
            continue;
        }
        const RecordingFileManifest& manifest = it.value();
        if (!manifest.rawPath.isEmpty()) {
            paths.push_back(manifest.rawPath.toStdString());
        }
    }
    return paths;
}

class PyRecordingSession
{
public:
    PyRecordingSession(std::shared_ptr<ScopeOneCore> scopeonecore,
                       std::shared_ptr<RecordingSessionData> session)
        : m_scopeonecore(std::move(scopeonecore))
          , m_session(std::move(session))
    {
        if (!m_scopeonecore || !m_session) {
            throw std::runtime_error("Recording session is not available");
        }
    }

    std::vector<std::string> cameraIds() const
    {
        return toStdVector(m_session->recordedCameraIds());
    }

    py::ssize_t frameCount(const std::optional<std::string>& cameraId) const
    {
        if (cameraId && !cameraId->empty()) {
            const auto* frames = m_session->framesForCamera(QString::fromStdString(*cameraId));
            return frames ? static_cast<py::ssize_t>(frames->size()) : 0;
        }

        return static_cast<py::ssize_t>(m_session->frameCount());
    }

    py::array frame(const std::string& cameraId, py::ssize_t index) const
    {
        const auto* frames = requireFrames(cameraId);
        if (index < 0 || index >= static_cast<py::ssize_t>(frames->size())) {
            throw std::runtime_error("frame index out of range");
        }
        return frameToArray(frames->at(static_cast<size_t>(index)));
    }

    py::list frames(const std::string& cameraId) const
    {
        const auto* frames = requireFrames(cameraId);
        py::list out;
        for (const RecordingFrame& frame : *frames) {
            out.append(frameToArray(frame));
        }
        return out;
    }

    std::vector<std::string> save(const std::string& saveDir,
                                  const std::string& baseName,
                                  const std::string& formatName,
                                  bool compression,
                                  int compressionLevel)
    {
        const QString saveDirValue = QString::fromStdString(saveDir).trimmed();
        if (saveDirValue.isEmpty()) {
            throw std::runtime_error("saveDir must not be empty");
        }

        const QString baseNameValue = QString::fromStdString(baseName).trimmed();
        if (baseNameValue.isEmpty()) {
            throw std::runtime_error("baseName must not be empty");
        }

        auto capturePlan = m_session->capturePlan();
        capturePlan.saveDir = saveDirValue;
        capturePlan.baseName = baseNameValue;
        capturePlan.format = parseRecordingFormat(formatName);
        capturePlan.enableCompression = compression;
        capturePlan.compressionLevel = compressionLevel;
        m_session->setCapturePlan(capturePlan);

        QString saveResult;
        {
            py::gil_scoped_release release;
            saveResult = m_scopeonecore->saveRecordingSession(m_session);
        }

        if (saveResult.startsWith(QStringLiteral("Error:"), Qt::CaseInsensitive)) {
            throw std::runtime_error(saveResult.toStdString());
        }

        return collectSavedPaths(m_session);
    }

private:
    const std::vector<RecordingFrame>* requireFrames(const std::string& cameraId) const
    {
        const QString cameraIdValue = QString::fromStdString(cameraId).trimmed();
        if (cameraIdValue.isEmpty()) {
            throw std::runtime_error("camera must not be empty");
        }

        const auto* frames = m_session->framesForCamera(cameraIdValue);
        if (!frames || frames->empty()) {
            throw std::runtime_error(QString("No frames captured for camera %1").arg(cameraIdValue).toStdString());
        }
        return frames;
    }

    static py::array frameToArray(const RecordingFrame& frame)
    {
        if (frame.width <= 0 || frame.height <= 0) {
            throw std::runtime_error("Recording frame has invalid dimensions");
        }

        const py::ssize_t height = static_cast<py::ssize_t>(frame.height);
        const py::ssize_t width = static_cast<py::ssize_t>(frame.width);
        const size_t expectedPixels = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
        const bool is16Bit =
            frame.header.pixelFormat == static_cast<quint32>(scopeone::core::SharedPixelFormat::Mono16)
            || frame.bits > 8;

        if (is16Bit) {
            const size_t expectedBytes = expectedPixels * sizeof(std::uint16_t);
            if (frame.rawData.size() < static_cast<int>(expectedBytes)) {
                throw std::runtime_error("Recording frame payload is smaller than expected");
            }
            py::array_t<std::uint16_t> out(py::array::ShapeContainer{height, width});
            std::memcpy(out.mutable_data(), frame.rawData.constData(), expectedBytes);
            return out;
        }

        const size_t expectedBytes = expectedPixels;
        if (frame.rawData.size() < static_cast<int>(expectedBytes)) {
            throw std::runtime_error("Recording frame payload is smaller than expected");
        }
        py::array_t<std::uint8_t> out(py::array::ShapeContainer{height, width});
        std::memcpy(out.mutable_data(), frame.rawData.constData(), expectedBytes);
        return out;
    }

    std::shared_ptr<ScopeOneCore> m_scopeonecore;
    std::shared_ptr<RecordingSessionData> m_session;
};

class PyScopeOne
{
public:
    PyScopeOne()
        : m_scopeonecore(createCore())
    {
    }

    bool loadConfig(const std::string& configPath)
    {
        ScopeOneCore::LoadConfigResult result;
        QString errorMessage;
        if (!m_scopeonecore->loadConfiguration(QString::fromStdString(configPath), &result, &errorMessage)) {
            throw std::runtime_error(
                errorMessage.isEmpty() ? std::string("Failed to load configuration")
                                       : errorMessage.toStdString());
        }
        return true;
    }

    void unloadConfig()
    {
        m_scopeonecore->unloadConfiguration();
    }

    std::vector<std::string> cameraIds() const
    {
        return toStdVector(m_scopeonecore->cameraIds());
    }

    PyRecordingSession record(int frameCount,
                              const std::string& cameraIdOrAll,
                              int timeoutMs)
    {
        if (frameCount <= 0) {
            throw std::runtime_error("frameCount must be > 0");
        }

        const QStringList activeCameraIds = resolveCameraIds(m_scopeonecore, cameraIdOrAll);
        const QString target = QString::fromStdString(cameraIdOrAll).trimmed();

        ScopeOneCore::RecordingSettings settings;
        settings.format = RecordingFormat::Tiff;
        settings.streamToDisk = false;
        settings.framesPerBurst = frameCount;
        settings.burstMode = false;
        settings.targetBursts = 1;
        settings.enableCompression = false;
        settings.captureAll = true;

        std::shared_ptr<RecordingSessionData> recordedSession;
        QMetaObject::Connection conn = QObject::connect(
            m_scopeonecore.get(),
            &ScopeOneCore::recordingStopped,
            m_scopeonecore.get(),
            [&recordedSession](const std::shared_ptr<RecordingSessionData>& session)
            {
                recordedSession = session;
            });

        const auto disconnectSignal = [conn]() { QObject::disconnect(conn); };

        bool previewStarted = false;
        try {
            m_scopeonecore->startPreview(target);
            previewStarted = true;

            if (!m_scopeonecore->startRecording(settings, activeCameraIds)) {
                throw std::runtime_error("Failed to start recording");
            }

            {
                py::gil_scoped_release release;

                QElapsedTimer timer;
                timer.start();
                const int waitTimeoutMs = (timeoutMs > 0) ? timeoutMs : 120000;
                while (m_scopeonecore->isRecording() && timer.elapsed() < waitTimeoutMs) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                if (m_scopeonecore->isRecording()) {
                    m_scopeonecore->stopRecording();
                }

                for (int i = 0; i < 20 && !recordedSession; ++i) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }

            if (previewStarted) {
                m_scopeonecore->stopPreview(target);
            }
            disconnectSignal();
        } catch (...) {
            if (m_scopeonecore->isRecording()) {
                m_scopeonecore->stopRecording();
            }
            if (previewStarted) {
                m_scopeonecore->stopPreview(target);
            }
            disconnectSignal();
            throw;
        }

        if (!recordedSession) {
            throw std::runtime_error("Recording finished but no session data was returned");
        }
        if (!recordedSession->hasAnyFrames()) {
            throw std::runtime_error("Recording finished but captured no frames");
        }

        return PyRecordingSession(m_scopeonecore, recordedSession);
    }

private:
    static std::shared_ptr<ScopeOneCore> createCore()
    {
        ensureQtCoreApp();
        return std::make_shared<ScopeOneCore>();
    }

    std::shared_ptr<ScopeOneCore> m_scopeonecore;
};

} // namespace

PYBIND11_MODULE(_core, m)
{
    m.doc() = "ScopeOne Python bindings";

    py::class_<PyRecordingSession>(m, "RecordingSession")
        .def("camera_ids",
             &PyRecordingSession::cameraIds,
             "Return camera ids captured in this session.")
        .def("frame_count",
             &PyRecordingSession::frameCount,
             py::arg("camera") = py::none(),
             "Return the total frame count or the frame count for one camera.")
        .def("frame",
             &PyRecordingSession::frame,
             py::arg("camera"),
             py::arg("index"),
             "Return one recorded frame as a NumPy array.")
        .def("frames",
             &PyRecordingSession::frames,
             py::arg("camera"),
             "Return all recorded frames for one camera as NumPy arrays.")
        .def("save",
             &PyRecordingSession::save,
             py::arg("save_dir"),
             py::arg("base_name"),
             py::arg("format") = "tiff",
             py::arg("compression") = false,
             py::arg("compression_level") = 6,
             "Save the buffered recording session to disk and return the raw output paths.");

    py::class_<PyScopeOne>(m, "ScopeOne")
        .def(py::init<>())
        .def("load",
             &PyScopeOne::loadConfig,
             py::arg("config_path"),
             "Load a .cfg file.")
        .def("unload",
             &PyScopeOne::unloadConfig,
             "Unload the current configuration.")
        .def("camera_ids",
             &PyScopeOne::cameraIds,
             "Return available camera ids.")
        .def("record",
             &PyScopeOne::record,
             py::arg("frame_count"),
             py::arg("camera") = "All",
             py::arg("timeout_ms") = 120000,
             "Record frames into an in-memory session.");
}
