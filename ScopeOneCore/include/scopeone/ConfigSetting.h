#pragma once

#include <QString>

namespace scopeone::core
{
    // One device property that a configuration preset sets
    struct ConfigSetting
    {
        QString device;
        QString property;
        QString value;
    };
}
