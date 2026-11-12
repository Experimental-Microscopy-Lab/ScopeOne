#pragma once

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "MMCore.h"
#include "scopeone/HardwareTypes.h"

// Helpers for the code that calls MMCore, MicroManagerProvider and MicroManagerCamera
namespace scopeone::core::internal::micromanager
{
    inline HardwareProviderDescriptor descriptor()
    {
        return {QStringLiteral("micro-manager"), QStringLiteral("Micro-Manager"), QStringLiteral("MMCore")};
    }

    // MMCore takes device and property names as trimmed narrow strings
    inline std::string name8(const QString& name)
    {
        return name.trimmed().toStdString();
    }

    inline QStringList toQStringList(const std::vector<std::string>& values)
    {
        QStringList result;
        result.reserve(static_cast<qsizetype>(values.size()));
        for (const std::string& value : values)
        {
            result.append(QString::fromStdString(value));
        }
        return result;
    }

    // Runs an MMCore query and returns the fallback when the core reports an error
    template <class T, class Call>
    T orElse(T fallback, Call&& call)
    {
        try
        {
            return call();
        }
        catch (const CMMError&)
        {
            return fallback;
        }
    }

    // Runs an MMCore command and reports a core error through errorMessage
    template <class Call>
    bool succeeds(QString* errorMessage, Call&& call)
    {
        if (errorMessage)
        {
            errorMessage->clear();
        }
        try
        {
            call();
            return true;
        }
        catch (const CMMError& error)
        {
            if (errorMessage)
            {
                *errorMessage = QString::fromStdString(error.getMsg());
            }
            return false;
        }
    }

    inline QString propertyType(CMMCore& core, const QString& device, const QString& property)
    {
        return orElse(QStringLiteral("Unknown"), [&]
        {
            switch (core.getPropertyType(name8(device).c_str(), name8(property).c_str()))
            {
            case MM::String: return QStringLiteral("String");
            case MM::Float: return QStringLiteral("Float");
            case MM::Integer: return QStringLiteral("Integer");
            default: return QStringLiteral("Unknown");
            }
        });
    }

    inline long propertySequenceCapacity(CMMCore& core, const QString& device, const QString& property)
    {
        return orElse(0L, [&]
        {
            const std::string label = name8(device);
            const std::string name = name8(property);
            return core.isPropertySequenceable(label.c_str(), name.c_str())
                ? core.getPropertySequenceMaxLength(label.c_str(), name.c_str())
                : 0L;
        });
    }

    inline bool startPropertySequence(CMMCore& core,
                                      const QString& device,
                                      const QString& property,
                                      const QStringList& values,
                                      QString* errorMessage)
    {
        return succeeds(errorMessage, [&]
        {
            const std::string label = name8(device);
            const std::string name = name8(property);
            std::vector<std::string> sequence;
            sequence.reserve(static_cast<std::size_t>(values.size()));
            for (const QString& value : values)
            {
                sequence.push_back(value.toStdString());
            }
            core.loadPropertySequence(label.c_str(), name.c_str(), sequence);
            core.startPropertySequence(label.c_str(), name.c_str());
        });
    }

    inline void stopPropertySequence(CMMCore& core, const QString& device, const QString& property)
    {
        succeeds(nullptr, [&] { core.stopPropertySequence(name8(device).c_str(), name8(property).c_str()); });
    }
}
