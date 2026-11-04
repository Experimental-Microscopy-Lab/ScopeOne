#pragma once

#include <QByteArray>
#include <QJsonObject>

#include "scopeone/DaqDevice.h"
#include "scopeone/SignalSource.h"

// How signal sources and DAQ devices cross the DriverHost boundary: descriptors and
// configurations as JSON in the control messages, data chunks as binary records of the stream
// ring. Both ends run on the same machine, so the arrays are copied in native byte order.
namespace scopeone::core::internal::hardwarestream
{
    QJsonObject toJson(const SignalSourceDescriptor& descriptor);
    SignalSourceDescriptor signalSourceDescriptorFromJson(const QJsonObject& object);
    QJsonObject toJson(const SignalAcquisitionConfig& config);
    SignalAcquisitionConfig signalAcquisitionConfigFromJson(const QJsonObject& object);

    QJsonObject toJson(const DaqDeviceDescriptor& descriptor);
    DaqDeviceDescriptor daqDeviceDescriptorFromJson(const QJsonObject& object);
    QJsonObject toJson(const DaqSessionConfig& config);
    DaqSessionConfig daqSessionConfigFromJson(const QJsonObject& object);

    QByteArray encode(const TimeSeriesChunk& chunk);
    QByteArray encode(const TimestampedEventChunk& chunk);
    QByteArray encode(const DaqInputChunk& chunk);
    bool decode(const QByteArray& payload, TimeSeriesChunk& chunk);
    bool decode(const QByteArray& payload, TimestampedEventChunk& chunk);
    bool decode(const QByteArray& payload, DaqInputChunk& chunk);
}
