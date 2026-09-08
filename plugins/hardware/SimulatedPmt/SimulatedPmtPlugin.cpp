#include "SimulatedPmtPlugin.h"

#include "SimulatedPmtSource.h"

namespace scopeone::plugins
{
    namespace
    {
        constexpr auto kSourceId = "pmt:simulator";

        scopeone::core::SignalParameterDescriptor realParameter(
            const QString& key,
            const QString& name,
            double value,
            double minimum,
            double maximum,
            const QString& suffix)
        {
            scopeone::core::SignalParameterDescriptor parameter;
            parameter.key = key;
            parameter.name = name;
            parameter.type = scopeone::core::SignalParameterType::Real;
            parameter.defaultValue = value;
            parameter.hasRange = true;
            parameter.minimum = minimum;
            parameter.maximum = maximum;
            parameter.suffix = suffix;
            return parameter;
        }

        scopeone::core::SignalParameterDescriptor choiceParameter(
            const QString& key,
            const QString& name,
            const QStringList& values,
            const QStringList& names,
            const QString& value)
        {
            scopeone::core::SignalParameterDescriptor parameter;
            parameter.key = key;
            parameter.name = name;
            parameter.type = scopeone::core::SignalParameterType::Choice;
            parameter.defaultValue = value;
            for (int index = 0; index < values.size(); ++index)
            {
                parameter.choices.append(values[index]);
                parameter.choiceNames.append(names[index]);
            }
            return parameter;
        }

        scopeone::core::SignalSourceDescriptor sourceDescriptor()
        {
            using namespace scopeone::core;

            SignalSourceDescriptor descriptor;
            descriptor.id = QString::fromLatin1(kSourceId);
            descriptor.name = QStringLiteral("Simulated PMT");
            descriptor.provider = QStringLiteral("ScopeOne");
            descriptor.quantity = QStringLiteral("Photon count rate");
            descriptor.unit = QStringLiteral("counts/s");
            descriptor.streamType = SignalStreamType::TimestampedEvents;
            descriptor.parameters.append(realParameter(
                QStringLiteral("voltage"),
                QStringLiteral("Control voltage"),
                1.0,
                0.0,
                1.25,
                QStringLiteral(" V")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("gain"),
                QStringLiteral("Gain"),
                1.0,
                1.0,
                100.0,
                QString()));
            descriptor.parameters.append(realParameter(
                QStringLiteral("baseRate"),
                QStringLiteral("Base rate"),
                1000000.0,
                10000.0,
                1000000.0,
                QStringLiteral(" counts/s")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("darkCountRate"),
                QStringLiteral("Dark count rate"),
                500.0,
                0.0,
                5000.0,
                QStringLiteral(" counts/s")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("modulationFrequency"),
                QStringLiteral("Modulation frequency"),
                2.0,
                0.0,
                1000.0,
                QStringLiteral(" Hz")));
            descriptor.parameters.append(realParameter(
                QStringLiteral("scanFrameRate"),
                QStringLiteral("Scan frame rate"),
                2.0,
                0.5,
                10.0,
                QStringLiteral(" Hz")));
            descriptor.parameters.append(choiceParameter(
                QStringLiteral("waveform"),
                QStringLiteral("Waveform"),
                {QStringLiteral("constant"), QStringLiteral("sine"), QStringLiteral("square")},
                {QStringLiteral("Constant"), QStringLiteral("Sine"), QStringLiteral("Square")},
                QStringLiteral("constant")));
            descriptor.parameters.append(choiceParameter(
                QStringLiteral("noiseMode"),
                QStringLiteral("Noise"),
                {QStringLiteral("poisson"), QStringLiteral("gaussian"), QStringLiteral("none")},
                {QStringLiteral("Poisson"), QStringLiteral("Gaussian"), QStringLiteral("None")},
                QStringLiteral("poisson")));
            descriptor.parameters.append(choiceParameter(
                QStringLiteral("pattern"),
                QStringLiteral("Pattern"),
                {QStringLiteral("beads"), QStringLiteral("usaf"),
                 QStringLiteral("grid"), QStringLiteral("cells")},
                {QStringLiteral("Fluorescent Beads"), QStringLiteral("USAF Target"),
                 QStringLiteral("Concentric Grid"), QStringLiteral("Cell Structure")},
                QStringLiteral("beads")));
            return descriptor;
        }
    }

    QList<scopeone::core::SignalSourceDescriptor> SimulatedPmtPlugin::signalSources() const
    {
        return {sourceDescriptor()};
    }

    scopeone::core::SignalSource* SimulatedPmtPlugin::createSignalSource(
        const QString& sourceId,
        QObject* parent)
    {
        return sourceId.trimmed() == QString::fromLatin1(kSourceId)
                   ? new SimulatedPmtSource(parent)
                   : nullptr;
    }
}
