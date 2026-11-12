#pragma once

#include <QByteArray>
#include <QSharedMemory>
#include <QString>
#include <QtGlobal>

#include <cstddef>

// A ring of records that one writer fills and one reader drains, in shared memory between
// DriverHost and ScopeOne or in the memory of one process. Unlike the camera frame ring it
// never overwrites a record the reader has not read: when the ring is full the writer drops
// the new record and the next record it writes carries the number it dropped, so every gap
// is reported. A record is stored in one piece; one that would cross the end of the ring
// starts again at its beginning after a skip record.
namespace scopeone::core::internal::streamring
{
    enum class RecordType : quint32
    {
        Skip = 0,
        TimeSeries = 1,
        TimestampedEvents = 2,
        DaqInput = 3
    };

    struct Record
    {
        RecordType type{RecordType::Skip};
        quint64 sequence{0};
        // Records the writer dropped right before this one because the ring was full
        quint64 droppedBefore{0};
        QByteArray payload;
    };

    // Bytes a ring of the given capacity takes, the control block included
    qsizetype ringBytes(qsizetype capacity);

    // Lays out an empty ring in memory of ringBytes(capacity) bytes
    void initializeRing(void* memory, qsizetype capacity);

    // Creates the shared memory of a ring of the given capacity under the native key, dropping
    // a ring of that key a crashed DriverHost left behind
    bool createSharedRing(QSharedMemory& shm,
                          const QNativeIpcKey& nativeKey,
                          qsizetype capacity,
                          QString* errorMessage);

    // Attaches the shared memory of a ring another process created
    bool attachSharedRing(QSharedMemory& shm, const QNativeIpcKey& nativeKey, QString* errorMessage);

    class Writer
    {
    public:
        Writer() = default;
        explicit Writer(void* memory);

        bool isValid() const { return m_base != nullptr; }

        // Appends one record. False when the ring has no room, and the record is counted as dropped.
        bool write(RecordType type, const void* payload, std::size_t bytes);

    private:
        uchar* m_base{nullptr};
        quint64 m_sequence{0};
        quint64 m_dropped{0};
    };

    class Reader
    {
    public:
        Reader() = default;
        explicit Reader(void* memory);

        bool isValid() const { return m_base != nullptr; }

        // Takes the oldest unread record. False when the ring is empty or holds a record that
        // does not fit the ring, which only a writer of another layout leaves.
        bool read(Record& record);

    private:
        uchar* m_base{nullptr};
    };
}
