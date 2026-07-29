#include "internal/OmeZarrStorage.h"

#include <QDir>
#include <QFile>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

#include "crc32c/crc32c.h"
#include "zstd.h"

namespace scopeone::core::internal
{
    namespace
    {
        constexpr int kChunkEdge = 512;
        constexpr quint64 kUnwrittenChunk = (std::numeric_limits<quint64>::max)();

        qint64 timeIndexForEvent(const AcquisitionEvent& event, const ExperimentPlan& plan)
        {
            return static_cast<qint64>(event.burstIndex) * (std::max)(1, plan.framesPerBurst)
                + event.timeIndex;
        }

        QString datasetPath(const QString& rootPath, int positionIndex, bool multiPosition)
        {
            if (!multiPosition)
            {
                return QDir(rootPath).filePath(QStringLiteral("0"));
            }
            return QDir(rootPath).filePath(
                QStringLiteral("Position %1/0").arg(positionIndex + 1));
        }

        quint64 readLittleEndian64(const char* data)
        {
            quint64 value = 0;
            for (int index = 0; index < 8; ++index)
            {
                value |= static_cast<quint64>(static_cast<unsigned char>(data[index]))
                    << (index * 8);
            }
            return value;
        }

        quint32 readLittleEndian32(const char* data)
        {
            quint32 value = 0;
            for (int index = 0; index < 4; ++index)
            {
                value |= static_cast<quint32>(static_cast<unsigned char>(data[index]))
                    << (index * 8);
            }
            return value;
        }

        ImageFrame readPlane(const QString& path,
                             const QString& cameraId,
                             const FrameRecord& record,
                             qint64 t,
                             int z,
                             bool compressed)
        {
            if (record.width <= 0 || record.height <= 0 || t < 0 || z < 0)
            {
                return {};
            }
            const int bytesPerPixel = record.pixelFormat == ImagePixelFormat::Mono8
                                          ? 1
                                          : record.pixelFormat == ImagePixelFormat::Mono16 ? 2 : 0;
            if (bytesPerPixel == 0)
            {
                return {};
            }

            const int chunkWidth = (std::min)(record.width, kChunkEdge);
            const int chunkHeight = (std::min)(record.height, kChunkEdge);
            const int chunksX = (record.width + chunkWidth - 1) / chunkWidth;
            const int chunksY = (record.height + chunkHeight - 1) / chunkHeight;
            const quint64 chunksPerShard = static_cast<quint64>(chunksX) * chunksY;
            if (chunksPerShard == 0
                || chunksPerShard > (std::numeric_limits<qsizetype>::max)() / 16)
            {
                return {};
            }

            QFile shard(QDir(path).filePath(
                QStringLiteral("c/%1/0/%2/0/0").arg(t).arg(z)));
            if (!shard.open(QIODevice::ReadOnly))
            {
                return {};
            }
            const qint64 tableBytes = static_cast<qint64>(chunksPerShard * 16);
            const qint64 indexBytes = tableBytes + 4;
            if (shard.size() < indexBytes || !shard.seek(shard.size() - indexBytes))
            {
                return {};
            }
            const QByteArray index = shard.read(indexBytes);
            if (index.size() != indexBytes)
            {
                return {};
            }
            const quint32 expectedChecksum = readLittleEndian32(index.constData() + tableBytes);
            const quint32 actualChecksum = crc32c::Crc32c(
                reinterpret_cast<const std::uint8_t*>(index.constData()),
                static_cast<size_t>(tableBytes));
            if (expectedChecksum != actualChecksum)
            {
                return {};
            }

            const qint64 stride = static_cast<qint64>(record.width) * bytesPerPixel;
            const qint64 byteCount = stride * record.height;
            const qint64 chunkBytes = static_cast<qint64>(chunkWidth)
                * chunkHeight * bytesPerPixel;
            if (stride > (std::numeric_limits<int>::max)()
                || byteCount <= 0
                || byteCount > (std::numeric_limits<qsizetype>::max)()
                || chunkBytes <= 0
                || chunkBytes > (std::numeric_limits<qsizetype>::max)())
            {
                return {};
            }

            QByteArray bytes(static_cast<qsizetype>(byteCount), '\0');
            QByteArray decoded(static_cast<qsizetype>(chunkBytes), '\0');
            for (int chunkY = 0; chunkY < chunksY; ++chunkY)
            {
                for (int chunkX = 0; chunkX < chunksX; ++chunkX)
                {
                    const quint64 chunkIndex = static_cast<quint64>(chunkY) * chunksX + chunkX;
                    const char* entry = index.constData() + static_cast<qint64>(chunkIndex * 16);
                    const quint64 offset = readLittleEndian64(entry);
                    const quint64 extent = readLittleEndian64(entry + 8);
                    if (offset == kUnwrittenChunk && extent == kUnwrittenChunk)
                    {
                        continue;
                    }
                    if (offset > static_cast<quint64>(shard.size() - indexBytes)
                        || extent > static_cast<quint64>(shard.size() - indexBytes) - offset
                        || extent > static_cast<quint64>(
                            (std::numeric_limits<qsizetype>::max)()))
                    {
                        return {};
                    }
                    if (!shard.seek(static_cast<qint64>(offset)))
                    {
                        return {};
                    }
                    const QByteArray payload = shard.read(static_cast<qint64>(extent));
                    if (payload.size() != static_cast<qsizetype>(extent))
                    {
                        return {};
                    }
                    if (compressed)
                    {
                        const size_t result = ZSTD_decompress(decoded.data(),
                                                              static_cast<size_t>(chunkBytes),
                                                              payload.constData(),
                                                              static_cast<size_t>(extent));
                        if (ZSTD_isError(result) || result != static_cast<size_t>(chunkBytes))
                        {
                            return {};
                        }
                    }
                    else
                    {
                        if (payload.size() != chunkBytes)
                        {
                            return {};
                        }
                        decoded = payload;
                    }

                    const int destinationX = chunkX * chunkWidth;
                    const int destinationY = chunkY * chunkHeight;
                    const int copyWidth = (std::min)(chunkWidth, record.width - destinationX);
                    const int copyHeight = (std::min)(chunkHeight, record.height - destinationY);
                    for (int row = 0; row < copyHeight; ++row)
                    {
                        std::memcpy(bytes.data()
                                        + static_cast<qint64>(destinationY + row) * stride
                                        + static_cast<qint64>(destinationX) * bytesPerPixel,
                                    decoded.constData()
                                        + static_cast<qint64>(row) * chunkWidth * bytesPerPixel,
                                    static_cast<size_t>(copyWidth) * bytesPerPixel);
                    }
                }
            }

            ImageFrame frame;
            frame.cameraId = cameraId;
            frame.width = record.width;
            frame.height = record.height;
            frame.stride = static_cast<int>(stride);
            frame.pixelFormat = record.pixelFormat;
            frame.bitsPerSample = ImageFrame::normalizedBitsPerSample(record.pixelFormat,
                                                                      record.bitsPerSample);
            frame.frameIndex = record.frameIndex;
            frame.timestampNs = record.timestampNs;
            frame.sourceRoiX = record.sourceRoiX;
            frame.sourceRoiY = record.sourceRoiY;
            frame.sourceRoiWidth = record.sourceRoiWidth;
            frame.sourceRoiHeight = record.sourceRoiHeight;
            frame.bytes = std::move(bytes);
            return frame.isValid() ? frame : ImageFrame{};
        }
    }

    ImageFrame readOmeZarrFrame(const QString& rootPath,
                                const QString& cameraId,
                                int frameIndex,
                                const ExperimentDocument& document)
    {
        if (rootPath.trimmed().isEmpty() || frameIndex < 0)
        {
            return {};
        }
        int storedIndex = 0;
        const AcquisitionEventRecord* selectedEvent = nullptr;
        const FrameRecord* selectedFrame = nullptr;
        for (const AcquisitionEventRecord& record : document.events)
        {
            const auto frameIt = record.frames.constFind(cameraId);
            if (!record.succeeded || frameIt == record.frames.constEnd())
            {
                continue;
            }
            if (storedIndex++ == frameIndex)
            {
                selectedEvent = &record;
                selectedFrame = &frameIt.value();
                break;
            }
        }
        if (!selectedEvent || !selectedFrame)
        {
            return {};
        }

        const int positionIndex = selectedEvent->event.positionIndex;
        const int z = selectedEvent->event.zIndex;
        const qint64 t = timeIndexForEvent(selectedEvent->event, document.plan);
        const bool multiPosition = document.plan.positions.size() > 1;
        if (t < 0
            || z < 0
            || positionIndex < 0
            || (multiPosition
                && positionIndex >= static_cast<int>(document.plan.positions.size()))
            || (!multiPosition && positionIndex != 0))
        {
            return {};
        }
        return readPlane(datasetPath(rootPath, positionIndex, multiPosition),
                         cameraId,
                         *selectedFrame,
                         t,
                         z,
                         document.plan.enableCompression);
    }
}
