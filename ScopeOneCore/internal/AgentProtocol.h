#pragma once

#include <QString>

#include "internal/DriverHostProtocol.h"

namespace scopeone::core::internal::agent
{
    using driverhost::kProtocolVersion;
    using driverhost::kEnvelopeKindField;
    using driverhost::kEnvelopeVersionField;
    using driverhost::kEnvelopeRequestIdField;
    using driverhost::kMessageTypeField;
    using driverhost::kMessageKindRequest;
    using driverhost::kMessageKindResponse;
    using driverhost::kMessageKindEvent;
    using driverhost::encodeUInt64;
    using driverhost::decodeUInt64;
    using driverhost::makeEnvelope;
    using driverhost::encodeMessage;
    using driverhost::DecodeResult;
    using driverhost::tryDecodeMessage;

    inline const QString kCommandShutdown = QStringLiteral("Shutdown");
    inline const QString kCommandStartPreview = QStringLiteral("StartPreview");
    inline const QString kCommandStopPreview = QStringLiteral("StopPreview");
    inline const QString kCommandSetFrameDeliveryMode = QStringLiteral("SetFrameDeliveryMode");
    inline const QString kCommandSetExposure = QStringLiteral("SetExposure");
    inline const QString kCommandListProperties = QStringLiteral("ListProperties");
    inline const QString kCommandGetProperty = QStringLiteral("GetProperty");
    inline const QString kCommandSetProperty = QStringLiteral("SetProperty");
    inline const QString kCommandCaptureEvent = QStringLiteral("CaptureEvent");
    inline const QString kCommandSetRoi = QStringLiteral("SetROI");
    inline const QString kCommandClearRoi = QStringLiteral("ClearROI");
    inline const QString kCommandGetRoi = QStringLiteral("GetROI");

    inline const QString kFrameDeliveryModePreviewLatest = QStringLiteral("PreviewLatest");
    inline const QString kFrameDeliveryModeLatestOnly = QStringLiteral("LatestOnly");
    inline const QString kFrameDeliveryModeAllFrames = QStringLiteral("AllFrames");

    inline const QString kEventHello = QStringLiteral("Hello");
    inline const QString kEventFrameAvailable = QStringLiteral("FrameAvailable");
    inline const QString kEventPreviewState = QStringLiteral("PreviewState");
    inline const QString kEventAgentError = QStringLiteral("AgentError");

    inline const QString kExecutableFileName = QStringLiteral("ScopeOne_Agent.exe");

    inline QString controlServerName(const QString& cameraId)
    {
        return QStringLiteral("ScopeOne.%1.ctrl").arg(cameraId);
    }

    inline QString sharedMemoryKey(const QString& cameraId)
    {
        return QStringLiteral("ScopeOne.%1.shm").arg(cameraId);
    }

} // namespace scopeone::core::internal::agent
