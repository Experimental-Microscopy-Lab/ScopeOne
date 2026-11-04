#pragma once

#include <QList>
#include <QString>

#include <functional>

#include "scopeone/DaqDevice.h"
#include "scopeone/scopeone_sdk_export.h"

namespace scopeone::core
{
    // A hardware provider of data acquisition devices, which run timed analog, digital and
    // counter tasks: they put out scan waveforms and clocks and read their inputs continuously
    class SCOPEONE_SDK_EXPORT DaqProvider
    {
    public:
        using InputSink = std::function<void(const DaqInputChunk&)>;
        using StateSink = std::function<void(const QString& deviceId,
                                             DaqState state,
                                             const QString& message)>;

        virtual ~DaqProvider();

        virtual QList<DaqDeviceDescriptor> daqDevices() const = 0;
        // The sinks may be called from any thread, and the input sink in the order of the samples
        virtual void setDaqSinks(InputSink input, StateSink state) = 0;
        // Raster scans of the session are already expanded into its tasks
        virtual bool startDaq(const DaqSessionConfig& config,
                              QString* errorMessage = nullptr) = 0;
        virtual void stopDaq(const QString& deviceId) = 0;
    };
}
