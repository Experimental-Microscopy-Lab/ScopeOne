#include "DhmReconstruction.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
    using Complex = std::complex<float>;
    constexpr float Pi = 3.14159265358979323846f;

    int nextPowerOfTwo(int value)
    {
        int size = 1;
        while (size < value)
        {
            size <<= 1;
        }
        return size;
    }

    int wrapIndex(int value, int size)
    {
        value %= size;
        return value < 0 ? value + size : value;
    }

    void fft1d(std::vector<Complex>& values, bool inverse)
    {
        const int size = static_cast<int>(values.size());
        for (int i = 1, j = 0; i < size; ++i)
        {
            int bit = size >> 1;
            for (; j & bit; bit >>= 1)
            {
                j ^= bit;
            }
            j ^= bit;
            if (i < j)
            {
                std::swap(values[i], values[j]);
            }
        }

        for (int length = 2; length <= size; length <<= 1)
        {
            const float angle = 2.0f * Pi / static_cast<float>(length)
                              * (inverse ? 1.0f : -1.0f);
            const Complex step(std::cos(angle), std::sin(angle));
            for (int start = 0; start < size; start += length)
            {
                Complex factor(1.0f, 0.0f);
                const int half = length / 2;
                for (int index = 0; index < half; ++index)
                {
                    const Complex even = values[start + index];
                    const Complex odd = factor * values[start + index + half];
                    values[start + index] = even + odd;
                    values[start + index + half] = even - odd;
                    factor *= step;
                }
            }
        }

        if (inverse)
        {
            const float scale = 1.0f / static_cast<float>(size);
            for (Complex& value : values)
            {
                value *= scale;
            }
        }
    }

    void fft2d(std::vector<Complex>& values,
               int width,
               int height,
               bool inverse,
               const std::atomic_bool& cancel,
               const std::function<void(int)>& progress,
               int progressStart,
               int progressEnd)
    {
        std::vector<Complex> line(static_cast<size_t>(std::max(width, height)));
        for (int y = 0; y < height; ++y)
        {
            if (cancel)
            {
                return;
            }
            line.assign(values.begin() + static_cast<size_t>(y) * width,
                        values.begin() + static_cast<size_t>(y + 1) * width);
            fft1d(line, inverse);
            std::copy(line.begin(), line.begin() + width,
                      values.begin() + static_cast<size_t>(y) * width);
            progress(progressStart + (progressEnd - progressStart) * y / (2 * height));
        }

        line.resize(height);
        for (int x = 0; x < width; ++x)
        {
            if (cancel)
            {
                return;
            }
            for (int y = 0; y < height; ++y)
            {
                line[y] = values[static_cast<size_t>(y) * width + x];
            }
            fft1d(line, inverse);
            for (int y = 0; y < height; ++y)
            {
                values[static_cast<size_t>(y) * width + x] = line[y];
            }
            progress(progressStart + (progressEnd - progressStart)
                                   * (height + x) / (height + width));
        }
    }

    std::vector<Complex> toComplex(const scopeone::core::ImageFrame& frame,
                                   int width,
                                   int height)
    {
        std::vector<Complex> values(static_cast<size_t>(width) * height);
        for (int y = 0; y < frame.height; ++y)
        {
            const auto* row = reinterpret_cast<const unsigned char*>(frame.bytes.constData()
                                                                       + y * frame.stride);
            for (int x = 0; x < frame.width; ++x)
            {
                const float value = frame.isMono16()
                                        ? static_cast<float>(reinterpret_cast<const unsigned short*>(row)[x])
                                        : static_cast<float>(row[x]);
                values[static_cast<size_t>(y) * width + x] = Complex(value, 0.0f);
            }
        }
        return values;
    }

    scopeone::core::ImageFrame phaseFrame(const scopeone::core::ImageFrame& input,
                                          const std::vector<Complex>& field,
                                          int width)
    {
        scopeone::core::ImageFrame output;
        output.cameraId = QStringLiteral("dhm.phase");
        output.width = input.width;
        output.height = input.height;
        output.stride = input.width * static_cast<int>(sizeof(unsigned short));
        output.bitsPerSample = 16;
        output.pixelFormat = scopeone::core::ImagePixelFormat::Mono16;
        output.frameIndex = input.frameIndex;
        output.timestampNs = input.timestampNs;
        output.bytes.resize(output.stride * output.height);

        for (int y = 0; y < input.height; ++y)
        {
            auto* row = reinterpret_cast<unsigned short*>(output.bytes.data()
                                                           + y * output.stride);
            for (int x = 0; x < input.width; ++x)
            {
                float phase = std::arg(field[static_cast<size_t>(y) * width + x]);
                phase = (phase + Pi) / (2.0f * Pi);
                row[x] = static_cast<unsigned short>(std::clamp(phase, 0.0f, 1.0f)
                                                      * 65535.0f);
            }
        }
        return output;
    }
}

namespace scopeone::dhm
{
    scopeone::core::ImageFrame reconstructPhase(
        const scopeone::core::ImageFrame& input,
        int sidebandX,
        int sidebandY,
        int radius,
        const std::atomic_bool& cancel,
        const std::function<void(int)>& progress)
    {
        if (!input.isValid() || (!input.isMono8() && !input.isMono16()))
        {
            return {};
        }

        const int width = nextPowerOfTwo(input.width);
        const int height = nextPowerOfTwo(input.height);
        const int safeRadius = std::clamp(radius, 1, std::min(width, height) / 2);
        const int effectiveSidebandX = std::clamp(sidebandX, -width / 2, width / 2 - 1);
        const int effectiveSidebandY = std::clamp(sidebandY, -height / 2, height / 2 - 1);
        const int centerX = width / 2 + effectiveSidebandX;
        const int centerY = height / 2 + effectiveSidebandY;
        auto field = toComplex(input, width, height);
        progress(5);

        fft2d(field, width, height, false, cancel, progress, 5, 45);
        if (cancel)
        {
            return {};
        }

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int shiftedX = (x + width / 2) % width;
                const int shiftedY = (y + height / 2) % height;
                const int dx = shiftedX - centerX;
                const int dy = shiftedY - centerY;
                if (dx * dx + dy * dy > safeRadius * safeRadius)
                {
                    field[static_cast<size_t>(y) * width + x] = Complex(0.0f, 0.0f);
                }
            }
        }

        // Move the selected carrier sideband to the zero-frequency origin.
        std::vector<Complex> demodulated(field.size(), Complex(0.0f, 0.0f));
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int shiftedX = (x + width / 2) % width;
                const int shiftedY = (y + height / 2) % height;
                const int dx = shiftedX - centerX;
                const int dy = shiftedY - centerY;
                if (dx * dx + dy * dy > safeRadius * safeRadius)
                {
                    continue;
                }
                const int targetX = wrapIndex(x - effectiveSidebandX, width);
                const int targetY = wrapIndex(y - effectiveSidebandY, height);
                demodulated[static_cast<size_t>(targetY) * width + targetX]
                    = field[static_cast<size_t>(y) * width + x];
            }
        }
        field.swap(demodulated);
        progress(55);

        fft2d(field, width, height, true, cancel, progress, 55, 95);
        if (cancel)
        {
            return {};
        }
        progress(100);
        return phaseFrame(input, field, width);
    }
}
