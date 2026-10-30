#include "scopeone/PluginManifest.h"
#include "scopeone/ProcessingPlugin.h"

#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QThread>

#include <memory>

// Runs processing modules written in Python. Every script in the python folder of a processing
// plugin directory is asked for its modules once, and each module in a pipeline then talks to
// its own Python process, one frame at a time, over stdin and stdout.
namespace
{
    using scopeone::core::ImageFrame;
    using scopeone::core::ImagePixelFormat;
    using scopeone::core::ProcessingModule;
    using scopeone::core::ProcessingModuleDescriptor;
    using scopeone::core::ProcessingParameterChoice;
    using scopeone::core::ProcessingParameterDescriptor;
    using scopeone::core::ProcessingParameterType;
    using scopeone::core::ProcessingResult;

    // SCOPEONE_PYTHON picks the interpreter, which needs numpy and the scopeone package
    QString pythonExecutable()
    {
        const QString configured = qEnvironmentVariable("SCOPEONE_PYTHON");
        if (!configured.isEmpty())
        {
            return configured;
        }
#ifdef Q_OS_WIN
        return QStringLiteral("python");
#else
        return QStringLiteral("python3");
#endif
    }

    QStringList workerArguments(const QString& command, const QString& scriptPath)
    {
        return {QStringLiteral("-m"), QStringLiteral("scopeone.processing"), command, scriptPath};
    }

    ProcessingParameterDescriptor parameterFrom(const QJsonObject& object)
    {
        static const QHash<QString, ProcessingParameterType> types{
            {QStringLiteral("int"), ProcessingParameterType::Integer},
            {QStringLiteral("real"), ProcessingParameterType::Real},
            {QStringLiteral("bool"), ProcessingParameterType::Boolean},
            {QStringLiteral("choice"), ProcessingParameterType::Choice}};
        ProcessingParameterDescriptor parameter;
        parameter.key = object.value(QStringLiteral("key")).toString();
        parameter.name = object.value(QStringLiteral("name")).toString();
        parameter.type = types.value(object.value(QStringLiteral("type")).toString());
        parameter.defaultValue = object.value(QStringLiteral("default")).toVariant();
        parameter.minimum = object.value(QStringLiteral("minimum")).toVariant();
        parameter.maximum = object.value(QStringLiteral("maximum")).toVariant();
        parameter.step = object.value(QStringLiteral("step")).toVariant();
        parameter.decimals = object.value(QStringLiteral("decimals")).toInt();
        for (const QJsonValue& choice : object.value(QStringLiteral("choices")).toArray())
        {
            parameter.choices.append({choice[QStringLiteral("name")].toString(),
                                      choice[QStringLiteral("value")].toVariant()});
        }
        return parameter;
    }

    struct PythonScriptModule
    {
        QString scriptPath;
        ProcessingModuleDescriptor descriptor;
    };

    // The modules a script declares, as `python -m scopeone.processing describe` lists them
    QList<PythonScriptModule> describeScript(const QString& scriptPath)
    {
        QProcess process;
        process.start(pythonExecutable(), workerArguments(QStringLiteral("describe"), scriptPath));
        process.waitForFinished(30000);
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        {
            qWarning().noquote() << QStringLiteral("Python processing script %1 failed: %2")
                                        .arg(scriptPath, QString::fromUtf8(process.readAllStandardError()));
            return {};
        }

        QList<PythonScriptModule> modules;
        for (const QJsonValue& value : QJsonDocument::fromJson(process.readAllStandardOutput()).array())
        {
            const QJsonObject object = value.toObject();
            PythonScriptModule module;
            module.scriptPath = scriptPath;
            module.descriptor.id = object.value(QStringLiteral("id")).toString();
            module.descriptor.name = object.value(QStringLiteral("name")).toString();
            for (const QJsonValue& parameter : object.value(QStringLiteral("parameters")).toArray())
            {
                module.descriptor.parameters.append(parameterFrom(parameter.toObject()));
            }
            modules.append(module);
        }
        return modules;
    }

    // One module of a pipeline. The pipeline calls process from changing pool threads, so the
    // Python process lives on a thread of its own and every frame is handed to that thread.
    class PythonProcessingModule final : public ProcessingModule
    {
    public:
        explicit PythonProcessingModule(std::shared_ptr<const PythonScriptModule> script)
            : m_script(std::move(script))
        {
            for (const ProcessingParameterDescriptor& parameter : m_script->descriptor.parameters)
            {
                m_parameters.insert(parameter.key, parameter.defaultValue);
            }
        }

        ~PythonProcessingModule() override
        {
            if (m_thread)
            {
                QMetaObject::invokeMethod(m_threadContext.get(), [this]() { m_process.reset(); },
                                          Qt::BlockingQueuedConnection);
                m_thread->quit();
                m_thread->wait();
            }
        }

        QString id() const override { return m_script->descriptor.id; }
        QString name() const override { return m_script->descriptor.name; }
        QVariantMap parameters() const override { return m_parameters; }

        void setParameters(const QVariantMap& parameters) override
        {
            for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it)
            {
                m_parameters.insert(it.key(), it.value());
            }
        }

        std::unique_ptr<ProcessingModule> createRuntime() const override
        {
            auto runtime = std::make_unique<PythonProcessingModule>(m_script);
            runtime->setParameters(m_parameters);
            return runtime;
        }

        ProcessingResult process(const ImageFrame& frame, int) override
        {
            if (!m_thread)
            {
                m_thread = std::make_unique<QThread>();
                m_threadContext = std::make_unique<QObject>();
                m_threadContext->moveToThread(m_thread.get());
                m_thread->start();
            }
            ProcessingResult result;
            QMetaObject::invokeMethod(m_threadContext.get(), [&]() { result = exchange(frame); },
                                      Qt::BlockingQueuedConnection);
            return result;
        }

    private:
        ProcessingResult failure(const QString& message)
        {
            const QString log = m_process ? QString::fromUtf8(m_process->readAllStandardError()) : QString();
            m_process.reset();
            return ProcessingResult(ImageFrame{}, log.isEmpty() ? message : message + QLatin1Char('\n') + log);
        }

        // Sends one frame and reads the frame that comes back, on the module's own thread
        ProcessingResult exchange(const ImageFrame& frame)
        {
            if (!m_process)
            {
                m_process = std::make_unique<QProcess>();
                m_process->start(pythonExecutable(),
                                 workerArguments(QStringLiteral("serve"), m_script->scriptPath));
                if (!m_process->waitForStarted(10000))
                {
                    return failure(QStringLiteral("Could not start %1").arg(pythonExecutable()));
                }
            }

            const QJsonObject request{
                {QStringLiteral("module"), m_script->descriptor.id},
                {QStringLiteral("width"), frame.width},
                {QStringLiteral("height"), frame.height},
                {QStringLiteral("stride"), frame.stride},
                {QStringLiteral("format"), frame.isMono16() ? QStringLiteral("mono16") : QStringLiteral("mono8")},
                {QStringLiteral("parameters"), QJsonObject::fromVariantMap(m_parameters)},
                {QStringLiteral("bytes"), static_cast<qint64>(frame.bytes.size())}};
            m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
            m_process->write(frame.bytes);

            while (!m_process->canReadLine())
            {
                if (!m_process->waitForReadyRead(30000))
                {
                    return failure(QStringLiteral("Python module %1 stopped answering").arg(id()));
                }
            }
            const QJsonObject reply = QJsonDocument::fromJson(m_process->readLine()).object();
            const QByteArray printed = m_process->readAllStandardError();
            if (!printed.isEmpty())
            {
                qInfo().noquote() << QStringLiteral("[%1] %2").arg(id(), QString::fromUtf8(printed).trimmed());
            }
            if (reply.contains(QStringLiteral("error")))
            {
                return ProcessingResult(ImageFrame{}, reply.value(QStringLiteral("error")).toString());
            }

            const qint64 byteCount = reply.value(QStringLiteral("bytes")).toInteger();
            while (m_process->bytesAvailable() < byteCount)
            {
                if (!m_process->waitForReadyRead(30000))
                {
                    return failure(QStringLiteral("Python module %1 stopped answering").arg(id()));
                }
            }

            ImageFrame output = frame;
            const bool mono16 = reply.value(QStringLiteral("format")).toString() == QStringLiteral("mono16");
            output.pixelFormat = mono16 ? ImagePixelFormat::Mono16 : ImagePixelFormat::Mono8;
            output.bitsPerSample = mono16 ? (frame.isMono16() ? frame.bitsPerSample : 16) : 8;
            output.width = reply.value(QStringLiteral("width")).toInt();
            output.height = reply.value(QStringLiteral("height")).toInt();
            output.stride = output.width * (mono16 ? 2 : 1);
            output.bytes = m_process->read(byteCount);
            return ProcessingResult(output);
        }

        std::shared_ptr<const PythonScriptModule> m_script;
        QVariantMap m_parameters;
        std::unique_ptr<QThread> m_thread;
        std::unique_ptr<QObject> m_threadContext;
        std::unique_ptr<QProcess> m_process;
    };

    class PythonProcessingPlugin final : public QObject,
                                         public scopeone::core::ProcessingPlugin
    {
        Q_OBJECT
        Q_PLUGIN_METADATA(IID ScopeOneProcessingPlugin_iid FILE "scopeone_plugin.json")
        Q_INTERFACES(scopeone::core::ProcessingPlugin)

    public:
        PythonProcessingPlugin()
        {
            for (const QString& directory : scopeone::core::pluginDirectories(
                     scopeone::core::PluginKind::Processing))
            {
                const QDir scripts(QDir(directory).filePath(QStringLiteral("python")));
                for (const QString& name : scripts.entryList({QStringLiteral("*.py")}, QDir::Files, QDir::Name))
                {
                    for (const PythonScriptModule& module : describeScript(scripts.filePath(name)))
                    {
                        m_modules.insert(module.descriptor.id,
                                         std::make_shared<const PythonScriptModule>(module));
                    }
                }
            }
        }

        QList<ProcessingModuleDescriptor> processingModules() const override
        {
            QList<ProcessingModuleDescriptor> descriptors;
            for (const auto& module : m_modules)
            {
                descriptors.append(module->descriptor);
            }
            return descriptors;
        }

        std::unique_ptr<ProcessingModule> createProcessingModule(const QString& moduleId) override
        {
            const auto module = m_modules.value(moduleId);
            return module ? std::make_unique<PythonProcessingModule>(module) : nullptr;
        }

    private:
        QHash<QString, std::shared_ptr<const PythonScriptModule>> m_modules;
    };
}

#include "PythonProcessingPlugin.moc"
