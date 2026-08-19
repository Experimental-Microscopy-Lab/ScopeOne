#pragma once

#include <QString>
#include <QStringList>

#include "scopeone/scopeone_core_export.h"

namespace scopeone::core
{
    class SCOPEONE_CORE_EXPORT DevicePropertyProvider
    {
    public:
        virtual ~DevicePropertyProvider();

        virtual QStringList listProperties(const QString& deviceId) = 0;
        virtual QString getProperty(const QString& deviceId,
                                    const QString& name,
                                    bool fromCache = false) = 0;
        virtual bool setProperty(const QString& deviceId,
                                 const QString& name,
                                 const QString& value,
                                 QString* errorMessage = nullptr) = 0;
        virtual QString getPropertyType(const QString& deviceId, const QString& name) = 0;
        virtual bool isPropertyReadOnly(const QString& deviceId, const QString& name) = 0;
        virtual bool isPropertyPreInit(const QString& deviceId, const QString& name) = 0;
        virtual QStringList getAllowedPropertyValues(const QString& deviceId,
                                                     const QString& name) = 0;
        virtual bool hasPropertyLimits(const QString& deviceId, const QString& name) = 0;
        virtual double getPropertyLowerLimit(const QString& deviceId, const QString& name) = 0;
        virtual double getPropertyUpperLimit(const QString& deviceId, const QString& name) = 0;
    };

    class SCOPEONE_CORE_EXPORT StageProvider
    {
    public:
        virtual ~StageProvider();

        virtual QString defaultXYStage() const = 0;
        virtual QString defaultZStage() const = 0;
        virtual bool getXYPosition(const QString& deviceId,
                                   double& x,
                                   double& y,
                                   QString* errorMessage = nullptr) const = 0;
        virtual bool getZPosition(const QString& deviceId,
                                  double& z,
                                  QString* errorMessage = nullptr) const = 0;
        virtual bool setRelativeXYPosition(const QString& deviceId,
                                           double dx,
                                           double dy,
                                           QString* errorMessage = nullptr) = 0;
        virtual bool setRelativeZPosition(const QString& deviceId,
                                          double dz,
                                          QString* errorMessage = nullptr) = 0;
        virtual bool setXYPosition(const QString& deviceId,
                                   double x,
                                   double y,
                                   QString* errorMessage = nullptr) = 0;
        virtual bool setZPosition(const QString& deviceId,
                                  double z,
                                  QString* errorMessage = nullptr) = 0;
    };

    class SCOPEONE_CORE_EXPORT ShutterProvider
    {
    public:
        virtual ~ShutterProvider();

        virtual bool isShutterOpen(const QString& deviceId,
                                   bool& open,
                                   QString* errorMessage = nullptr) const = 0;
        virtual bool setShutterOpen(const QString& deviceId,
                                    bool open,
                                    QString* errorMessage = nullptr) = 0;
    };

    class SCOPEONE_CORE_EXPORT StateProvider
    {
    public:
        virtual ~StateProvider();

        virtual bool getState(const QString& deviceId,
                              long& state,
                              QString* errorMessage = nullptr) const = 0;
        virtual bool setState(const QString& deviceId,
                              long state,
                              QString* errorMessage = nullptr) = 0;
        virtual QString stateLabel(const QString& deviceId, long state) const = 0;
    };

    class SCOPEONE_CORE_EXPORT ConfigurationProvider
    {
    public:
        virtual ~ConfigurationProvider();

        virtual QStringList availableConfigGroups() const = 0;
        virtual QStringList availableConfigs(const QString& groupName) const = 0;
        virtual QString currentConfig(const QString& groupName) const = 0;
        virtual bool setConfig(const QString& groupName,
                               const QString& configName,
                               QString* errorMessage = nullptr) = 0;
    };
}
