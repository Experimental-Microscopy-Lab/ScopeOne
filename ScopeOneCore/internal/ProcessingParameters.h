#pragma once

#include "scopeone/ProcessingTypes.h"

#include <initializer_list>

namespace scopeone::core::internal
{
    // Builders for the parameter descriptors of a module, shared by the built-in and the CUDA
    // modules so that every descriptor is written the same way.
    inline ProcessingParameterDescriptor integerParameter(const char* key,
                                                          const char* name,
                                                          int value,
                                                          int minimum,
                                                          int maximum,
                                                          int step = 1)
    {
        return {QString::fromLatin1(key),
                QString::fromLatin1(name),
                ProcessingParameterType::Integer,
                value,
                minimum,
                maximum,
                step};
    }

    inline ProcessingParameterDescriptor realParameter(const char* key,
                                                       const char* name,
                                                       double value,
                                                       double minimum,
                                                       double maximum,
                                                       double step,
                                                       int decimals)
    {
        ProcessingParameterDescriptor descriptor{
            QString::fromLatin1(key),
            QString::fromLatin1(name),
            ProcessingParameterType::Real,
            value,
            minimum,
            maximum,
            step};
        descriptor.decimals = decimals;
        return descriptor;
    }

    inline ProcessingParameterDescriptor booleanParameter(const char* key,
                                                          const char* name,
                                                          bool value)
    {
        return {QString::fromLatin1(key),
                QString::fromLatin1(name),
                ProcessingParameterType::Boolean,
                value};
    }

    // The choices take the values 0, 1, 2 ... in the order given
    inline ProcessingParameterDescriptor choiceParameter(const char* key,
                                                         const char* name,
                                                         int value,
                                                         std::initializer_list<const char*> choices)
    {
        ProcessingParameterDescriptor descriptor{
            QString::fromLatin1(key),
            QString::fromLatin1(name),
            ProcessingParameterType::Choice,
            value};
        int index = 0;
        for (const char* choice : choices)
        {
            descriptor.choices.append({QString::fromLatin1(choice), index++});
        }
        return descriptor;
    }

    // The parameter is shown only while the choice parameter key has one of the values
    inline ProcessingParameterDescriptor shownWhen(ProcessingParameterDescriptor descriptor,
                                                   const char* key,
                                                   std::initializer_list<int> values)
    {
        descriptor.shownWithKey = QString::fromLatin1(key);
        descriptor.shownWithValues = QList<int>(values);
        return descriptor;
    }
}
