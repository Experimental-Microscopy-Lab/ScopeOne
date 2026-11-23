#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <functional>

#include "scopeone/ConfigSetting.h"
#include "scopeone/scopeone_core_export.h"

namespace scopeone::core
{
    // A cfg file as an ordered list of lines. A line keeps its text, including its line ending and
    // trailing spaces, until it is edited, so a document that is not edited is written back byte for
    // byte. The Micro-Manager commands Device, Property, Parent, Label, ConfigGroup, ConfigPixelSize
    // and PixelSize_um and the ScopeOne commands ScopeOneDevice, ScopeOneProperty, ScopeOneConfigGroup,
    // ScopeOneLightSource and ScopeOneLimit are split into fields. Comments, blank lines and the other
    // commands stay as they are. A new line goes after the last line of its command, or to the end of
    // the file.
    // The roles are the lines of Micro-Manager's pseudo device Core. A preset setting of a ScopeOne
    // device is a ScopeOneConfigGroup line, any other a ConfigGroup line.
    class SCOPEONE_CORE_EXPORT HardwareConfiguration
    {
    public:
        struct Preset
        {
            QString name;
            QList<ConfigSetting> settings;
        };

        struct Group
        {
            QString name;
            QList<Preset> presets;
        };

        // A ScopeOneDevice line with the ScopeOneProperty lines of its label
        struct ScopeOneDevice
        {
            QString label;
            QString driverId;
            QString deviceName;
            QList<QPair<QString, QString>> properties;
            bool operator==(const ScopeOneDevice&) const = default;
        };

        // One light source channel of the ScopeOneLightSource lines of a name. Each role names its own
        // device, an empty property means the role is not declared.
        struct LightSource
        {
            QString name;
            QString enableDevice;
            QString enableProperty;
            QString onValue;
            QString offValue;
            QString powerDevice;
            QString powerProperty;
            QString readingDevice;
            QString readingProperty;
        };

        // The range of a ScopeOneLimit line that Core keeps a device property in. The axes of a stage
        // are the properties X, Y and Z in µm, and the exposure of a camera is Exposure in ms.
        struct Limit
        {
            QString device;
            QString property;
            double min{0.0};
            double max{0.0};
        };

        // The device whose properties are the roles: Camera, XYStage, Focus, Shutter, AutoFocus and
        // AutoShutter
        static QString coreDevice();
        // A value a cfg line can hold: no surrounding spaces, line breaks or NUL, and no comma unless
        // the line allows it
        static bool isValidField(const QString& value, bool allowComma = false);
        // Reads a file. A file that cannot be read gives an empty document and the reason.
        static HardwareConfiguration read(const QString& path, QString* errorMessage);
        static HardwareConfiguration fromByteArray(const QByteArray& bytes);
        bool write(const QString& path, QString* errorMessage) const;
        QByteArray toByteArray() const;

        // The labels of the devices of Micro-Manager, which are edited in Micro-Manager
        QStringList microManagerDeviceLabels() const;
        // The device of a role, or the value of AutoShutter, as the hardware takes it at the start
        QString role(const QString& role) const;
        // Whether a device label or role value names a ScopeOne device or one of its subdevices
        bool namesScopeOneDevice(const QString& value) const;
        // The ScopeOne devices, a second device of a label is left out
        QList<ScopeOneDevice> scopeOneDevices() const;
        // The groups in the order of their first line, each preset with the settings of both commands
        QList<Group> configGroups() const;
        // The channels in the order of their first line
        QList<LightSource> lightSources() const;
        // The limits in the order of their lines, the last line of a device property wins
        QList<Limit> limits() const;
        // The ScopeOne lines that name no device to start, each as "label (reason)"
        QStringList rejectedLines() const;

        void addScopeOneDevice(const ScopeOneDevice& device);
        // Removes the device with its ScopeOneProperty, ScopeOneConfigGroup, ScopeOneLightSource and
        // ScopeOneLimit lines
        void removeScopeOneDevice(const QString& label);
        void setScopeOneProperty(const QString& label, const QString& name, const QString& value);

        void addConfigGroup(const QString& name);
        void removeConfigGroup(const QString& name);
        void renameConfigGroup(const QString& name, const QString& newName);
        // Replaces the settings of a preset, or adds the preset
        void setPreset(const QString& group, const QString& preset, const QList<ConfigSetting>& settings);
        void removePreset(const QString& group, const QString& preset);
        void renamePreset(const QString& group, const QString& preset, const QString& newName);

        // Replaces the lines of the channel, or adds the channel
        void setLightSource(const LightSource& source);
        void removeLightSource(const QString& name);
        // Replaces the line of the device property, or adds it
        void setLimit(const Limit& limit);
        void removeLimit(const QString& device, const QString& property);
        // Sets the role as the hardware takes it at the start, an empty value removes its lines
        void setRole(const QString& role, const QString& value);

        // The Property lines, including the ones of Core
        QList<ConfigSetting> propertyLines() const;
        // Removes the role lines that name ScopeOne devices, which MMCore cannot resolve, and
        // returns whether there were any
        bool removeScopeOneRoles();
        // Renames a property of a device in the Property and ConfigGroup lines, for a provider that
        // names the property otherwise
        void renameMicroManagerProperty(const QString& device, const QString& name, const QString& newName);

    private:
        enum class Kind
        {
            Other,
            Device,
            Property,
            Parent,
            Label,
            ConfigGroup,
            ConfigPixelSize,
            PixelSizeUm,
            ScopeOneDevice,
            ScopeOneProperty,
            ScopeOneConfigGroup,
            ScopeOneLightSource,
            ScopeOneLimit,
            // A ScopeOneLightSource line of no valid form, its only field is the name for the message
            MalformedLightSource
        };

        struct Line
        {
            QByteArray text;
            QByteArray ending;
            Kind kind{Kind::Other};
            QStringList fields;
            // The bytes are not valid UTF-8 and the fields were decoded as Latin-1
            bool latin1{false};
        };

        // A ConfigGroup line of Micro-Manager or of ScopeOne
        static bool isGroupLine(const Line& line);

        static Line parseLine(const QByteArray& text, const QByteArray& ending);
        static QByteArray encode(const QString& text, bool latin1);

        qsizetype lastLine(const std::function<bool(const Line&)>& match) const;
        // Removes the lines that match and returns the place of the first one, or -1
        qsizetype removeLines(const std::function<bool(const Line&)>& match);
        void insertLine(qsizetype at, const QStringList& fields);
        qsizetype insertAfterLast(Kind kind, const QStringList& fields);
        void replaceFields(qsizetype index, const QStringList& fields);
        QByteArray documentEnding() const;

        QList<Line> m_lines;
    };
}
