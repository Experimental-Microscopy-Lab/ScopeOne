// The cfg file as a document of lines
#include "scopeone/HardwareConfiguration.h"

#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>

#include <algorithm>

namespace scopeone::core
{
    namespace
    {
        // Splits one line into fieldCount fields, the last of which keeps its commas. A line with
        // fewer commas gives an empty list.
        QStringList splitFields(const QString& line, int fieldCount)
        {
            QStringList parts;
            int start = 0;
            for (int i = 1; i < fieldCount; ++i)
            {
                const int comma = line.indexOf(QLatin1Char(','), start);
                if (comma < 0)
                {
                    return {};
                }
                parts.append(line.mid(start, comma - start).trimmed());
                start = comma + 1;
            }
            parts.append(line.mid(start).trimmed());
            return parts;
        }

        // Micro-Manager cuts a line at every comma and drops the empty tokens (CDeviceUtils::Tokenize),
        // so "Device,L,lib,name," has 4 fields and "Property,Dev,,V" is the property V with no value.
        QStringList mmTokens(const QString& line)
        {
            QStringList tokens;
            for (const QString& token : line.split(QLatin1Char(','), Qt::SkipEmptyParts))
            {
                tokens.append(token.trimmed());
            }
            return tokens;
        }

        // The number of tokens a Micro-Manager command takes in MMCore's loader. Property may leave
        // its value out and ConfigGroup may leave its value out or give the group only. 0 means the
        // line is not of the form of the command.
        int mmFieldCount(const QString& command, int tokens)
        {
            if (command == QStringLiteral("Device") || command == QStringLiteral("Label"))
            {
                return tokens == 4 ? 4 : 0;
            }
            if (command == QStringLiteral("Property"))
            {
                return tokens == 4 || tokens == 3 ? tokens : 0;
            }
            if (command == QStringLiteral("Parent") || command == QStringLiteral("PixelSize_um"))
            {
                return tokens == 3 ? 3 : 0;
            }
            if (command == QStringLiteral("ConfigPixelSize"))
            {
                return tokens == 5 ? 5 : 0;
            }
            if (command == QStringLiteral("ConfigGroup"))
            {
                return tokens == 6 || tokens == 5 || tokens == 2 ? tokens : 0;
            }
            return 0;
        }

        // The number of fields of a ScopeOne command, the last of which keeps its commas
        int scopeOneFieldCount(const QString& command, int commas)
        {
            if (command == QStringLiteral("ScopeOneDevice") || command == QStringLiteral("ScopeOneProperty"))
            {
                return commas >= 3 ? 4 : 0;
            }
            if (command == QStringLiteral("ScopeOneConfigGroup"))
            {
                return commas >= 5 ? 6 : 0;
            }
            return 0;
        }

        // Valid UTF-8 is decoded as UTF-8, any other bytes as Latin-1, so a line keeps its bytes
        // when it is written again. Rebuilding an edited line from UTF-8 turned the bytes of a
        // Latin-1 file, such as the micro sign of a unit, into U+FFFD.
        QString decode(const QByteArray& bytes, bool* latin1)
        {
            QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
            const QString text = decoder.decode(bytes);
            *latin1 = decoder.hasError();
            return *latin1 ? QString::fromLatin1(bytes) : text;
        }
    }

    bool HardwareConfiguration::isGroupLine(const Line& line)
    {
        return line.kind == Kind::ConfigGroup || line.kind == Kind::ScopeOneConfigGroup;
    }

    bool HardwareConfiguration::isValidField(const QString& value, bool allowComma)
    {
        return value == value.trimmed() && !value.contains(QLatin1Char('\n'))
            && !value.contains(QLatin1Char('\r')) && !value.contains(QChar(0))
            && (allowComma || !value.contains(QLatin1Char(',')));
    }

    HardwareConfiguration HardwareConfiguration::read(const QString& path, QString* errorMessage)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            if (errorMessage)
            {
                *errorMessage = file.errorString();
            }
            return {};
        }
        return fromByteArray(file.readAll());
    }

    HardwareConfiguration HardwareConfiguration::fromByteArray(const QByteArray& bytes)
    {
        HardwareConfiguration document;
        qsizetype start = 0;
        while (start < bytes.size())
        {
            const qsizetype newline = bytes.indexOf('\n', start);
            const qsizetype end = newline < 0 ? bytes.size() : newline;
            const bool carriage = newline >= 0 && end > start && bytes[end - 1] == '\r';
            const QByteArray ending = newline < 0 ? QByteArray{} : carriage ? QByteArray("\r\n") : QByteArray("\n");
            document.m_lines.append(parseLine(bytes.mid(start, end - start - (carriage ? 1 : 0)), ending));
            start = end + 1;
        }
        return document;
    }

    QByteArray HardwareConfiguration::encode(const QString& text, bool latin1)
    {
        return latin1 ? text.toLatin1() : text.toUtf8();
    }

    HardwareConfiguration::Line HardwareConfiguration::parseLine(const QByteArray& bytes, const QByteArray& ending)
    {
        Line line;
        line.text = bytes;
        line.ending = ending;
        const QString text = decode(bytes, &line.latin1).trimmed();
        const int commas = static_cast<int>(text.count(QLatin1Char(',')));
        const QString command = text.left(text.indexOf(QLatin1Char(','))).trimmed();
        if (text.startsWith(QStringLiteral("ScopeOneLightSource")))
        {
            // splitFields keeps the rest of the line in its last field, so the role is taken from
            // a split with one field more than the role's own and the fields are split again by
            // role. Splitting at 4 fields left "Enable,Laser,On,Off" in the role and rejected every
            // line.
            const QStringList head = splitFields(text, 5);
            const QString role = head.size() == 5 ? head[3] : QString{};
            const QStringList enableFields = splitFields(text, 7);
            if (role == QStringLiteral("Enable") && enableFields.size() == 7)
            {
                line.kind = Kind::ScopeOneLightSource;
                line.fields = enableFields;
            }
            else if ((role == QStringLiteral("Power") || role == QStringLiteral("Reading"))
                     && !head[4].contains(QLatin1Char(',')))
            {
                line.kind = Kind::ScopeOneLightSource;
                line.fields = head;
            }
            else
            {
                line.kind = Kind::MalformedLightSource;
                line.fields = {head.size() == 5 ? head[1] : text};
            }
            return line;
        }
        static const QList<QPair<QString, Kind>> kinds{
            {QStringLiteral("Device"), Kind::Device},
            {QStringLiteral("Property"), Kind::Property},
            {QStringLiteral("Parent"), Kind::Parent},
            {QStringLiteral("Label"), Kind::Label},
            {QStringLiteral("ConfigGroup"), Kind::ConfigGroup},
            {QStringLiteral("ConfigPixelSize"), Kind::ConfigPixelSize},
            {QStringLiteral("PixelSize_um"), Kind::PixelSizeUm},
            {QStringLiteral("ScopeOneDevice"), Kind::ScopeOneDevice},
            {QStringLiteral("ScopeOneProperty"), Kind::ScopeOneProperty},
            {QStringLiteral("ScopeOneConfigGroup"), Kind::ScopeOneConfigGroup}};
        const auto kind = std::find_if(kinds.cbegin(), kinds.cend(),
                                       [&](const auto& entry) { return entry.first == command; });
        if (kind == kinds.cend())
        {
            return line;
        }
        QStringList fields;
        if (kind->second == Kind::ScopeOneDevice || kind->second == Kind::ScopeOneProperty
            || kind->second == Kind::ScopeOneConfigGroup)
        {
            const int count = scopeOneFieldCount(command, commas);
            if (count > 0)
            {
                fields = splitFields(text, count);
            }
        }
        else
        {
            fields = mmTokens(text);
            const int count = mmFieldCount(command, static_cast<int>(fields.size()));
            if (count == 0)
            {
                return line;
            }
            // A Property or ConfigGroup line without its value sets an empty value
            if ((kind->second == Kind::Property && count == 3) || (kind->second == Kind::ConfigGroup && count == 5))
            {
                fields.append(QString{});
            }
        }
        if (!fields.isEmpty())
        {
            line.kind = kind->second;
            line.fields = fields;
        }
        return line;
    }

    QByteArray HardwareConfiguration::toByteArray() const
    {
        QByteArray bytes;
        for (const Line& line : m_lines)
        {
            bytes += line.text;
            bytes += line.ending;
        }
        return bytes;
    }

    bool HardwareConfiguration::write(const QString& path, QString* errorMessage) const
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(toByteArray()) < 0 || !file.commit())
        {
            if (errorMessage)
            {
                *errorMessage = file.errorString();
            }
            return false;
        }
        return true;
    }

    QStringList HardwareConfiguration::microManagerDeviceLabels() const
    {
        QStringList labels;
        for (const Line& line : m_lines)
        {
            if (line.kind == Kind::Device)
            {
                labels.append(line.fields[1]);
            }
        }
        return labels;
    }

    QList<ConfigSetting> HardwareConfiguration::propertyLines() const
    {
        QList<ConfigSetting> properties;
        for (const Line& line : m_lines)
        {
            if (line.kind == Kind::Property)
            {
                properties.append({line.fields[1], line.fields[2], line.fields[3]});
            }
        }
        return properties;
    }

    QString HardwareConfiguration::coreRole(const QString& role) const
    {
        // System/Startup is applied after Property lines, even when it appears earlier in the file.
        const qsizetype startup = lastLine([&](const Line& line)
        {
            return line.kind == Kind::ConfigGroup && line.fields.size() >= 6
                   && line.fields[1] == QStringLiteral("System") && line.fields[2] == QStringLiteral("Startup")
                   && line.fields[3] == QStringLiteral("Core") && line.fields[4] == role;
        });
        if (startup >= 0) return m_lines[startup].fields[5];
        const qsizetype index = lastLine([&](const Line& line)
        {
            return line.kind == Kind::Property && line.fields[1] == QStringLiteral("Core") && line.fields[2] == role;
        });
        return index < 0 ? QString{} : m_lines[index].fields[3];
    }

    bool HardwareConfiguration::namesScopeOneDevice(const QString& value) const
    {
        if (value.isEmpty() || microManagerDeviceLabels().contains(value)) return false;
        for (const auto& device : scopeOneDevices())
            if (value == device.label || value.startsWith(device.label + QLatin1Char('.'))) return true;
        return false;
    }

    bool HardwareConfiguration::removeScopeOneCoreRoles()
    {
        bool removed = false;
        // MMCore cannot resolve ScopeOne role values before those devices are registered.
        removeLines([&](const Line& line)
        {
            QString role, value;
            if (line.kind == Kind::Property && line.fields[1] == QStringLiteral("Core"))
            {
                role = line.fields[2]; value = line.fields[3];
            }
            else if (line.kind == Kind::ConfigGroup && line.fields.size() >= 6
                     && line.fields[1] == QStringLiteral("System") && line.fields[2] == QStringLiteral("Startup")
                     && line.fields[3] == QStringLiteral("Core"))
            {
                role = line.fields[4]; value = line.fields[5];
            }
            const bool match = namesScopeOneDevice(value);
            removed = removed || match;
            return match;
        });
        return removed;
    }

    QList<HardwareConfiguration::ScopeOneDevice> HardwareConfiguration::scopeOneDevices() const
    {
        QList<ScopeOneDevice> devices;
        for (const Line& line : m_lines)
        {
            if (line.kind != Kind::ScopeOneDevice)
            {
                continue;
            }
            const bool used = std::any_of(devices.cbegin(), devices.cend(),
                                          [&](const ScopeOneDevice& existing) { return existing.label == line.fields[1]; });
            if (!used)
            {
                devices.append({line.fields[1], line.fields[2], line.fields[3], {}});
            }
        }
        for (const Line& line : m_lines)
        {
            if (line.kind != Kind::ScopeOneProperty)
            {
                continue;
            }
            for (ScopeOneDevice& device : devices)
            {
                if (device.label == line.fields[1])
                {
                    device.properties.append({line.fields[2], line.fields[3]});
                    break;
                }
            }
        }
        return devices;
    }

    QList<HardwareConfiguration::Group> HardwareConfiguration::configGroups() const
    {
        QList<Group> groups;
        const auto groupOf = [&](const QString& name) -> Group&
        {
            for (Group& group : groups)
            {
                if (group.name == name)
                {
                    return group;
                }
            }
            groups.append(Group{name, {}});
            return groups.last();
        };
        for (const Line& line : m_lines)
        {
            if (!isGroupLine(line))
            {
                continue;
            }
            Group& group = groupOf(line.fields[1]);
            if (line.fields.size() == 2)
            {
                continue;
            }
            auto preset = std::find_if(group.presets.begin(), group.presets.end(),
                                       [&](const Preset& existing) { return existing.name == line.fields[2]; });
            if (preset == group.presets.end())
            {
                group.presets.append(Preset{line.fields[2], {}});
                preset = group.presets.end() - 1;
            }
            PresetSetting setting;
            setting.device = line.fields[3];
            setting.property = line.fields[4];
            setting.value = line.fields[5];
            setting.source = line.kind == Kind::ConfigGroup ? Source::MicroManager : Source::ScopeOne;
            preset->settings.append(setting);
        }
        return groups;
    }

    QList<HardwareConfiguration::LightSource> HardwareConfiguration::lightSources() const
    {
        QList<LightSource> sources;
        for (const Line& line : m_lines)
        {
            if (line.kind != Kind::ScopeOneLightSource)
            {
                continue;
            }
            const QStringList& fields = line.fields;
            auto it = std::find_if(sources.begin(), sources.end(),
                                   [&](const LightSource& source) { return source.name == fields[1]; });
            if (it == sources.end())
            {
                sources.append(LightSource{fields[1], {}, {}, {}, {}, {}, {}, {}, {}});
                it = sources.end() - 1;
            }
            if (fields[3] == QStringLiteral("Enable"))
            {
                it->enableDevice = fields[2];
                it->enableProperty = fields[4];
                it->onValue = fields[5];
                it->offValue = fields[6];
            }
            else if (fields[3] == QStringLiteral("Power"))
            {
                it->powerDevice = fields[2];
                it->powerProperty = fields[4];
            }
            else
            {
                it->readingDevice = fields[2];
                it->readingProperty = fields[4];
            }
        }
        return sources;
    }

    QStringList HardwareConfiguration::rejectedLines() const
    {
        QStringList rejected;
        QStringList labels;
        QStringList malformed;
        for (const Line& line : m_lines)
        {
            if (line.kind == Kind::ScopeOneDevice)
            {
                if (labels.contains(line.fields[1]))
                {
                    rejected.append(QStringLiteral("%1 (label used twice)").arg(line.fields[1]));
                }
                else
                {
                    labels.append(line.fields[1]);
                }
            }
            else if (line.kind == Kind::MalformedLightSource)
            {
                malformed.append(line.fields[0]);
                rejected.append(QStringLiteral("%1 (malformed ScopeOneLightSource line)").arg(line.fields[0]));
            }
        }
        for (const LightSource& source : lightSources())
        {
            // A channel that has a malformed line is reported once
            if (source.powerProperty.isEmpty() && !malformed.contains(source.name))
            {
                rejected.append(QStringLiteral("%1 (light source without Power)").arg(source.name));
            }
        }
        QStringList orphans;
        for (const Line& line : m_lines)
        {
            if (line.kind == Kind::ScopeOneProperty && !labels.contains(line.fields[1])
                && !orphans.contains(line.fields[1]))
            {
                orphans.append(line.fields[1]);
                rejected.append(QStringLiteral("%1 (no ScopeOneDevice)").arg(line.fields[1]));
            }
        }
        return rejected;
    }

    void HardwareConfiguration::addScopeOneDevice(const ScopeOneDevice& device)
    {
        insertAfterLast(Kind::ScopeOneDevice,
                        {QStringLiteral("ScopeOneDevice"), device.label, device.driverId, device.deviceName});
        for (const auto& property : device.properties)
        {
            setScopeOneProperty(device.label, property.first, property.second);
        }
    }

    void HardwareConfiguration::removeScopeOneDevice(const QString& label)
    {
        // A Core role left naming the removed device reached MMCore on the next load, which failed
        // the whole configuration on the unknown label
        const auto namesDevice = [&](const QString& value)
        { return value == label || value.startsWith(label + QLatin1Char('.')); };
        removeLines([&](const Line& line)
        {
            return (line.kind == Kind::Property && line.fields[1] == QStringLiteral("Core")
                    && namesDevice(line.fields[3]))
                   || (line.kind == Kind::ConfigGroup && line.fields.size() >= 6
                       && line.fields[1] == QStringLiteral("System") && line.fields[2] == QStringLiteral("Startup")
                       && line.fields[3] == QStringLiteral("Core") && namesDevice(line.fields[5]))
                   || (line.kind == Kind::ScopeOneDevice && line.fields[1] == label)
                   || (line.kind == Kind::ScopeOneProperty && line.fields[1] == label)
                   || (line.kind == Kind::ScopeOneConfigGroup && line.fields[3] == label)
                   || (line.kind == Kind::ScopeOneLightSource && line.fields[2] == label);
        });
    }

    void HardwareConfiguration::setScopeOneProperty(const QString& label, const QString& name, const QString& value)
    {
        const QStringList fields{QStringLiteral("ScopeOneProperty"), label, name, value};
        // MMCore and coreRole() take the last of two equal lines, so that is the one to change
        const qsizetype index = lastLine([&](const Line& line)
        {
            return line.kind == Kind::ScopeOneProperty && line.fields[1] == label && line.fields[2] == name;
        });
        if (index >= 0)
        {
            replaceFields(index, fields);
        }
        else
        {
            insertAfterLast(Kind::ScopeOneProperty, fields);
        }
    }

    void HardwareConfiguration::addConfigGroup(const QString& name)
    {
        insertAfterLast(Kind::ConfigGroup, {QStringLiteral("ConfigGroup"), name});
    }

    void HardwareConfiguration::removeConfigGroup(const QString& name)
    {
        removeLines([&](const Line& line)
        {
            return isGroupLine(line) && line.fields[1] == name;
        });
    }

    void HardwareConfiguration::renameConfigGroup(const QString& name, const QString& newName)
    {
        for (qsizetype index = 0; index < m_lines.size(); ++index)
        {
            const Line& line = m_lines[index];
            if (isGroupLine(line) && line.fields[1] == name)
            {
                QStringList fields = line.fields;
                fields[1] = newName;
                replaceFields(index, fields);
            }
        }
    }

    void HardwareConfiguration::setPreset(const QString& group, const QString& preset,
                                          const QList<PresetSetting>& settings)
    {
        const auto ofPreset = [&](const Line& line)
        {
            return isGroupLine(line)
                   && line.fields.size() == 6 && line.fields[1] == group && line.fields[2] == preset;
        };
        QList<bool> matched(settings.size(), false);
        for (qsizetype index = m_lines.size(); index-- > 0;)
        {
            if (!ofPreset(m_lines[index])) continue;
            const Line& line = m_lines[index];
            qsizetype found = -1;
            for (qsizetype i = 0; i < settings.size(); ++i)
            {
                const auto& setting = settings[i];
                const bool sameKind = (line.kind == Kind::ScopeOneConfigGroup) == (setting.source == Source::ScopeOne);
                if (!matched[i] && sameKind && line.fields[3] == setting.device && line.fields[4] == setting.property)
                { found = i; break; }
            }
            if (found < 0) { m_lines.removeAt(index); continue; }
            matched[found] = true;
            if (line.fields[5] != settings[found].value)
            {
                QStringList fields = line.fields;
                fields[5] = settings[found].value;
                // Rebuilding the whole preset moved comments between its settings.
                replaceFields(index, fields);
            }
        }
        for (qsizetype i = 0; i < settings.size(); ++i)
        {
            if (matched[i]) continue;
            const auto& setting = settings[i];
            const bool scopeOne = setting.source == Source::ScopeOne;
            const QStringList fields{scopeOne ? QStringLiteral("ScopeOneConfigGroup") : QStringLiteral("ConfigGroup"),
                                     group, preset, setting.device, setting.property, setting.value};
            insertAfterLast(scopeOne ? Kind::ScopeOneConfigGroup : Kind::ConfigGroup, fields);
        }
    }

    void HardwareConfiguration::removePreset(const QString& group, const QString& preset)
    {
        removeLines([&](const Line& line)
        {
            return isGroupLine(line)
                   && line.fields.size() == 6 && line.fields[1] == group && line.fields[2] == preset;
        });
    }

    void HardwareConfiguration::renamePreset(const QString& group, const QString& preset, const QString& newName)
    {
        for (qsizetype index = 0; index < m_lines.size(); ++index)
        {
            const Line& line = m_lines[index];
            if (isGroupLine(line)
                && line.fields.size() == 6 && line.fields[1] == group && line.fields[2] == preset)
            {
                QStringList fields = line.fields;
                fields[2] = newName;
                replaceFields(index, fields);
            }
        }
    }

    void HardwareConfiguration::setLightSource(const LightSource& source)
    {
        QList<QStringList> lines;
        const QString head = QStringLiteral("ScopeOneLightSource");
        if (!source.enableProperty.isEmpty())
        {
            lines.append({head, source.name, source.enableDevice, QStringLiteral("Enable"),
                          source.enableProperty, source.onValue, source.offValue});
        }
        if (!source.powerProperty.isEmpty())
        {
            lines.append({head, source.name, source.powerDevice, QStringLiteral("Power"), source.powerProperty});
        }
        if (!source.readingProperty.isEmpty())
        {
            lines.append({head, source.name, source.readingDevice, QStringLiteral("Reading"), source.readingProperty});
        }
        QList<bool> matched(lines.size(), false);
        for (qsizetype index = m_lines.size(); index-- > 0;)
        {
            const Line& line = m_lines[index];
            if (line.kind == Kind::MalformedLightSource && line.fields[0] == source.name)
            { m_lines.removeAt(index); continue; }
            if (line.kind != Kind::ScopeOneLightSource || line.fields[1] != source.name) continue;
            qsizetype found = -1;
            for (qsizetype i = 0; i < lines.size(); ++i)
                if (!matched[i] && line.fields[3] == lines[i][3]) { found = i; break; }
            if (found < 0) { m_lines.removeAt(index); continue; }
            matched[found] = true;
            // Replacing the whole channel moved comments between its role lines.
            if (line.fields != lines[found]) replaceFields(index, lines[found]);
        }
        for (qsizetype i = 0; i < lines.size(); ++i)
        {
            if (!matched[i])
            {
                insertAfterLast(Kind::ScopeOneLightSource, lines[i]);
            }
        }
    }

    void HardwareConfiguration::removeLightSource(const QString& name)
    {
        removeLines([&](const Line& line)
        {
            return (line.kind == Kind::ScopeOneLightSource && line.fields[1] == name)
                   || (line.kind == Kind::MalformedLightSource && line.fields[0] == name);
        });
    }

    void HardwareConfiguration::setCoreRole(const QString& role, const QString& value)
    {
        const auto ofRole = [&](const Line& line)
        {
            return (line.kind == Kind::Property && line.fields[1] == QStringLiteral("Core") && line.fields[2] == role)
                   || (line.kind == Kind::ConfigGroup && line.fields.size() >= 6
                       && line.fields[1] == QStringLiteral("System") && line.fields[2] == QStringLiteral("Startup")
                       && line.fields[3] == QStringLiteral("Core") && line.fields[4] == role);
        };
        if (value.isEmpty())
        {
            removeLines(ofRole);
            return;
        }
        const qsizetype startup = lastLine([&](const Line& line) { return ofRole(line) && line.kind == Kind::ConfigGroup; });
        if (startup >= 0)
        {
            QStringList fields = m_lines[startup].fields;
            fields[5] = value;
            replaceFields(startup, fields);
            return;
        }
        const QStringList fields{QStringLiteral("Property"), QStringLiteral("Core"), role, value};
        const qsizetype index = lastLine(ofRole);
        if (index >= 0)
        {
            replaceFields(index, fields);
            return;
        }
        // A new role goes behind the other lines of Core, then behind the last Property line
        const qsizetype core = lastLine([](const Line& line)
        {
            return line.kind == Kind::Property && line.fields[1] == QStringLiteral("Core");
        });
        if (core >= 0)
        {
            insertLine(core + 1, fields);
        }
        else
        {
            insertAfterLast(Kind::Property, fields);
        }
    }

    qsizetype HardwareConfiguration::lastLine(const std::function<bool(const Line&)>& match) const
    {
        for (qsizetype index = m_lines.size() - 1; index >= 0; --index)
        {
            if (match(m_lines[index]))
            {
                return index;
            }
        }
        return -1;
    }

    qsizetype HardwareConfiguration::removeLines(const std::function<bool(const Line&)>& match)
    {
        qsizetype first = -1;
        for (qsizetype index = m_lines.size() - 1; index >= 0; --index)
        {
            if (match(m_lines[index]))
            {
                m_lines.removeAt(index);
                first = index;
            }
        }
        return first;
    }

    // The line ending that new lines get, the one of the first line that has one
    QByteArray HardwareConfiguration::documentEnding() const
    {
        for (const Line& line : m_lines)
        {
            if (!line.ending.isEmpty())
            {
                return line.ending;
            }
        }
        return "\n";
    }

    // Inserts a line before the place at. The line before it gets an ending if the file ended
    // without one, so the new line does not join it.
    void HardwareConfiguration::insertLine(qsizetype at, const QStringList& fields)
    {
        const QByteArray ending = documentEnding();
        if (at > 0 && m_lines[at - 1].ending.isEmpty())
        {
            m_lines[at - 1].ending = ending;
        }
        m_lines.insert(at, parseLine(encode(fields.join(QLatin1Char(',')), false), ending));
    }

    // Inserts a line behind the last line of its kind, or at the end of the file. It returns the
    // place of the new line.
    qsizetype HardwareConfiguration::insertAfterLast(Kind kind, const QStringList& fields)
    {
        const qsizetype last = lastLine([&](const Line& line) { return line.kind == kind; });
        const qsizetype at = last < 0 ? m_lines.size() : last + 1;
        insertLine(at, fields);
        return at;
    }

    void HardwareConfiguration::replaceFields(qsizetype index, const QStringList& fields)
    {
        const Line old = m_lines[index];
        const QString text = fields.join(QLatin1Char(','));
        // Latin-1 cannot represent every edited name; use UTF-8 when it would lose characters.
        const bool latin1 = old.latin1 && QString::fromLatin1(text.toLatin1()) == text;
        m_lines[index] = parseLine(encode(text, latin1), old.ending);
    }
}
