// Saving and restoring the settings of the recording panel, as the settings that were used last

#include "RecordingWidget.h"
#include "scopeone/ScopeOneCore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>

namespace scopeone::ui
{
    namespace
    {
        constexpr auto kLastSettingsKey = "Recording/LastSettings";

        QJsonObject readJsonSetting(const char* key)
        {
            QSettings settings("ScopeOne", "ScopeOne");
            return QJsonDocument::fromJson(settings.value(key).toString().toUtf8()).object();
        }

        void writeJsonSetting(const char* key, const QJsonObject& object)
        {
            QSettings settings("ScopeOne", "ScopeOne");
            settings.setValue(key, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
        }
    }

    // Collects what the panel is set to, apart from where the images are saved
    QJsonObject RecordingWidget::captureSettings() const
    {
        QJsonObject object;
        object.insert(QStringLiteral("format"), m_formatCombo->currentData().toInt());
        object.insert(QStringLiteral("compression"), m_compressionCheck->isChecked());
        object.insert(QStringLiteral("compressionLevel"), m_compressionLevelSpin->value());
        object.insert(QStringLiteral("pyramid"), m_pyramidCheck->isChecked());
        object.insert(QStringLiteral("frames"), m_framesSpin->value());
        object.insert(QStringLiteral("intervalMs"), m_mdaIntervalSpin->value());
        object.insert(QStringLiteral("burst"), m_burstModeCheck->isChecked());
        object.insert(QStringLiteral("bursts"), m_burstCountSpin->value());
        object.insert(QStringLiteral("burstInterval"), m_burstIntervalSpin->value());
        object.insert(QStringLiteral("burstUnit"), m_burstIntervalUnitCombo->currentText());

        object.insert(QStringLiteral("zEnabled"), m_mdaEnableZCheck->isChecked());
        object.insert(QStringLiteral("zStart"), m_mdaZStartSpin->value());
        object.insert(QStringLiteral("zStep"), m_mdaZStepSpin->value());
        object.insert(QStringLiteral("zCount"), m_mdaZCountSpin->value());
        object.insert(QStringLiteral("zRelative"), m_mdaZRelativeCheck->isChecked());

        object.insert(QStringLiteral("xyEnabled"), m_mdaEnableXYCheck->isChecked());
        object.insert(QStringLiteral("xStart"), m_mdaXStartSpin->value());
        object.insert(QStringLiteral("xStep"), m_mdaXStepSpin->value());
        object.insert(QStringLiteral("xCount"), m_mdaXCountSpin->value());
        object.insert(QStringLiteral("yStart"), m_mdaYStartSpin->value());
        object.insert(QStringLiteral("yStep"), m_mdaYStepSpin->value());
        object.insert(QStringLiteral("yCount"), m_mdaYCountSpin->value());
        QJsonArray positions;
        for (int row = 0; row < m_positionTable->rowCount(); ++row)
        {
            QJsonArray cells;
            for (int column = 0; column < 4; ++column)
            {
                const QTableWidgetItem* item = m_positionTable->item(row, column);
                cells.append(item ? item->text() : QString());
            }
            positions.append(cells);
        }
        object.insert(QStringLiteral("positions"), positions);

        object.insert(QStringLiteral("autofocus"), m_mdaAutofocusCheck->isChecked());
        object.insert(QStringLiteral("autofocusEvery"), m_mdaAutofocusEverySpin->value());
        object.insert(QStringLiteral("keepShutterSlices"), m_keepShutterSlicesCheck->isChecked());
        object.insert(QStringLiteral("keepShutterChannels"), m_keepShutterChannelsCheck->isChecked());
        object.insert(QStringLiteral("hardwareSequencing"), m_hardwareSequencingCheck->isChecked());
        object.insert(QStringLiteral("comment"), m_commentLineEdit->text());

        QJsonArray order;
        for (const int axis : m_orderPreference)
        {
            order.append(axis);
        }
        object.insert(QStringLiteral("order"), order);

        object.insert(QStringLiteral("channelsEnabled"), m_mdaEnableChannelCheck->isChecked());
        object.insert(QStringLiteral("channelGroup"), m_channelGroupCombo->currentText());
        QJsonArray channels;
        for (int row = 0; row < m_channelTable->rowCount(); ++row)
        {
            QJsonObject channel;
            channel.insert(QStringLiteral("used"), m_channelTable->item(row, 0)->checkState() == Qt::Checked);
            channel.insert(QStringLiteral("preset"), m_channelTable->item(row, 1)->text());
            channel.insert(QStringLiteral("exposureMs"), m_channelTable->item(row, 2)->data(Qt::EditRole).toDouble());
            channel.insert(QStringLiteral("zOffsetUm"), m_channelTable->item(row, 3)->data(Qt::EditRole).toDouble());
            channel.insert(QStringLiteral("zStack"), m_channelTable->item(row, 4)->checkState() == Qt::Checked);
            channel.insert(QStringLiteral("skipFrames"), m_channelTable->item(row, 5)->data(Qt::EditRole).toInt());
            if (const auto* camera = qobject_cast<QComboBox*>(m_channelTable->cellWidget(row, 6)))
            {
                channel.insert(QStringLiteral("camera"), camera->currentText());
            }
            if (const auto* color = qobject_cast<QPushButton*>(m_channelTable->cellWidget(row, 7)))
            {
                channel.insert(QStringLiteral("color"), color->property("channelColor").toString());
            }
            channels.append(channel);
        }
        object.insert(QStringLiteral("channels"), channels);
        return object;
    }

    // Sets the panel from settings that captureSettings wrote. Missing entries keep their value.
    void RecordingWidget::applySettings(const QJsonObject& object)
    {
        const auto has = [&object](const char* key) { return object.contains(QLatin1String(key)); };
        const auto number = [&object](const char* key) { return object.value(QLatin1String(key)).toDouble(); };
        const auto flag = [&object](const char* key) { return object.value(QLatin1String(key)).toBool(); };

        if (has("format"))
        {
            const int index = m_formatCombo->findData(object.value(QStringLiteral("format")).toInt());
            if (index >= 0) m_formatCombo->setCurrentIndex(index);
        }
        if (has("compression")) m_compressionCheck->setChecked(flag("compression"));
        if (has("compressionLevel")) m_compressionLevelSpin->setValue(static_cast<int>(number("compressionLevel")));
        if (has("pyramid")) m_pyramidCheck->setChecked(flag("pyramid"));
        if (has("frames")) m_framesSpin->setValue(static_cast<int>(number("frames")));
        if (has("intervalMs")) m_mdaIntervalSpin->setValue(number("intervalMs"));
        if (has("burst")) m_burstModeCheck->setChecked(flag("burst"));
        if (has("bursts")) m_burstCountSpin->setValue(static_cast<int>(number("bursts")));
        if (has("burstInterval")) m_burstIntervalSpin->setValue(number("burstInterval"));
        if (has("burstUnit"))
        {
            const int index = m_burstIntervalUnitCombo->findText(object.value(QStringLiteral("burstUnit")).toString());
            if (index >= 0) m_burstIntervalUnitCombo->setCurrentIndex(index);
        }

        if (has("zEnabled")) m_mdaEnableZCheck->setChecked(flag("zEnabled"));
        if (has("zStart")) m_mdaZStartSpin->setValue(number("zStart"));
        if (has("zStep")) m_mdaZStepSpin->setValue(number("zStep"));
        if (has("zCount")) m_mdaZCountSpin->setValue(static_cast<int>(number("zCount")));
        if (has("zRelative")) m_mdaZRelativeCheck->setChecked(flag("zRelative"));

        if (has("xyEnabled")) m_mdaEnableXYCheck->setChecked(flag("xyEnabled"));
        if (has("xStart")) m_mdaXStartSpin->setValue(number("xStart"));
        if (has("xStep")) m_mdaXStepSpin->setValue(number("xStep"));
        if (has("xCount")) m_mdaXCountSpin->setValue(static_cast<int>(number("xCount")));
        if (has("yStart")) m_mdaYStartSpin->setValue(number("yStart"));
        if (has("yStep")) m_mdaYStepSpin->setValue(number("yStep"));
        if (has("yCount")) m_mdaYCountSpin->setValue(static_cast<int>(number("yCount")));
        if (has("positions"))
        {
            m_positionTable->setRowCount(0);
            for (const QJsonValue& value : object.value(QStringLiteral("positions")).toArray())
            {
                const QJsonArray cells = value.toArray();
                if (cells.size() == 4)
                {
                    const auto text = [&cells](int index) { return cells.at(index).toString(); };
                    addListedPosition(text(0), text(1).toDouble(), text(2).toDouble(), text(3));
                }
            }
        }

        if (has("autofocus")) m_mdaAutofocusCheck->setChecked(flag("autofocus"));
        if (has("autofocusEvery")) m_mdaAutofocusEverySpin->setValue(static_cast<int>(number("autofocusEvery")));
        if (has("keepShutterSlices")) m_keepShutterSlicesCheck->setChecked(flag("keepShutterSlices"));
        if (has("keepShutterChannels")) m_keepShutterChannelsCheck->setChecked(flag("keepShutterChannels"));
        if (has("hardwareSequencing")) m_hardwareSequencingCheck->setChecked(flag("hardwareSequencing"));
        if (has("comment")) m_commentLineEdit->setText(object.value(QStringLiteral("comment")).toString());

        if (has("order"))
        {
            // Only the four axes of the panel are accepted, each once
            std::vector<int> order;
            for (const QJsonValue& value : object.value(QStringLiteral("order")).toArray())
            {
                const int axis = value.toInt(-1);
                if (axis >= 0 && axis <= 3 && std::find(order.begin(), order.end(), axis) == order.end())
                {
                    order.push_back(axis);
                }
            }
            if (order.size() == m_orderPreference.size())
            {
                m_orderPreference = order;
            }
        }

        if (has("channelsEnabled")) m_mdaEnableChannelCheck->setChecked(flag("channelsEnabled"));
        if (has("channels"))
        {
            // The presets are listed once the hardware is loaded, which may be later
            m_savedChannelGroup = object.value(QStringLiteral("channelGroup")).toString();
            m_savedChannels = object.value(QStringLiteral("channels")).toArray();
            const int index = m_channelGroupCombo->findText(m_savedChannelGroup);
            if (index >= 0)
            {
                QSignalBlocker blocker(m_channelGroupCombo);
                m_channelGroupCombo->setCurrentIndex(index);
            }
            m_channelTable->setRowCount(0);
            loadChannelPresets();
        }
        syncOrderList();
        updateUiState();
    }

    // Remembers the settings for the next start of the application
    void RecordingWidget::saveLastSettings() const
    {
        writeJsonSetting(kLastSettingsKey, captureSettings());
    }

    void RecordingWidget::restoreLastSettings()
    {
        const QJsonObject object = readJsonSetting(kLastSettingsKey);
        if (!object.isEmpty())
        {
            applySettings(object);
        }
    }
}
