#include "internal/StreamRing.h"

#include <atomic>
#include <cstring>

namespace scopeone::core::internal::streamring
{
    namespace
    {
        constexpr quint32 kMagic = 0x53524E47; // "SRNG"
        constexpr quint32 kLayoutVersion = 1;

        // The cursors count bytes from the start of the stream and never wrap, so the bytes
        // in use are writeCursor - readCursor. Each sits on a cache line of its own because
        // the two processes write them.
        struct alignas(64) Control
        {
            quint32 magic;
            quint32 version;
            quint64 capacity;
            alignas(64) quint64 writeCursor;
            alignas(64) quint64 readCursor;
        };

        struct RecordHeader
        {
            quint32 bytes;
            quint32 type;
            quint64 sequence;
            quint64 droppedBefore;
            quint64 payloadBytes;
        };

        // Every record starts on 8 bytes, so the first two words of any record, which hold its
        // size and type, always fit before the end of the ring
        constexpr quint64 kAlignment = 8;
        static_assert(sizeof(RecordHeader) % kAlignment == 0);
        static_assert(std::atomic_ref<quint64>::is_always_lock_free,
                      "The stream ring requires lock-free 64-bit atomics");

        quint64 aligned(quint64 bytes)
        {
            return (bytes + kAlignment - 1) / kAlignment * kAlignment;
        }

        Control* control(uchar* base)
        {
            return reinterpret_cast<Control*>(base);
        }

        uchar* data(uchar* base)
        {
            return base + sizeof(Control);
        }
    }

    qsizetype ringBytes(qsizetype capacity)
    {
        return static_cast<qsizetype>(sizeof(Control) + aligned(static_cast<quint64>(capacity)));
    }

    void initializeRing(void* memory, qsizetype capacity)
    {
        Control initial{};
        initial.magic = kMagic;
        initial.version = kLayoutVersion;
        initial.capacity = aligned(static_cast<quint64>(capacity));
        std::memcpy(memory, &initial, sizeof(initial));
    }

    bool createSharedRing(QSharedMemory& shm,
                          const QNativeIpcKey& nativeKey,
                          qsizetype capacity,
                          QString* errorMessage)
    {
        shm.setNativeKey(nativeKey);
        const qsizetype bytes = ringBytes(capacity);
        if (!shm.create(bytes))
        {
            if (shm.attach())
            {
                shm.detach();
            }
            if (!shm.create(bytes))
            {
                if (errorMessage)
                {
                    *errorMessage = QStringLiteral("Cannot create shared memory '%1': %2")
                                        .arg(nativeKey.nativeKey(), shm.errorString());
                }
                return false;
            }
        }
        initializeRing(shm.data(), capacity);
        return true;
    }

    bool attachSharedRing(QSharedMemory& shm, const QNativeIpcKey& nativeKey, QString* errorMessage)
    {
        shm.setNativeKey(nativeKey);
        if (!shm.attach(QSharedMemory::ReadWrite))
        {
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Cannot attach shared memory '%1': %2")
                                    .arg(nativeKey.nativeKey(), shm.errorString());
            }
            return false;
        }
        const Control* const ring = control(static_cast<uchar*>(shm.data()));
        if (shm.size() < static_cast<qsizetype>(sizeof(Control))
            || ring->magic != kMagic || ring->version != kLayoutVersion
            || shm.size() < ringBytes(static_cast<qsizetype>(ring->capacity)))
        {
            shm.detach();
            if (errorMessage)
            {
                *errorMessage = QStringLiteral("Shared memory '%1' is not a stream ring").arg(nativeKey.nativeKey());
            }
            return false;
        }
        return true;
    }

    Writer::Writer(void* memory)
        : m_base(static_cast<uchar*>(memory))
    {
    }

    bool Writer::write(RecordType type, const void* payload, std::size_t bytes)
    {
        Control* const ring = control(m_base);
        const quint64 capacity = ring->capacity;
        const quint64 need = aligned(sizeof(RecordHeader) + bytes);
        const quint64 write = std::atomic_ref<quint64>(ring->writeCursor).load(std::memory_order_relaxed);
        const quint64 read = std::atomic_ref<quint64>(ring->readCursor).load(std::memory_order_acquire);
        const quint64 offset = write % capacity;
        const quint64 tail = capacity - offset;
        const quint64 skip = tail < need ? tail : 0;
        if (need > capacity / 2 || write + skip + need - read > capacity)
        {
            ++m_dropped;
            return false;
        }

        uchar* const area = data(m_base);
        if (skip > 0)
        {
            const quint32 skipHeader[2]{static_cast<quint32>(skip), static_cast<quint32>(RecordType::Skip)};
            std::memcpy(area + offset, skipHeader, sizeof(skipHeader));
        }
        const quint64 start = (write + skip) % capacity;
        const RecordHeader header{static_cast<quint32>(need), static_cast<quint32>(type), m_sequence++, m_dropped,
                                  static_cast<quint64>(bytes)};
        std::memcpy(area + start, &header, sizeof(header));
        if (bytes > 0)
        {
            std::memcpy(area + start + sizeof(header), payload, bytes);
        }
        m_dropped = 0;
        std::atomic_ref<quint64>(ring->writeCursor).store(write + skip + need, std::memory_order_release);
        return true;
    }

    Reader::Reader(void* memory)
        : m_base(static_cast<uchar*>(memory))
    {
    }

    bool Reader::read(Record& record)
    {
        Control* const ring = control(m_base);
        const quint64 capacity = ring->capacity;
        quint64 read = std::atomic_ref<quint64>(ring->readCursor).load(std::memory_order_relaxed);
        const quint64 write = std::atomic_ref<quint64>(ring->writeCursor).load(std::memory_order_acquire);
        const uchar* const area = data(m_base);
        while (read < write)
        {
            const quint64 offset = read % capacity;
            quint32 words[2]{};
            std::memcpy(words, area + offset, sizeof(words));
            const quint64 bytes = words[0];
            if (bytes < kAlignment || bytes % kAlignment != 0 || bytes > write - read
                || offset + bytes > capacity)
            {
                return false;
            }
            if (static_cast<RecordType>(words[1]) == RecordType::Skip)
            {
                read += bytes;
                continue;
            }
            RecordHeader header{};
            std::memcpy(&header, area + offset, sizeof(header));
            if (header.payloadBytes > bytes - sizeof(header))
            {
                return false;
            }
            record.type = static_cast<RecordType>(header.type);
            record.sequence = header.sequence;
            record.droppedBefore = header.droppedBefore;
            record.payload = QByteArray(reinterpret_cast<const char*>(area + offset + sizeof(header)),
                                        static_cast<qsizetype>(header.payloadBytes));
            std::atomic_ref<quint64>(ring->readCursor).store(read + bytes, std::memory_order_release);
            return true;
        }
        std::atomic_ref<quint64>(ring->readCursor).store(read, std::memory_order_release);
        return false;
    }
}
