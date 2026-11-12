#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

#include "scopeone/scopeone_sdk_export.h"
#include "scopeone/ImageFrame.h"

namespace scopeone::core
{
    // A device that includes cameras. Every such device implements this interface, the drivers
    // as well as Micro-Manager inside ScopeOne, and every call names one camera by its id.
    //
    // Threads: DriverHost calls the methods of a driver one at a time, from one thread, so they
    // need no locks against each other. The frame sink may be called from any thread, such as a
    // callback thread of the camera SDK, while a method runs.
    //
    // Frames: Mono8 or Mono16, at most 16 MB each; a larger frame is not delivered. frameIndex
    // counts the images the camera took, from 1, including those the device did not hand on,
    // so ScopeOne can tell the frame rate and see frames that were lost. timestampNs is the time
    // the image was taken, in nanoseconds since the Unix epoch, or 0 to let ScopeOne stamp it.
    class SCOPEONE_SDK_EXPORT Camera
    {
    public:
        // ScopeOne copies the frame before the sink returns, so the bytes of a frame may point
        // into a buffer of the camera, for example through QByteArray::fromRawData, and need to
        // stay valid only during the call. The copy takes about a millisecond for 8 MB.
        using FrameSink = std::function<void(const ImageFrame&)>;
        // Called with whether any camera of the device is running, whenever that may change
        using PreviewStateSink = std::function<void(bool)>;

        virtual ~Camera();

        // Once a new sink is set, the old one is no longer called from any thread
        virtual void setFrameSink(FrameSink sink) = 0;
        virtual void setPreviewStateSink(PreviewStateSink sink) = 0;
        // Starts the continuous acquisition of a camera. Starting a running camera succeeds.
        virtual bool startPreview(const QString& cameraId) = 0;
        // Once it returns, the camera hands no further frame to the sink
        virtual bool stopPreview(const QString& cameraId) = 0;
        virtual bool isPreviewRunning(const QString& cameraId) const = 0;
        virtual bool getExposure(const QString& cameraId, double& exposureMs) const = 0;
        virtual bool setExposure(const QString& cameraId, double exposureMs) = 0;
        virtual bool setROI(const QString& cameraId, int x, int y, int width, int height) = 0;
        virtual bool clearROI(const QString& cameraId) = 0;
        virtual bool getROI(const QString& cameraId,
                            int& x,
                            int& y,
                            int& width,
                            int& height) = 0;
        // Takes one new image with the current settings, started after the call, and returns it
        // instead of handing it to the sink. A running acquisition may be stopped for it.
        // timeoutMs is how long ScopeOne waits for the image; a device that waits for the camera
        // itself should give up after it.
        virtual bool snap(const QString& cameraId,
                          ImageFrame& frame,
                          int timeoutMs = 1500) = 0;
        // Whether the camera can take a series of images in one sequence acquisition, which
        // lets the hardware time the images, for example while a stage steps through a
        // sequence of positions
        virtual bool supportsSequence(const QString& cameraId) const
        {
            (void)cameraId;
            return false;
        }
        // Takes count images in one sequence acquisition of the camera
        virtual bool snapSequence(const QString& cameraId,
                                  int count,
                                  std::vector<ImageFrame>& frames,
                                  int timeoutMs = 10000)
        {
            (void)cameraId;
            (void)count;
            (void)frames;
            (void)timeoutMs;
            return false;
        }
    };
}
