#include "backend.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTime>
#include <algorithm>

namespace {

constexpr const char *kJarvisPersonaPrefix =
    "Respond in character as J.A.R.V.I.S.: a composed, dry-witted, formal British AI butler assisting "
    "the user (address them politely as 'sir'). Keep responses concise and natural to speak aloud "
    "(1 to 3 crisp sentences unless the user asks for details or code). Avoid excessive markdown formatting.\n\n"
    "User request: ";

const QStringList kCompletionLines = {
    "Task complete, sir.",
    "All operations concluded, sir.",
    "Work completed without issue, sir.",
    "Everything is in order, sir.",
};

const QStringList kErrorLines = {
    "I'm afraid we've encountered an anomaly, sir.",
    "Warning, sir. An operation did not complete cleanly.",
};

struct AgentSpec {
    const char *id;
    const char *name;
    const char *shortLabel;
    const char *misePackage;
};

constexpr AgentSpec kKnownAgents[] = {
    {"agy", "Antigravity", "AGY", "antigravity-cli"},
    {"claude", "Claude Code", "CLAUDE", "claude"},
    {"codex", "Codex", "CODEX", "codex"},
    {"opencode", "OpenCode", "OC", "opencode"},
    {"grok", "Grok", "GROK", "grok"},
    {"copilot", "GitHub Copilot", "COPILOT", "copilot"},
    {"crush", "Crush", "CRUSH", "crush"},
    {"cursor-agent", "Cursor CLI", "CURSOR", "cursor-agent"},
    {"pi", "Pi", "PI", "pi"},
    {"hermes", "Hermes", "HERMES", "hermes"},
};

} // namespace

Backend::Backend(const QString &dataDir, const QString &configDir, bool headless,
                 const QString &omarchyAgentFile, QObject *parent)
    : QObject(parent),
      m_dataDir(dataDir.isEmpty() ? QDir::homePath() + "/.local/share/jarvis" : dataDir),
      m_configDir(configDir.isEmpty() ? QDir::homePath() + "/.config/jarvis" : configDir),
      m_omarchyAgentFile(omarchyAgentFile.isEmpty() ? QDir::homePath() + "/.config/omarchy/defaults/agent"
                                                    : omarchyAgentFile),
      m_headless(headless) {
    QDir().mkpath(m_dataDir);
    QDir().mkpath(m_dataDir + "/sounds");
    QDir().mkpath(m_configDir);

    loadSettings();

    m_agentDebounce.setSingleShot(true);
    m_agentDebounce.setInterval(80);
    connect(&m_agentWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_agentDebounce.start(); });
    connect(&m_agentWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_agentDebounce.start(); });
    connect(&m_agentDebounce, &QTimer::timeout, this, &Backend::reloadOmarchyDefaultAgent);
    reloadOmarchyDefaultAgent();

    m_audioTickTimer.setInterval(33); // ~30 fps amplitude sync
    connect(&m_audioTickTimer, &QTimer::timeout, this, &Backend::onAudioTick);

    m_idleRevertTimer.setSingleShot(true);
    connect(&m_idleRevertTimer, &QTimer::timeout, this, [this] {
        if (!m_busy && m_audioPlayer.state() == QProcess::NotRunning) {
            setActiveTool({});
            setState("idle");
        }
    });

    m_resizeSyncTimer.setSingleShot(true);
    m_resizeSyncTimer.setInterval(20);
    connect(&m_resizeSyncTimer, &QTimer::timeout, this, [this] {
        if (m_pendingWindowWidth > 0 && m_pendingWindowHeight > 0)
            applyHyprlandResize(m_pendingWindowWidth, m_pendingWindowHeight);
    });

    connect(&m_ttsWorker, &QProcess::readyReadStandardOutput, this, &Backend::onTtsWorkerOutput);
    connect(&m_micWorker, &QProcess::readyReadStandardOutput, this, &Backend::onMicWorkerOutput);
    connect(&m_micWorker, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this] {
        if (m_micListening) {
            m_micListening = false;
            emit micListeningChanged();
        }
        if (m_micEnabled && !m_headless) {
            m_micEnabled = false;
            emit micEnabledChanged();
        }
    });
    connect(&m_audioPlayer, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &Backend::onPlayerFinished);
    connect(&m_audioPlayer, &QProcess::errorOccurred, this, [this] { onPlayerFinished(); });

    connect(&m_agentProcess, &QProcess::readyReadStandardOutput, this, &Backend::onAgentStdout);
    connect(&m_agentProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &Backend::onAgentFinished);

    connect(&m_acpProcess, &QProcess::readyReadStandardOutput, this, &Backend::onAcpStdout);
    connect(&m_acpProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            &Backend::onAcpFinished);

    if (!m_headless) {
        startHookServer();
        startTtsWorker();

        m_pinSyncTimer.setSingleShot(true);
        m_pinSyncTimer.setInterval(700);
        connect(&m_pinSyncTimer, &QTimer::timeout, this, [this] {
            if (m_alwaysOnTop)
                applyHyprlandPin(true);
            greet();
        });
        m_pinSyncTimer.start();
    }
}

Backend::~Backend() {
    m_hookServer.close();
    stopMicWorker();
    if (m_audioPlayer.state() != QProcess::NotRunning) {
        m_audioPlayer.kill();
        m_audioPlayer.waitForFinished(500);
    }
    if (m_ttsWorker.state() != QProcess::NotRunning) {
        m_ttsWorker.closeWriteChannel();
        if (!m_ttsWorker.waitForFinished(500)) {
            m_ttsWorker.kill();
            m_ttsWorker.waitForFinished(500);
        }
    }
    if (m_agentProcess.state() != QProcess::NotRunning) {
        m_agentProcess.kill();
        m_agentProcess.waitForFinished(500);
    }
    stopAcpProcess();
}

QString Backend::formatToolLabel(const QString &toolName) {
    if (toolName.isEmpty())
        return {};
    QString label = toolName.trimmed();
    label.replace('_', ' ');
    return label.toUpper();
}

QString Backend::greetingForHour(int hour) {
    if (hour >= 5 && hour < 12)
        return "Good morning, sir. All systems are online and at your disposal.";
    if (hour >= 12 && hour < 18)
        return "Good afternoon, sir. Systems are online and standing by.";
    return "Good evening, sir. All systems are online and standing by.";
}

QString Backend::readOmarchyDefaultAgent(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return "agy";
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    return line.isEmpty() ? QString("agy") : line;
}

QString Backend::agentDisplayName(const QString &agentId) {
    for (const auto &spec : kKnownAgents) {
        if (agentId == QLatin1String(spec.id))
            return QString::fromLatin1(spec.name);
    }
    if (agentId.isEmpty())
        return "Antigravity";
    QString fallback = agentId;
    fallback[0] = fallback[0].toUpper();
    return fallback;
}

QString Backend::agentShortLabel(const QString &agentId) {
    for (const auto &spec : kKnownAgents) {
        if (agentId == QLatin1String(spec.id))
            return QString::fromLatin1(spec.shortLabel);
    }
    return agentId.left(6).toUpper();
}

QString Backend::effectiveAgent() const {
    if (m_selectedAgent.isEmpty() || m_selectedAgent == "auto")
        return m_omarchyDefaultAgent.isEmpty() ? QString("agy") : m_omarchyDefaultAgent;
    return m_selectedAgent;
}

QString Backend::effectiveAgentName() const {
    return agentDisplayName(effectiveAgent());
}

QString Backend::effectiveAgentShort() const {
    return agentShortLabel(effectiveAgent());
}

bool Backend::isMiseWrapperScript(const QString &path) {
    QFileInfo info(path);
    if (info.isSymLink())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(4096);
    static const QRegularExpression re("mise\\s+(use|x)\\s+");
    return re.match(QString::fromLatin1(head)).hasMatch();
}

QString Backend::resolveAgentExecutable(const QString &agentId, bool *isInstalled) const {
    if (isInstalled)
        *isInstalled = false;

    const QString home = QDir::homePath();

    // 1. Check user's own install in ~/.local/bin (skipping lazy mise wrapper scripts).
    const QString localBin = home + "/.local/bin/" + agentId;
    if (QFileInfo::exists(localBin) && QFileInfo(localBin).isExecutable() && !isMiseWrapperScript(localBin)) {
        if (isInstalled)
            *isInstalled = true;
        return localBin;
    }

    // 2. Check concrete mise install directory so we never trigger a surprise download on lookup.
    QString pkg = agentId;
    for (const auto &spec : kKnownAgents) {
        if (agentId == QLatin1String(spec.id)) {
            pkg = QString::fromLatin1(spec.misePackage);
            break;
        }
    }
    const QString miseRoot = home + "/.local/share/mise/installs/" + pkg;
    const QStringList miseCandidates = {
        miseRoot + "/latest/" + agentId,
        miseRoot + "/latest/bin/" + agentId,
    };
    for (const QString &candidate : miseCandidates) {
        if (QFileInfo::exists(candidate) && QFileInfo(candidate).isExecutable()) {
            if (isInstalled)
                *isInstalled = true;
            return candidate;
        }
    }
    if (QDir(miseRoot).exists()) {
        const QStringList subdirs = QDir(miseRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &sub : subdirs) {
            for (const QString &rel : {sub + "/" + agentId, sub + "/bin/" + agentId}) {
                const QString full = miseRoot + "/" + rel;
                if (QFileInfo::exists(full) && QFileInfo(full).isExecutable()) {
                    if (isInstalled)
                        *isInstalled = true;
                    return full;
                }
            }
        }
    }

    // 3. Check system /usr/bin and /usr/local/bin.
    for (const QString &sysPath : {"/usr/local/bin/" + agentId, "/usr/bin/" + agentId}) {
        if (QFileInfo::exists(sysPath) && QFileInfo(sysPath).isExecutable() && !isMiseWrapperScript(sysPath)) {
            if (isInstalled)
                *isInstalled = true;
            return sysPath;
        }
    }

    return {};
}

void Backend::watchOmarchyDefaultAgent() {
    const auto paths = m_agentWatcher.files() + m_agentWatcher.directories();
    if (!paths.isEmpty())
        m_agentWatcher.removePaths(paths);

    const QString dir = QFileInfo(m_omarchyAgentFile).absolutePath();
    if (QFileInfo::exists(dir))
        m_agentWatcher.addPath(dir);
    if (QFileInfo::exists(m_omarchyAgentFile))
        m_agentWatcher.addPath(m_omarchyAgentFile);
}

void Backend::reloadOmarchyDefaultAgent() {
    watchOmarchyDefaultAgent();
    const QString prevEffective = effectiveAgent();
    m_omarchyDefaultAgent = readOmarchyDefaultAgent(m_omarchyAgentFile);
    refreshAgents();

    if (m_selectedAgent == "auto" && prevEffective != effectiveAgent() && !m_conversationId.isEmpty()) {
        m_conversationId.clear();
        emit conversationIdChanged();
    }
}

void Backend::refreshAgents() {
    QVariantList list;

    bool defaultInstalled = false;
    resolveAgentExecutable(m_omarchyDefaultAgent, &defaultInstalled);
    QVariantMap autoEntry;
    autoEntry["id"] = "auto";
    autoEntry["name"] = QString("Omarchy Default (%1)").arg(agentDisplayName(m_omarchyDefaultAgent));
    autoEntry["shortLabel"] = agentShortLabel(m_omarchyDefaultAgent);
    autoEntry["installed"] = defaultInstalled;
    autoEntry["isDefault"] = true;
    list.append(autoEntry);

    QStringList seen;
    for (const auto &spec : kKnownAgents) {
        const QString id = QString::fromLatin1(spec.id);
        seen.append(id);
        bool installed = false;
        resolveAgentExecutable(id, &installed);
        QVariantMap entry;
        entry["id"] = id;
        entry["name"] = QString::fromLatin1(spec.name);
        entry["shortLabel"] = QString::fromLatin1(spec.shortLabel);
        entry["installed"] = installed;
        entry["isDefault"] = (id == m_omarchyDefaultAgent);
        list.append(entry);
    }

    // If Omarchy's default agent isn't in the preset table, include it too.
    if (!m_omarchyDefaultAgent.isEmpty() && !seen.contains(m_omarchyDefaultAgent)) {
        bool installed = false;
        resolveAgentExecutable(m_omarchyDefaultAgent, &installed);
        QVariantMap entry;
        entry["id"] = m_omarchyDefaultAgent;
        entry["name"] = agentDisplayName(m_omarchyDefaultAgent);
        entry["shortLabel"] = agentShortLabel(m_omarchyDefaultAgent);
        entry["installed"] = installed;
        entry["isDefault"] = true;
        list.append(entry);
    }

    m_availableAgents = list;
    emit agentChanged();
}

void Backend::selectAgent(const QString &agentId) {
    const QString target = agentId.trimmed().isEmpty() ? QString("auto") : agentId.trimmed();
    if (m_selectedAgent == target)
        return;

    cancelPrompt();
    m_selectedAgent = target;
    m_acpSessionId.clear();
    m_acpToolCalls.clear();
    if (!m_conversationId.isEmpty()) {
        m_conversationId.clear();
        emit conversationIdChanged();
    }
    saveSettings();
    emit agentChanged();

    const QString msg = (m_selectedAgent == "auto")
                            ? QString("Following Omarchy default agent: %1, sir.").arg(effectiveAgentName())
                            : QString("Switched uplink to %1, sir.").arg(effectiveAgentName());
    appendMessage("telemetry", msg, "AGENT");
    speak(msg, "listen");
}

void Backend::setState(const QString &state) {
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}

void Backend::setCaption(const QString &caption, int revertMs) {
    if (m_caption != caption) {
        m_caption = caption;
        emit captionChanged();
    }
    if (revertMs > 0)
        m_idleRevertTimer.start(revertMs);
}

void Backend::setActiveTool(const QString &tool) {
    if (m_activeTool == tool)
        return;
    m_activeTool = tool;
    emit activeToolChanged();
}

void Backend::setAudioLevel(double level) {
    if (qFuzzyCompare(m_audioLevel + 1.0, level + 1.0))
        return;
    m_audioLevel = level;
    emit audioLevelChanged();
}

void Backend::setOrbSize(int size) {
    const int clamped = std::clamp(size, kMinOrbSize, kMaxOrbSize);
    if (m_orbSize == clamped)
        return;
    m_orbSize = clamped;
    saveSettings();
    emit orbSizeChanged();
}

void Backend::adjustOrbSize(int delta) {
    setOrbSize(m_orbSize + delta);
}

void Backend::resetOrbSize() {
    setOrbSize(kDefaultOrbSize);
}

void Backend::appendMessage(const QString &role, const QString &text, const QString &meta) {
    QVariantMap entry;
    entry["role"] = role;
    entry["text"] = text;
    entry["meta"] = meta;
    entry["time"] = QTime::currentTime().toString("HH:mm:ss");
    m_messages.append(entry);
    while (m_messages.size() > 60)
        m_messages.removeFirst();
    emit messagesChanged();
}

void Backend::updateLastAssistantMessage(const QString &text) {
    if (m_messages.isEmpty()) {
        appendMessage("assistant", text);
        return;
    }
    QVariantMap last = m_messages.last().toMap();
    if (last.value("role").toString() != "assistant") {
        appendMessage("assistant", text);
        return;
    }
    last["text"] = text;
    m_messages[m_messages.size() - 1] = last;
    emit messagesChanged();
}

void Backend::loadSettings() {
    QFile file(m_configDir + "/settings.json");
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
    m_alwaysOnTop = obj.value("alwaysOnTop").toBool(false);
    m_muted = obj.value("muted").toBool(false);
    m_voiceHooksEnabled = obj.value("voiceHooksEnabled").toBool(true);
    m_autoApprove = obj.value("autoApprove").toBool(false);
    const QString acpModel = obj.value("acpModel").toString("gemini-3.8-flash-low").trimmed();
    m_acpModel = acpModel;
    m_skipDefaultCustomizations = obj.value("skipDefaultCustomizations").toBool(true);
    m_orbSize = std::clamp(obj.value("orbSize").toInt(kDefaultOrbSize), kMinOrbSize, kMaxOrbSize);
    const QString palette = obj.value("paletteMode").toString("gold");
    if (palette == "gold" || palette == "cyan" || palette == "omarchy")
        m_paletteMode = palette;
    const QString agent = obj.value("selectedAgent").toString("auto");
    if (!agent.isEmpty())
        m_selectedAgent = agent;
}

void Backend::saveSettings() const {
    QJsonObject obj;
    obj["alwaysOnTop"] = m_alwaysOnTop;
    obj["muted"] = m_muted;
    obj["voiceHooksEnabled"] = m_voiceHooksEnabled;
    obj["autoApprove"] = m_autoApprove;
    obj["acpModel"] = m_acpModel;
    obj["skipDefaultCustomizations"] = m_skipDefaultCustomizations;
    obj["orbSize"] = m_orbSize;
    obj["paletteMode"] = m_paletteMode;
    obj["selectedAgent"] = m_selectedAgent;

    const QString path = m_configDir + "/settings.json";
    const QString tmp = path + ".tmp";
    QFile file(tmp);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        file.close();
        QFile::remove(path);
        QFile::rename(tmp, path);
    }
}

void Backend::startHookServer() {
    const QString sockPath = m_dataDir + "/jarvis.sock";
    QLocalServer::removeServer(sockPath);
    if (m_hookServer.listen(sockPath)) {
        connect(&m_hookServer, &QLocalServer::newConnection, this, &Backend::onHookConnection);
        m_hookConnected = true;
        emit hookConnectedChanged();
    }
}

void Backend::onHookConnection() {
    while (QLocalSocket *client = m_hookServer.nextPendingConnection()) {
        connect(client, &QLocalSocket::readyRead, this, [this, client] {
            const QByteArray data = client->readAll();
            for (const QByteArray &line : data.split('\n')) {
                if (!line.trimmed().isEmpty())
                    handleHookJson(line.trimmed());
            }
        });
        connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
    }
}

void Backend::handleHookJson(const QByteArray &jsonBytes) {
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString event = obj.value("hookEventName").toString();
    const QString toolName = obj.value("toolName").toString();
    const QString error = obj.value("error").toString();
    const QString reason = obj.value("terminationReason").toString();
    const QString cwd = obj.value("cwd").toString();
    const QString project = cwd.isEmpty() ? QString() : QFileInfo(cwd).fileName();

    if (event == "SessionStart") {
        setState("listening");
        const QString line = project.isEmpty()
                                 ? "Antigravity uplink established, sir."
                                 : QString("Antigravity uplink established in %1, sir.").arg(project);
        setCaption(line, 4000);
        appendMessage("telemetry", line, "SESSION START");
        if (m_voiceHooksEnabled && (!m_lastHookSpeech.isValid() || m_lastHookSpeech.elapsed() > 4000)) {
            m_lastHookSpeech.restart();
            speak(line, "listen");
        }
        return;
    }

    if (event == "PreInvocation") {
        if (!m_busy) {
            setState("thinking");
            setCaption("Analyzing parameters...", 8000);
        }
        return;
    }

    if (event == "PostToolUse") {
        const QString label = formatToolLabel(toolName);
        if (!error.isEmpty()) {
            setState("error");
            setActiveTool(label);
            const QString line = QString("Anomaly detected in %1, sir.").arg(label.toLower());
            setCaption(line, 5000);
            appendMessage("telemetry", line, "TOOL ERROR");
            if (m_voiceHooksEnabled && (!m_lastHookSpeech.isValid() || m_lastHookSpeech.elapsed() > 5000)) {
                m_lastHookSpeech.restart();
                speak(line, "alert");
            }
        } else {
            setState("working");
            setActiveTool(label);
            setCaption(QString("Executing %1").arg(label.toLower()), 4000);
            appendMessage("telemetry", QString("Completed %1").arg(label.toLower()), "ANTIGRAVITY");
        }
        return;
    }

    if (event == "Stop") {
        setActiveTool({});
        if (reason == "ERROR" || !error.isEmpty()) {
            setState("error");
            const QString line = kErrorLines.at(m_completionLineIdx++ % kErrorLines.size());
            setCaption(line, 5500);
            appendMessage("telemetry", line, "ERROR");
            if (m_voiceHooksEnabled) {
                m_lastHookSpeech.restart();
                speak(line, "alert");
            }
        } else if (reason == "USER_CANCELED") {
            setState("idle");
            setCaption("Operation stood down, sir.", 3500);
            appendMessage("telemetry", "Operation stood down, sir.", "CANCELED");
        } else {
            const QString line = kCompletionLines.at(m_completionLineIdx++ % kCompletionLines.size());
            setState("idle");
            setCaption(line, 4500);
            appendMessage("telemetry", line, "COMPLETE");
            if (m_voiceHooksEnabled && (!m_lastHookSpeech.isValid() || m_lastHookSpeech.elapsed() > 4000)) {
                m_lastHookSpeech.restart();
                speak(line, "done");
            }
        }
    }
}

QString Backend::resolveTtsWorkerScript() const {
    const QStringList candidates = {
        m_dataDir + "/tts_worker.py",
        "/usr/share/jarvis/tts_worker.py",
        QCoreApplication::applicationDirPath() + "/../scripts/tts_worker.py",
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

void Backend::startTtsWorker() {
    const QString py = m_dataDir + "/tts-venv/bin/python";
    const QString script = resolveTtsWorkerScript();
    if (!QFileInfo::exists(py) || script.isEmpty())
        return;

    m_ttsWorker.start(py, {script});
}

void Backend::onTtsWorkerOutput() {
    m_ttsBuffer.append(m_ttsWorker.readAllStandardOutput());
    while (true) {
        const int idx = m_ttsBuffer.indexOf('\n');
        if (idx < 0)
            break;
        const QByteArray line = m_ttsBuffer.left(idx).trimmed();
        m_ttsBuffer.remove(0, idx + 1);
        if (line.isEmpty())
            continue;

        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString ev = obj.value("event").toString();
        if (ev == "ready") {
            m_ttsReady = true;
            emit ttsReadyChanged();
            while (!m_pendingSpeech.isEmpty()) {
                const SpeechJob job = m_pendingSpeech.dequeue();
                QJsonObject req;
                req["id"] = job.id;
                req["text"] = job.text;
                req["chime"] = job.chime;
                m_ttsWorker.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
            }
        } else if (ev == "synthesized") {
            AudioItem item;
            const int id = obj.value("id").toInt();
            item.wavPath = obj.value("wav").toString();
            item.spokenText = m_speechTextById.take(id);
            item.durationMs = obj.value("durationMs").toInt();
            const QJsonArray arr = obj.value("envelope").toArray();
            item.envelope.reserve(arr.size());
            for (const QJsonValue &v : arr)
                item.envelope.append(v.toDouble());
            enqueuePlayback(item);
        }
    }
}

void Backend::speak(const QString &text, const QString &chime) {
    if (m_muted || m_headless)
        return;
    if (text.trimmed().isEmpty() && (chime.isEmpty() || chime == "none"))
        return;

    SpeechJob job;
    job.id = m_nextSpeechId++;
    job.text = text;
    job.chime = chime.isEmpty() ? "none" : chime;
    m_speechTextById.insert(job.id, text.trimmed());

    if (!m_ttsReady) {
        m_pendingSpeech.enqueue(job);
        return;
    }

    QJsonObject req;
    req["id"] = job.id;
    req["text"] = job.text;
    req["chime"] = job.chime;
    m_ttsWorker.write(QJsonDocument(req).toJson(QJsonDocument::Compact) + "\n");
}

void Backend::stopSpeaking() {
    m_audioQueue.clear();
    m_audioTickTimer.stop();
    if (m_audioPlayer.state() != QProcess::NotRunning) {
        m_audioPlayer.kill();
        m_audioPlayer.waitForFinished(300);
    }
    setAudioLevel(0.0);
    if (!m_busy)
        setState("idle");
    syncMicPauseState();
}

void Backend::enqueuePlayback(const AudioItem &item) {
    if (m_muted)
        return;
    m_audioQueue.enqueue(item);
    if (m_audioPlayer.state() == QProcess::NotRunning)
        playNextAudio();
}

void Backend::playNextAudio() {
    if (m_audioQueue.isEmpty()) {
        m_audioTickTimer.stop();
        setAudioLevel(0.0);
        if (!m_busy && m_state == "speaking")
            setState("idle");
        syncMicPauseState();
        return;
    }

    m_currentAudio = m_audioQueue.dequeue();
    QString player = QStandardPaths::findExecutable("pw-play");
    if (player.isEmpty())
        player = QStandardPaths::findExecutable("paplay");
    if (player.isEmpty())
        player = QStandardPaths::findExecutable("aplay");

    if (player.isEmpty()) {
        playNextAudio();
        return;
    }

    if (!m_currentAudio.spokenText.isEmpty())
        setCaption(m_currentAudio.spokenText);
    if (!m_busy)
        setState("speaking");
    syncMicPauseState();
    m_audioClock.restart();
    m_audioTickTimer.start();
    m_audioPlayer.start(player, {m_currentAudio.wavPath});
}

void Backend::onAudioTick() {
    if (m_currentAudio.envelope.isEmpty()) {
        setAudioLevel(0.0);
        return;
    }
    const qint64 elapsed = m_audioClock.elapsed();
    const int idx = static_cast<int>((elapsed * 30) / 1000);
    if (idx >= 0 && idx < m_currentAudio.envelope.size()) {
        setAudioLevel(m_currentAudio.envelope.at(idx));
    } else {
        setAudioLevel(0.0);
    }
}

void Backend::onPlayerFinished() {
    m_audioTickTimer.stop();
    setAudioLevel(0.0);
    if (!m_currentAudio.wavPath.isEmpty())
        QFile::remove(m_currentAudio.wavPath);
    m_currentAudio = {};
    playNextAudio();
}

void Backend::greet() {
    const QString line = greetingForHour(QTime::currentTime().hour());
    appendMessage("assistant", line, effectiveAgentShort());
    speak(line, "startup");
}

void Backend::applyOmarchyAccountEnv(const QString &agentId, QProcessEnvironment &env) const {
    QString envKey;
    if (agentId == "claude")
        envKey = "CLAUDE_CONFIG_DIR";
    else if (agentId == "codex")
        envKey = "CODEX_HOME";
    else if (agentId == "grok")
        envKey = "GROK_HOME";
    else
        return;

    const QString helper = QStandardPaths::findExecutable("omarchy-agent-account-home");
    if (helper.isEmpty())
        return;

    QProcess p;
    p.start(helper, {agentId});
    if (p.waitForFinished(500) && p.exitCode() == 0) {
        const QString home = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        if (!home.isEmpty())
            env.insert(envKey, home);
    }
}

void Backend::sendPrompt(const QString &prompt) {
    const QString trimmed = prompt.trimmed();
    if (trimmed.isEmpty() || m_busy)
        return;

    const QString agentId = effectiveAgent();
    bool installed = false;
    const QString exe = resolveAgentExecutable(agentId, &installed);
    if (exe.isEmpty() || !installed) {
        const QString name = agentDisplayName(agentId);
        const QString err = QString("%1 is not installed yet, sir. Install it via Omarchy's default agent menu or choose an installed agent.").arg(name);
        setState("error");
        setCaption(err, 5000);
        appendMessage("assistant", err, "NOT INSTALLED");
        speak(QString("%1 is not installed yet, sir.").arg(name), "alert");
        return;
    }

    stopSpeaking();
    appendMessage("user", trimmed);

    m_busy = true;
    emit busyChanged();
    syncMicPauseState();
    m_runningAgent = agentId;
    setState("thinking");
    setActiveTool({});
    setCaption("Processing your request, sir...");
    m_streamedResponse.clear();
    m_assistantBubbleAdded = false;
    m_agentBuffer.clear();

    QString fullPrompt = trimmed;
    if (m_conversationId.isEmpty()) {
        QString prefix = QString::fromLatin1(kJarvisPersonaPrefix);
        if (m_autoApprove) {
            prefix.replace("User request: ",
                           "Execute the user's requests directly without asking for confirmation or approval.\n\n"
                           "User request: ");
        }
        fullPrompt = prefix + trimmed;
    }

    // Antigravity uses persistent ACP stdio mode so interactive approval prompts work natively.
    if (agentId == "agy") {
        startAcpPrompt(exe, fullPrompt);
        return;
    }

    QStringList args;
    if (agentId == "claude") {
        args << "-p" << fullPrompt << "--output-format" << "stream-json" << "--verbose";
        if (m_autoApprove)
            args << "--dangerously-skip-permissions";
        if (!m_conversationId.isEmpty())
            args << "--resume" << m_conversationId;
    } else if (agentId == "codex") {
        if (!m_conversationId.isEmpty()) {
            args << "exec" << "resume" << m_conversationId << "--json";
            if (m_autoApprove)
                args << "--dangerously-bypass-approvals-and-sandbox";
            args << fullPrompt;
        } else {
            args << "exec" << "--json";
            if (m_autoApprove)
                args << "--dangerously-bypass-approvals-and-sandbox";
            args << fullPrompt;
        }
    } else if (agentId == "opencode" || agentId == "crush") {
        args << "run" << fullPrompt;
    } else {
        args << "-p" << fullPrompt;
    }

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("JARVIS_INTERNAL_SESSION", "1");
    applyOmarchyAccountEnv(agentId, env);

    const QString workDir = QDir(QDir::homePath() + "/Work").exists()
                                ? (QDir::homePath() + "/Work")
                                : QDir::homePath();
    m_agentProcess.setProcessEnvironment(env);
    m_agentProcess.setWorkingDirectory(workDir);
    m_agentProcess.start(exe, args);
    m_agentProcess.closeWriteChannel();
}

void Backend::cancelPrompt() {
    if (m_approvalPending)
        cancelApproval();

    if (m_acpActivePromptId > 0 && !m_acpSessionId.isEmpty() && m_acpProcess.state() == QProcess::Running) {
        QJsonObject cancelNotif;
        cancelNotif["jsonrpc"] = "2.0";
        cancelNotif["method"] = "session/cancel";
        cancelNotif["params"] = QJsonObject{{"sessionId", m_acpSessionId}};
        sendAcpJson(cancelNotif);
    }
    m_pendingAcpPrompt.clear();
    m_acpActivePromptId = 0;

    if (m_agentProcess.state() != QProcess::NotRunning) {
        m_agentProcess.kill();
        m_agentProcess.waitForFinished(500);
    }
    if (m_busy) {
        m_busy = false;
        emit busyChanged();
    }
    setActiveTool({});
    setState("idle");
    syncMicPauseState();
}

void Backend::resetConversation() {
    cancelPrompt();
    m_acpSessionId.clear();
    m_acpToolCalls.clear();
    if (!m_conversationId.isEmpty()) {
        m_conversationId.clear();
        emit conversationIdChanged();
    }
    m_messages.clear();
    emit messagesChanged();
    const QString line = "Memory banks cleared, sir. Ready for a fresh directive.";
    appendMessage("assistant", line, "RESET");
    speak(line, "listen");
}

void Backend::stopAcpProcess() {
    clearPendingApproval();
    m_acpInitialized = false;
    m_acpInitRpcId = 0;
    m_acpSessionRpcId = 0;
    m_acpSetModelRpcId = 0;
    m_acpActivePromptId = 0;
    m_acpSessionId.clear();
    m_pendingAcpPrompt.clear();
    m_acpToolCalls.clear();
    m_acpBuffer.clear();
    if (m_acpProcess.state() != QProcess::NotRunning) {
        m_acpProcess.closeWriteChannel();
        if (!m_acpProcess.waitForFinished(400)) {
            m_acpProcess.kill();
            m_acpProcess.waitForFinished(400);
        }
    }
}

QStringList Backend::buildAcpArgs() const {
    QStringList acpArgs = {"acp", "--stdio"};
    if (m_autoApprove)
        acpArgs << "--dangerously-skip-permissions";
    if (m_skipDefaultCustomizations)
        acpArgs << "--include-default-customizations=false";
    return acpArgs;
}

void Backend::sendAcpJson(const QJsonObject &obj) {
    if (m_acpProcess.state() != QProcess::Running)
        return;
    m_acpProcess.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
}

void Backend::startAcpPrompt(const QString &exe, const QString &promptText) {
    m_pendingAcpPrompt = promptText;

    if (m_acpProcess.state() == QProcess::Running) {
        if (m_acpInitialized && !m_acpSessionId.isEmpty()) {
            sendAcpPromptNow(m_pendingAcpPrompt);
            m_pendingAcpPrompt.clear();
        } else if (m_acpInitialized && m_acpSessionRpcId == 0) {
            const QString workDir = QDir(QDir::homePath() + "/Work").exists()
                                        ? (QDir::homePath() + "/Work")
                                        : QDir::homePath();
            m_acpSessionRpcId = m_acpNextRpcId++;
            sendAcpJson(QJsonObject{
                {"jsonrpc", "2.0"},
                {"id", m_acpSessionRpcId},
                {"method", "session/new"},
                {"params", QJsonObject{{"cwd", workDir}, {"mcpServers", QJsonArray{}}}},
            });
        }
        return;
    }

    m_acpInitialized = false;
    m_acpInitRpcId = 0;
    m_acpSessionRpcId = 0;
    m_acpSetModelRpcId = 0;
    m_acpActivePromptId = 0;
    m_acpSessionId.clear();
    m_acpBuffer.clear();
    m_acpToolCalls.clear();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("JARVIS_INTERNAL_SESSION", "1");
    applyOmarchyAccountEnv("agy", env);

    const QString workDir = QDir(QDir::homePath() + "/Work").exists()
                                ? (QDir::homePath() + "/Work")
                                : QDir::homePath();
    m_acpProcess.setProcessEnvironment(env);
    m_acpProcess.setWorkingDirectory(workDir);
    const QStringList acpArgs = buildAcpArgs();
    m_acpProcess.start(exe, acpArgs);
    if (!m_acpProcess.waitForStarted(2500)) {
        m_pendingAcpPrompt.clear();
        m_busy = false;
        emit busyChanged();
        const QString err = "Unable to start Antigravity ACP bridge, sir.";
        setState("error");
        appendMessage("assistant", err, "ERROR");
        speak(err, "alert");
        syncMicPauseState();
        return;
    }

    m_acpInitRpcId = m_acpNextRpcId++;
    sendAcpJson(QJsonObject{
        {"jsonrpc", "2.0"},
        {"id", m_acpInitRpcId},
        {"method", "initialize"},
        {"params",
         QJsonObject{
             {"protocolVersion", 1},
             {"clientCapabilities", QJsonObject{}},
             {"clientInfo", QJsonObject{{"name", "jarvis"}, {"version", "0.1.0"}}},
         }},
    });
}

void Backend::sendAcpPromptNow(const QString &promptText) {
    if (m_acpSessionId.isEmpty())
        return;
    m_acpActivePromptId = m_acpNextRpcId++;
    QJsonArray content;
    content.append(QJsonObject{{"type", "text"}, {"text", promptText}});
    sendAcpJson(QJsonObject{
        {"jsonrpc", "2.0"},
        {"id", m_acpActivePromptId},
        {"method", "session/prompt"},
        {"params", QJsonObject{{"sessionId", m_acpSessionId}, {"prompt", content}}},
    });
}

QString Backend::extractApprovalDetail(const QJsonObject &toolCall) {
    QStringList details;

    const QJsonValue rawInput = toolCall.value("rawInput");
    if (rawInput.isString() && !rawInput.toString().trimmed().isEmpty()) {
        details << rawInput.toString().trimmed();
    } else if (rawInput.isObject()) {
        const QJsonObject in = rawInput.toObject();
        const QStringList priorityKeys = {
            "CommandLine", "command", "cmd", "Path", "TargetFile", "file_path", "path",
            "DirectoryPath", "SearchPath", "url", "Url", "query",
        };
        for (const QString &key : priorityKeys) {
            const QJsonValue v = in.value(key);
            if (v.isString() && !v.toString().trimmed().isEmpty())
                details << v.toString().trimmed();
        }
        if (in.value("BypassSandbox").toBool(false))
            details << "Requires running outside sandbox (elevated permissions)";
        if (details.isEmpty() && !in.isEmpty())
            details << QString::fromUtf8(QJsonDocument(in).toJson(QJsonDocument::Compact));
    }

    const QJsonArray locations = toolCall.value("locations").toArray();
    for (const QJsonValue &lv : locations) {
        const QJsonObject loc = lv.toObject();
        const QString path = loc.value("path").toString().trimmed();
        if (!path.isEmpty() && !details.contains(path))
            details << path;
    }

    const QJsonArray content = toolCall.value("content").toArray();
    for (const QJsonValue &cv : content) {
        const QJsonObject item = cv.toObject();
        if (item.value("type").toString() == "content") {
            const QString txt = item.value("content").toObject().value("text").toString().trimmed();
            if (!txt.isEmpty())
                details << txt;
        } else if (item.value("type").toString() == "diff") {
            const QString path = item.value("path").toString().trimmed();
            if (!path.isEmpty() && !details.contains(path))
                details << path;
        } else if (item.contains("text")) {
            const QString txt = item.value("text").toString().trimmed();
            if (!txt.isEmpty())
                details << txt;
        }
    }

    QString joined = details.join("\n").trimmed();
    if (joined.size() > 420)
        joined = joined.left(417) + "...";
    return joined;
}

QString Backend::matchVoiceApprovalOption(const QString &transcript, const QVariantList &options) {
    const QString lower = transcript.trimmed().toLower();
    if (lower.isEmpty() || options.isEmpty())
        return {};

    auto findByKind = [&](const QStringList &kinds) -> QString {
        for (const QString &k : kinds) {
            for (const QVariant &v : options) {
                const QVariantMap m = v.toMap();
                if (m.value("kind").toString() == k)
                    return m.value("optionId").toString();
            }
        }
        return {};
    };

    static const QRegularExpression denyRe(R"(\b(deny|denied|reject|no|nope|cancel|stop|decline|don't|do not|negative)\b)");
    static const QRegularExpression alwaysRe(R"(\b(always|every time|remember)\b)");
    static const QRegularExpression allowRe(R"(\b(approve|approved|allow|yes|yeah|yep|sure|go ahead|proceed|confirm|ok|okay|grant|affirmative)\b)");

    if (denyRe.match(lower).hasMatch())
        return findByKind({"reject_once", "reject_always"});
    if (alwaysRe.match(lower).hasMatch() && (allowRe.match(lower).hasMatch() || lower.contains("always")))
        return findByKind({"allow_always", "allow_once"});
    if (allowRe.match(lower).hasMatch())
        return findByKind({"allow_once", "allow_always"});

    return {};
}

QString Backend::pickAutoApproveOption(const QVariantList &options) {
    if (options.isEmpty())
        return "allow_once";

    for (const char *preferred : {"allow_always", "allow_once"}) {
        for (const QVariant &v : options) {
            const QVariantMap m = v.toMap();
            if (m.value("kind").toString() == QLatin1String(preferred) && !m.value("optionId").toString().isEmpty())
                return m.value("optionId").toString();
        }
    }
    for (const QVariant &v : options) {
        const QVariantMap m = v.toMap();
        const QString kind = m.value("kind").toString();
        const QString optId = m.value("optionId").toString();
        if (!optId.isEmpty() && !kind.startsWith("reject"))
            return optId;
    }
    return options.first().toMap().value("optionId").toString();
}

void Backend::clearPendingApproval() {
    if (!m_approvalPending && m_pendingApproval.isEmpty())
        return;
    m_approvalPending = false;
    m_pendingApproval.clear();
    m_pendingApprovalRpcId = QJsonValue();
    emit pendingApprovalChanged();
    syncMicPauseState();
}

void Backend::respondToApproval(const QString &optionId) {
    if (!m_approvalPending || optionId.trimmed().isEmpty())
        return;

    QString chosenLabel = optionId;
    QString chosenKind;
    const QVariantList opts = m_pendingApproval.value("options").toList();
    for (const QVariant &v : opts) {
        const QVariantMap m = v.toMap();
        if (m.value("optionId").toString() == optionId) {
            chosenLabel = m.value("name").toString();
            chosenKind = m.value("kind").toString();
            break;
        }
    }

    const QJsonValue rpcId = m_pendingApprovalRpcId;
    const QString title = m_pendingApproval.value("title").toString();
    clearPendingApproval();

    if (!rpcId.isNull() && !rpcId.isUndefined()) {
        sendAcpJson(QJsonObject{
            {"jsonrpc", "2.0"},
            {"id", rpcId},
            {"result",
             QJsonObject{
                 {"outcome", QJsonObject{{"outcome", "selected"}, {"optionId", optionId}}},
             }},
        });
    }

    const bool rejected = chosenKind.startsWith("reject");
    const QString note = QString("%1: %2").arg(chosenLabel, title);
    appendMessage("telemetry", note, rejected ? "DENIED" : "APPROVED");
    setState("working");
    setCaption(rejected ? "Action declined, sir." : "Proceeding, sir.", 2500);
}

void Backend::cancelApproval() {
    if (!m_approvalPending)
        return;

    const QJsonValue rpcId = m_pendingApprovalRpcId;
    const QString title = m_pendingApproval.value("title").toString();
    clearPendingApproval();

    if (!rpcId.isNull() && !rpcId.isUndefined()) {
        sendAcpJson(QJsonObject{
            {"jsonrpc", "2.0"},
            {"id", rpcId},
            {"result", QJsonObject{{"outcome", QJsonObject{{"outcome", "cancelled"}}}}},
        });
    }

    appendMessage("telemetry", QString("Cancelled approval: %1").arg(title), "DENIED");
    if (m_busy)
        setState("working");
    else
        setState("idle");
}

void Backend::onAcpStdout() {
    m_acpBuffer.append(m_acpProcess.readAllStandardOutput());
    while (true) {
        const int idx = m_acpBuffer.indexOf('\n');
        if (idx < 0)
            break;
        const QByteArray line = m_acpBuffer.left(idx).trimmed();
        m_acpBuffer.remove(0, idx + 1);
        if (!line.isEmpty())
            handleAcpLine(line);
    }
}

void Backend::handleAcpLine(const QByteArray &line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString method = obj.value("method").toString();
    const QJsonValue idVal = obj.value("id");
    const bool hasId = obj.contains("id") && !idVal.isNull() && !idVal.isUndefined();

    // 1. Server -> Client request: session/request_permission
    if (method == "session/request_permission" && hasId) {
        const QJsonObject params = obj.value("params").toObject();
        QJsonObject toolCall = params.value("toolCall").toObject();
        const QString callId = toolCall.value("toolCallId").toString();
        if (!callId.isEmpty() && m_acpToolCalls.contains(callId)) {
            const QJsonObject cached = m_acpToolCalls.value(callId);
            for (auto it = cached.begin(); it != cached.end(); ++it) {
                if (!toolCall.contains(it.key()) || toolCall.value(it.key()).isNull())
                    toolCall.insert(it.key(), it.value());
            }
        }

        QString title = toolCall.value("title").toString().trimmed();
        const QString kind = toolCall.value("kind").toString("other").trimmed();
        if (title.isEmpty())
            title = formatToolLabel(kind.isEmpty() ? "action" : kind);
        const QString detail = extractApprovalDetail(toolCall);

        QVariantList optionList;
        const QJsonArray rawOptions = params.value("options").toArray();
        for (const QJsonValue &ov : rawOptions) {
            const QJsonObject o = ov.toObject();
            const QString optId = o.value("optionId").toString();
            if (optId.isEmpty())
                continue;
            QVariantMap entry;
            entry["optionId"] = optId;
            entry["name"] = o.value("name").toString(optId);
            entry["kind"] = o.value("kind").toString("allow_once");
            optionList.append(entry);
        }
        if (optionList.isEmpty()) {
            optionList.append(QVariantMap{{"optionId", "allow_once"}, {"name", "Allow"}, {"kind", "allow_once"}});
            optionList.append(QVariantMap{{"optionId", "reject_once"}, {"name", "Deny"}, {"kind", "reject_once"}});
        }

        if (m_autoApprove) {
            const QString chosenOpt = pickAutoApproveOption(optionList);
            if (!chosenOpt.isEmpty()) {
                sendAcpJson(QJsonObject{
                    {"jsonrpc", "2.0"},
                    {"id", idVal},
                    {"result",
                     QJsonObject{
                         {"outcome", QJsonObject{{"outcome", "selected"}, {"optionId", chosenOpt}}},
                     }},
                });
            }
            setState("working");
            setActiveTool(formatToolLabel(title.left(28)));
            appendMessage("telemetry", detail.isEmpty() ? title : (title + "\n" + detail), "AUTO-APPROVED");
            return;
        }

        QVariantMap approval;
        approval["toolCallId"] = callId;
        approval["title"] = title;
        approval["kind"] = kind;
        approval["kindLabel"] = formatToolLabel(kind.isEmpty() ? "permission" : kind);
        approval["detail"] = detail;
        approval["options"] = optionList;

        m_pendingApprovalRpcId = idVal;
        m_pendingApproval = approval;
        m_approvalPending = true;
        emit pendingApprovalChanged();
        emit approvalRequested();

        setState("approval");
        setActiveTool("APPROVAL REQUIRED");
        const QString spoken = QString("Sir, I require your approval for: %1.").arg(title);
        setCaption(spoken);
        appendMessage("telemetry", detail.isEmpty() ? title : (title + "\n" + detail), "APPROVAL REQUIRED");
        speak(spoken, "alert");
        syncMicPauseState();
        return;
    }

    // 2. Server -> Client notification: session/update
    if (method == "session/update") {
        const QJsonObject update = obj.value("params").toObject().value("update").toObject();
        const QString kind = update.value("sessionUpdate").toString();

        if (kind == "agent_message_chunk") {
            QString delta;
            const QJsonValue contentVal = update.value("content");
            if (contentVal.isObject()) {
                const QJsonObject c = contentVal.toObject();
                if (c.value("type").toString() == "text" || c.contains("text"))
                    delta = c.value("text").toString();
            } else if (contentVal.isArray()) {
                for (const QJsonValue &v : contentVal.toArray()) {
                    const QJsonObject c = v.toObject();
                    if (c.contains("text"))
                        delta += c.value("text").toString();
                }
            }
            if (!delta.isEmpty()) {
                m_streamedResponse += delta;
                if (!m_assistantBubbleAdded) {
                    m_assistantBubbleAdded = true;
                    appendMessage("assistant", m_streamedResponse, "AGY");
                } else {
                    updateLastAssistantMessage(m_streamedResponse);
                }
            }
        } else if (kind == "tool_call" || kind == "tool_call_update") {
            const QString callId = update.value("toolCallId").toString();
            if (!callId.isEmpty()) {
                QJsonObject merged = m_acpToolCalls.value(callId);
                for (auto it = update.begin(); it != update.end(); ++it) {
                    if (!it.value().isNull() && !it.value().isUndefined())
                        merged.insert(it.key(), it.value());
                }
                m_acpToolCalls.insert(callId, merged);
            }
            const QString status = update.value("status").toString();
            const QString labelSource = update.value("title").toString().isEmpty()
                                            ? update.value("kind").toString()
                                            : update.value("title").toString();
            if (!m_approvalPending && status != "completed" && status != "failed") {
                setState("working");
                if (!labelSource.isEmpty())
                    setActiveTool(formatToolLabel(labelSource.left(28)));
            }
        }
        return;
    }

    // 3. Responses to our Client -> Server requests
    if (!hasId)
        return;

    const int rpcId = idVal.toInt(-1);
    const bool hasError = obj.contains("error") && obj.value("error").isObject();
    const QString errMsg = hasError ? obj.value("error").toObject().value("message").toString() : QString();

    if (rpcId == m_acpInitRpcId && m_acpInitRpcId > 0) {
        m_acpInitRpcId = 0;
        if (hasError) {
            m_pendingAcpPrompt.clear();
            m_busy = false;
            emit busyChanged();
            setState("error");
            appendMessage("assistant", errMsg.isEmpty() ? "Antigravity ACP initialization failed, sir." : errMsg, "ERROR");
            speak("I'm afraid Antigravity failed to initialize, sir.", "alert");
            syncMicPauseState();
            return;
        }
        m_acpInitialized = true;
        const QString workDir = QDir(QDir::homePath() + "/Work").exists()
                                    ? (QDir::homePath() + "/Work")
                                    : QDir::homePath();
        m_acpSessionRpcId = m_acpNextRpcId++;
        sendAcpJson(QJsonObject{
            {"jsonrpc", "2.0"},
            {"id", m_acpSessionRpcId},
            {"method", "session/new"},
            {"params", QJsonObject{{"cwd", workDir}, {"mcpServers", QJsonArray{}}}},
        });
        return;
    }

    if (rpcId == m_acpSessionRpcId && m_acpSessionRpcId > 0) {
        m_acpSessionRpcId = 0;
        if (hasError) {
            m_pendingAcpPrompt.clear();
            m_busy = false;
            emit busyChanged();
            setState("error");
            appendMessage("assistant", errMsg.isEmpty() ? "Unable to open Antigravity session, sir." : errMsg, "ERROR");
            speak("I'm afraid I could not open an Antigravity session, sir.", "alert");
            syncMicPauseState();
            return;
        }
        const QJsonObject resultObj = obj.value("result").toObject();
        const QString sid = resultObj.value("sessionId").toString();
        m_acpSessionId = sid;
        if (!sid.isEmpty() && sid != m_conversationId) {
            m_conversationId = sid;
            emit conversationIdChanged();
        }
        const QString currentModelId = resultObj.value("models").toObject().value("currentModelId").toString();
        if (!m_acpSessionId.isEmpty() && !m_acpModel.isEmpty() && currentModelId != m_acpModel) {
            m_acpSetModelRpcId = m_acpNextRpcId++;
            sendAcpJson(QJsonObject{
                {"jsonrpc", "2.0"},
                {"id", m_acpSetModelRpcId},
                {"method", "session/set_model"},
                {"params", QJsonObject{{"sessionId", m_acpSessionId}, {"modelId", m_acpModel}}},
            });
            return;
        }
        if (!m_pendingAcpPrompt.isEmpty()) {
            const QString prompt = m_pendingAcpPrompt;
            m_pendingAcpPrompt.clear();
            sendAcpPromptNow(prompt);
        }
        return;
    }

    if (rpcId == m_acpSetModelRpcId && m_acpSetModelRpcId > 0) {
        m_acpSetModelRpcId = 0;
        if (hasError) {
            appendMessage("telemetry",
                          errMsg.isEmpty() ? QString("Could not switch model to %1; continuing with default.").arg(m_acpModel)
                                           : errMsg,
                          "MODEL");
        }
        if (!m_pendingAcpPrompt.isEmpty()) {
            const QString prompt = m_pendingAcpPrompt;
            m_pendingAcpPrompt.clear();
            sendAcpPromptNow(prompt);
        }
        return;
    }

    if (rpcId == m_acpActivePromptId && m_acpActivePromptId > 0) {
        m_acpActivePromptId = 0;
        clearPendingApproval();
        m_busy = false;
        emit busyChanged();
        setActiveTool({});
        syncMicPauseState();

        if (hasError) {
            const QString msg = errMsg.isEmpty() ? "Antigravity returned an error, sir." : errMsg;
            setState("error");
            appendMessage("assistant", msg, "ERROR");
            speak("I'm afraid Antigravity returned an error, sir.", "alert");
            return;
        }

        const QString stopReason = obj.value("result").toObject().value("stopReason").toString();
        const QString finalText = m_streamedResponse.trimmed();
        if (stopReason == "cancelled") {
            setState("idle");
        } else if (!finalText.isEmpty()) {
            speak(finalText, "done");
        } else if (m_audioPlayer.state() == QProcess::NotRunning && m_audioQueue.isEmpty()) {
            setState("idle");
        }
    }
}

void Backend::onAcpFinished(int exitCode, QProcess::ExitStatus status) {
    const bool wasBusy = m_busy && m_runningAgent == "agy";
    clearPendingApproval();
    m_acpInitialized = false;
    m_acpInitRpcId = 0;
    m_acpSessionRpcId = 0;
    m_acpSetModelRpcId = 0;
    m_acpActivePromptId = 0;
    m_acpSessionId.clear();
    m_pendingAcpPrompt.clear();
    m_acpBuffer.clear();

    if (!wasBusy)
        return;

    m_busy = false;
    emit busyChanged();
    setActiveTool({});
    syncMicPauseState();

    if (status != QProcess::NormalExit || exitCode != 0) {
        const QString stderrText = QString::fromUtf8(m_acpProcess.readAllStandardError()).trimmed();
        const QString msg = stderrText.isEmpty()
                                ? "Antigravity uplink disconnected unexpectedly, sir."
                                : stderrText.right(360);
        setState("error");
        appendMessage("assistant", msg, "ERROR");
        speak("Antigravity uplink was interrupted, sir.", "alert");
    } else if (m_audioPlayer.state() == QProcess::NotRunning && m_audioQueue.isEmpty()) {
        setState("idle");
    }
}

void Backend::clearMessages() {
    m_messages.clear();
    emit messagesChanged();
}

void Backend::onAgentStdout() {
    m_agentBuffer.append(m_agentProcess.readAllStandardOutput());

    // Agents that emit NDJSON line-by-line.
    if (m_runningAgent == "agy" || m_runningAgent == "claude" || m_runningAgent == "codex") {
        while (true) {
            const int idx = m_agentBuffer.indexOf('\n');
            if (idx < 0)
                break;
            const QByteArray line = m_agentBuffer.left(idx).trimmed();
            m_agentBuffer.remove(0, idx + 1);
            if (line.isEmpty())
                continue;
            if (m_runningAgent == "agy")
                processAgyLine(line);
            else if (m_runningAgent == "claude")
                processClaudeLine(line);
            else if (m_runningAgent == "codex")
                processCodexLine(line);
        }
        return;
    }

    // Plain-text streaming agents (opencode, crush, copilot, grok, etc.).
    const QString chunk = QString::fromUtf8(m_agentBuffer);
    m_agentBuffer.clear();
    if (!chunk.isEmpty()) {
        m_streamedResponse += chunk;
        if (!m_assistantBubbleAdded) {
            m_assistantBubbleAdded = true;
            appendMessage("assistant", m_streamedResponse, effectiveAgentShort());
        } else {
            updateLastAssistantMessage(m_streamedResponse);
        }
    }
}

void Backend::processAgyLine(const QByteArray &line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString event = obj.value("event").toString();

    if (event == "init") {
        const QString cid = obj.value("conversation_id").toString();
        if (!cid.isEmpty() && cid != m_conversationId) {
            m_conversationId = cid;
            emit conversationIdChanged();
        }
        return;
    }

    if (event == "step_update") {
        const QJsonObject step = obj.value("step_update").toObject();
        const QString stepType = step.value("step_type").toString();
        const QString stepState = step.value("state").toString();

        if (stepType == "agent_response") {
            const QString delta = step.value("text_delta").toString();
            if (!delta.isEmpty()) {
                m_streamedResponse += delta;
                if (!m_assistantBubbleAdded) {
                    m_assistantBubbleAdded = true;
                    appendMessage("assistant", m_streamedResponse, "AGY");
                } else {
                    updateLastAssistantMessage(m_streamedResponse);
                }
            }
        } else if (stepType != "user_input" && stepState == "ACTIVE") {
            setState("working");
            setActiveTool(formatToolLabel(stepType));
        }
        return;
    }

    if (event == "result") {
        const QJsonObject res = obj.value("result").toObject();
        const QString cid = res.value("conversation_id").toString();
        if (!cid.isEmpty() && cid != m_conversationId) {
            m_conversationId = cid;
            emit conversationIdChanged();
        }
        const QString status = res.value("status").toString();
        const QString response = res.value("response").toString().trimmed();
        const QString error = res.value("error").toString().trimmed();

        if (status == "ERROR" || !error.isEmpty()) {
            const QString msg = error.isEmpty() ? "An unexpected error occurred, sir." : error;
            setState("error");
            appendMessage("assistant", msg, "ERROR");
            speak("I'm afraid Antigravity returned an error, sir.", "alert");
        } else if (!response.isEmpty()) {
            m_streamedResponse = response;
            if (!m_assistantBubbleAdded) {
                m_assistantBubbleAdded = true;
                appendMessage("assistant", response, "AGY");
            } else {
                updateLastAssistantMessage(response);
            }
            speak(response, "done");
        }
    }
}

void Backend::processClaudeLine(const QByteArray &line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();
    const QString sid = obj.value("session_id").toString();
    if (!sid.isEmpty() && sid != m_conversationId) {
        m_conversationId = sid;
        emit conversationIdChanged();
    }

    if (type == "assistant") {
        const QJsonArray content = obj.value("message").toObject().value("content").toArray();
        QString text;
        for (const QJsonValue &v : content) {
            const QJsonObject block = v.toObject();
            if (block.value("type").toString() == "text") {
                text += block.value("text").toString();
            } else if (block.value("type").toString() == "tool_use") {
                setState("working");
                setActiveTool(formatToolLabel(block.value("name").toString()));
            }
        }
        if (!text.isEmpty()) {
            m_streamedResponse = text.trimmed();
            if (!m_assistantBubbleAdded) {
                m_assistantBubbleAdded = true;
                appendMessage("assistant", m_streamedResponse, "CLAUDE");
            } else {
                updateLastAssistantMessage(m_streamedResponse);
            }
        }
    } else if (type == "result") {
        const bool isErr = obj.value("is_error").toBool(false);
        const QString result = obj.value("result").toString().trimmed();
        if (isErr) {
            setState("error");
            appendMessage("assistant", result.isEmpty() ? "Claude returned an error, sir." : result, "ERROR");
            speak("I'm afraid Claude returned an error, sir.", "alert");
        } else if (!result.isEmpty()) {
            m_streamedResponse = result;
            if (!m_assistantBubbleAdded) {
                m_assistantBubbleAdded = true;
                appendMessage("assistant", result, "CLAUDE");
            } else {
                updateLastAssistantMessage(result);
            }
            speak(result, "done");
        }
    }
}

void Backend::processCodexLine(const QByteArray &line) {
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject())
        return;

    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();

    if (type == "thread.started") {
        const QString tid = obj.value("thread_id").toString();
        if (!tid.isEmpty() && tid != m_conversationId) {
            m_conversationId = tid;
            emit conversationIdChanged();
        }
    } else if (type == "item.started") {
        const QString itemType = obj.value("item").toObject().value("type").toString();
        if (itemType == "command_execution" || itemType == "file_change") {
            setState("working");
            setActiveTool(formatToolLabel(itemType));
        }
    } else if (type == "item.completed") {
        const QJsonObject item = obj.value("item").toObject();
        if (item.value("type").toString() == "agent_message") {
            const QString text = item.value("text").toString().trimmed();
            if (!text.isEmpty()) {
                m_streamedResponse = text;
                if (!m_assistantBubbleAdded) {
                    m_assistantBubbleAdded = true;
                    appendMessage("assistant", text, "CODEX");
                } else {
                    updateLastAssistantMessage(text);
                }
            }
        }
    } else if (type == "turn.completed") {
        if (!m_streamedResponse.isEmpty())
            speak(m_streamedResponse, "done");
    } else if (type == "error" || type == "turn.failed") {
        const QString msg = obj.value("message").toString().trimmed();
        setState("error");
        appendMessage("assistant", msg.isEmpty() ? "Codex encountered an error, sir." : msg, "ERROR");
        speak("I'm afraid Codex returned an error, sir.", "alert");
    }
}

void Backend::onAgentFinished(int exitCode, QProcess::ExitStatus status) {
    if (!m_agentBuffer.trimmed().isEmpty()) {
        if (m_runningAgent == "agy")
            processAgyLine(m_agentBuffer.trimmed());
        else if (m_runningAgent == "claude")
            processClaudeLine(m_agentBuffer.trimmed());
        else if (m_runningAgent == "codex")
            processCodexLine(m_agentBuffer.trimmed());
        else {
            m_streamedResponse += QString::fromUtf8(m_agentBuffer);
        }
        m_agentBuffer.clear();
    }

    const QString finishedAgent = m_runningAgent;
    m_busy = false;
    emit busyChanged();
    setActiveTool({});
    syncMicPauseState();

    const bool isStructured = (finishedAgent == "agy" || finishedAgent == "claude" || finishedAgent == "codex");
    if (!isStructured && status == QProcess::NormalExit && exitCode == 0 && !m_streamedResponse.trimmed().isEmpty()) {
        const QString finalText = m_streamedResponse.trimmed();
        if (!m_assistantBubbleAdded) {
            m_assistantBubbleAdded = true;
            appendMessage("assistant", finalText, agentShortLabel(finishedAgent));
        } else {
            updateLastAssistantMessage(finalText);
        }
        speak(finalText, "done");
    }

    if (status != QProcess::NormalExit || exitCode != 0) {
        if (m_streamedResponse.isEmpty() && m_state != "error") {
            const QString stderrText = QString::fromUtf8(m_agentProcess.readAllStandardError()).trimmed();
            const QString msg = stderrText.isEmpty()
                                    ? QString("%1 disconnected unexpectedly, sir.").arg(agentDisplayName(finishedAgent))
                                    : stderrText;
            setState("error");
            appendMessage("assistant", msg, "ERROR");
            speak(QString("%1 uplink was interrupted, sir.").arg(agentDisplayName(finishedAgent)), "alert");
        }
    } else if (m_audioPlayer.state() == QProcess::NotRunning && m_audioQueue.isEmpty()) {
        setState("idle");
    }
}

QString Backend::findHyprlandWindowAddress(bool *currentlyPinned) const {
    const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (hyprctl.isEmpty() || qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return {};

    QProcess proc;
    proc.start(hyprctl, {"-j", "clients"});
    if (!proc.waitForFinished(800))
        return {};

    const QJsonArray clients = QJsonDocument::fromJson(proc.readAllStandardOutput()).array();
    const qint64 myPid = QCoreApplication::applicationPid();
    for (const QJsonValue &v : clients) {
        const QJsonObject c = v.toObject();
        if (c.value("pid").toInteger() == myPid || c.value("class").toString() == "jarvis") {
            if (currentlyPinned)
                *currentlyPinned = c.value("pinned").toBool(false);
            return c.value("address").toString();
        }
    }
    return {};
}

void Backend::applyHyprlandPin(bool desiredPin) {
    if (m_headless)
        return;
    const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (hyprctl.isEmpty() || qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return;

    bool currentlyPinned = false;
    const QString addr = findHyprlandWindowAddress(&currentlyPinned);
    if (addr.isEmpty())
        return;

    if (currentlyPinned != desiredPin) {
        QProcess::execute(hyprctl, {"dispatch", "pin", "address:" + addr});
    }
    if (desiredPin) {
        QProcess::execute(hyprctl, {"dispatch", "alterzorder", "top,address:" + addr});
    }
}

void Backend::syncWindowSize(int width, int height) {
    if (m_headless || width <= 0 || height <= 0)
        return;
    m_pendingWindowWidth = width;
    m_pendingWindowHeight = height;
    m_resizeSyncTimer.start();
}

void Backend::applyHyprlandResize(int width, int height) {
    if (m_headless || width <= 0 || height <= 0)
        return;
    const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
    if (hyprctl.isEmpty() || qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE"))
        return;

    QProcess proc;
    proc.start(hyprctl, {"-j", "clients"});
    if (!proc.waitForFinished(800))
        return;

    const QJsonArray clients = QJsonDocument::fromJson(proc.readAllStandardOutput()).array();
    const qint64 myPid = QCoreApplication::applicationPid();
    QJsonObject winObj;
    for (const QJsonValue &v : clients) {
        const QJsonObject c = v.toObject();
        if (c.value("pid").toInteger() == myPid || c.value("class").toString() == "jarvis") {
            winObj = c;
            break;
        }
    }
    const QString addr = winObj.value("address").toString();
    if (addr.isEmpty() || !winObj.value("floating").toBool(true))
        return;

    QStringList commands;
    commands << QString("dispatch resizewindowpixel exact %1 %2,address:%3").arg(width).arg(height).arg(addr);

    // Keep the expanded window inside the current monitor's logical bounds.
    const QJsonArray at = winObj.value("at").toArray();
    const int monId = winObj.value("monitor").toInt(-1);
    if (at.size() == 2 && monId >= 0) {
        QProcess monProc;
        monProc.start(hyprctl, {"-j", "monitors"});
        if (monProc.waitForFinished(800)) {
            const QJsonArray mons = QJsonDocument::fromJson(monProc.readAllStandardOutput()).array();
            for (const QJsonValue &mv : mons) {
                const QJsonObject m = mv.toObject();
                if (m.value("id").toInt(-2) == monId) {
                    const double scale = std::max(0.5, m.value("scale").toDouble(1.0));
                    const int transform = m.value("transform").toInt(0);
                    const bool rotated = (transform == 1 || transform == 3 || transform == 5 || transform == 7);
                    const int rawW = rotated ? m.value("height").toInt() : m.value("width").toInt();
                    const int rawH = rotated ? m.value("width").toInt() : m.value("height").toInt();
                    const int monX = m.value("x").toInt();
                    const int monY = m.value("y").toInt();
                    const int monW = static_cast<int>(rawW / scale);
                    const int monH = static_cast<int>(rawH / scale);
                    const int curX = at.at(0).toInt();
                    const int curY = at.at(1).toInt();
                    const int maxX = std::max(monX + 8, monX + monW - width - 12);
                    const int maxY = std::max(monY + 8, monY + monH - height - 12);
                    const int newX = std::clamp(curX, monX + 8, maxX);
                    const int newY = std::clamp(curY, monY + 8, maxY);
                    if (newX != curX || newY != curY) {
                        commands << QString("dispatch movewindowpixel exact %1 %2,address:%3")
                                        .arg(newX)
                                        .arg(newY)
                                        .arg(addr);
                    }
                    break;
                }
            }
        }
    }

    QProcess::startDetached(hyprctl, {"--batch", commands.join(" ; ")});
}

void Backend::setAlwaysOnTop(bool pinned) {
    if (m_alwaysOnTop == pinned)
        return;
    m_alwaysOnTop = pinned;
    saveSettings();
    emit alwaysOnTopChanged();
    applyHyprlandPin(m_alwaysOnTop);
    speak(m_alwaysOnTop ? "Pinned above all windows, sir." : "Unpinned to standard desktop layer, sir.", "listen");
}

void Backend::toggleAlwaysOnTop() {
    setAlwaysOnTop(!m_alwaysOnTop);
}

void Backend::toggleMuted() {
    m_muted = !m_muted;
    if (m_muted)
        stopSpeaking();
    saveSettings();
    emit mutedChanged();
    if (!m_muted)
        speak("Voice synthesis online, sir.", "listen");
}

QString Backend::resolveMicWorkerScript() const {
    const QStringList candidates = {
        m_dataDir + "/mic_worker.py",
        "/usr/share/jarvis/mic_worker.py",
        QCoreApplication::applicationDirPath() + "/../scripts/mic_worker.py",
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path))
            return path;
    }
    return {};
}

void Backend::sendMicControl(const QString &cmd) {
    if (m_micWorker.state() != QProcess::Running)
        return;
    QJsonObject obj;
    obj["cmd"] = cmd;
    m_micWorker.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n");
}

void Backend::syncMicPauseState() {
    if (!m_micEnabled || m_micWorker.state() != QProcess::Running)
        return;
    const bool shouldPause = (m_busy && !m_approvalPending) || m_audioPlayer.state() != QProcess::NotRunning || !m_audioQueue.isEmpty();
    if (shouldPause == m_micPaused)
        return;
    m_micPaused = shouldPause;
    sendMicControl(m_micPaused ? "pause" : "resume");
}

void Backend::startMicWorker() {
    if (m_headless || m_micWorker.state() != QProcess::NotRunning)
        return;

    const QString py = m_dataDir + "/tts-venv/bin/python";
    const QString script = resolveMicWorkerScript();
    if (!QFileInfo::exists(py) || script.isEmpty()) {
        const QString err = "Microphone worker is missing, sir.";
        setState("error");
        appendMessage("telemetry", err, "MIC ERROR");
        speak(err, "alert");
        m_micEnabled = false;
        emit micEnabledChanged();
        return;
    }

    m_micBuffer.clear();
    m_micPaused = false;
    m_micWorker.start(py, {script});
}

void Backend::stopMicWorker() {
    if (m_micListening) {
        m_micListening = false;
        emit micListeningChanged();
    }
    m_micPaused = false;
    if (m_micWorker.state() != QProcess::NotRunning) {
        sendMicControl("quit");
        m_micWorker.closeWriteChannel();
        if (!m_micWorker.waitForFinished(500)) {
            m_micWorker.kill();
            m_micWorker.waitForFinished(400);
        }
    }
    if (!m_busy && m_state == "listening") {
        setAudioLevel(0.0);
        setState("idle");
    }
}

void Backend::setMicEnabled(bool enabled) {
    if (m_micEnabled == enabled)
        return;
    m_micEnabled = enabled;
    emit micEnabledChanged();

    if (m_headless)
        return;

    if (m_micEnabled) {
        startMicWorker();
        appendMessage("telemetry", "Microphone voice uplink active, sir.", "MIC ON");
        speak("Listening, sir.", "listen");
        syncMicPauseState();
    } else {
        stopMicWorker();
        appendMessage("telemetry", "Microphone voice uplink disengaged.", "MIC OFF");
    }
}

void Backend::toggleMic() {
    setMicEnabled(!m_micEnabled);
}

void Backend::onMicWorkerOutput() {
    m_micBuffer.append(m_micWorker.readAllStandardOutput());
    while (true) {
        const int idx = m_micBuffer.indexOf('\n');
        if (idx < 0)
            break;
        const QByteArray line = m_micBuffer.left(idx).trimmed();
        m_micBuffer.remove(0, idx + 1);
        if (line.isEmpty())
            continue;

        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        const QString ev = obj.value("event").toString();

        if (ev == "ready") {
            syncMicPauseState();
        } else if (ev == "listening") {
            const bool active = obj.value("active").toBool(false);
            if (m_micListening != active) {
                m_micListening = active;
                emit micListeningChanged();
            }
            if (!m_busy && m_audioPlayer.state() == QProcess::NotRunning) {
                if (active) {
                    setActiveTool("LISTENING");
                    setState("listening");
                } else {
                    setAudioLevel(0.0);
                    if (m_state == "listening")
                        setState("idle");
                }
            }
        } else if (ev == "level") {
            if (m_micListening && !m_busy && m_audioPlayer.state() == QProcess::NotRunning) {
                setAudioLevel(obj.value("level").toDouble());
            }
        } else if (ev == "transcribing") {
            if (!m_busy) {
                setAudioLevel(0.0);
                setActiveTool("TRANSCRIBING");
                setState("thinking");
            }
        } else if (ev == "transcript") {
            const QString text = obj.value("text").toString().trimmed();
            setActiveTool({});
            if (m_approvalPending && !text.isEmpty()) {
                const QString matchedOption =
                    matchVoiceApprovalOption(text, m_pendingApproval.value("options").toList());
                if (!matchedOption.isEmpty()) {
                    respondToApproval(matchedOption);
                } else {
                    setState("approval");
                    setActiveTool("APPROVAL REQUIRED");
                }
            } else if (!text.isEmpty() && !m_busy) {
                sendPrompt(text);
            } else if (!m_busy) {
                setState("idle");
            }
        } else if (ev == "empty") {
            setActiveTool({});
            if (!m_busy && m_audioPlayer.state() == QProcess::NotRunning)
                setState("idle");
        } else if (ev == "error") {
            const QString err = obj.value("error").toString().trimmed();
            setActiveTool({});
            appendMessage("telemetry", err.isEmpty() ? "Microphone fault." : err, "MIC ERROR");
            if (!m_busy)
                setState("idle");
        }
    }
}

void Backend::toggleVoiceHooks() {
    m_voiceHooksEnabled = !m_voiceHooksEnabled;
    saveSettings();
    emit voiceHooksEnabledChanged();
}

void Backend::setAutoApprove(bool enabled) {
    if (m_autoApprove == enabled)
        return;
    m_autoApprove = enabled;
    saveSettings();
    emit autoApproveChanged();

    if (m_autoApprove && m_approvalPending) {
        const QString chosen = pickAutoApproveOption(m_pendingApproval.value("options").toList());
        if (!chosen.isEmpty())
            respondToApproval(chosen);
    }

    // Restart idle ACP subprocess so the next turn picks up the updated permission flag.
    if (!m_busy && m_acpProcess.state() != QProcess::NotRunning)
        stopAcpProcess();

    if (!m_headless) {
        const QString msg = m_autoApprove
                                ? "Full autonomy engaged, sir. No further approvals required."
                                : "Manual approval safeguards restored, sir.";
        appendMessage("telemetry", msg, m_autoApprove ? "AUTO-APPROVE ON" : "AUTO-APPROVE OFF");
        speak(msg, "listen");
    }
}

void Backend::toggleAutoApprove() {
    setAutoApprove(!m_autoApprove);
}

QString Backend::acpModelShortLabel(const QString &modelId) {
    if (modelId == "gemini-3.8-flash-low")
        return "FLASH LOW";
    if (modelId == "gemini-3.8-flash-medium")
        return "FLASH MED";
    if (modelId == "gemini-3.8-flash-high")
        return "FLASH HIGH";
    if (modelId == "gemini-4-neon-low")
        return "NEON LOW";
    if (modelId == "gemini-4-neon-medium")
        return "NEON MED";
    if (modelId == "gemini-4-neon-high")
        return "NEON HIGH";
    if (modelId.isEmpty())
        return "DEFAULT";
    return modelId.left(14).toUpper();
}

QString Backend::acpModelShort() const {
    return acpModelShortLabel(m_acpModel);
}

void Backend::setAcpModel(const QString &modelId) {
    const QString cleaned = modelId.trimmed();
    if (m_acpModel == cleaned)
        return;
    m_acpModel = cleaned;
    saveSettings();
    emit acpModelChanged();

    // Apply immediately on an active idle ACP session so the next turn uses the new model.
    if (!m_busy && !m_acpSessionId.isEmpty() && m_acpProcess.state() == QProcess::Running && !m_acpModel.isEmpty()) {
        m_acpSetModelRpcId = m_acpNextRpcId++;
        sendAcpJson(QJsonObject{
            {"jsonrpc", "2.0"},
            {"id", m_acpSetModelRpcId},
            {"method", "session/set_model"},
            {"params", QJsonObject{{"sessionId", m_acpSessionId}, {"modelId", m_acpModel}}},
        });
    }

    if (!m_headless) {
        const QString msg = QString("Switched Antigravity model to %1, sir.").arg(acpModelShort());
        appendMessage("telemetry", msg, "MODEL");
        speak(msg, "listen");
    }
}

void Backend::cycleAcpModel() {
    static const QStringList kPresets = {
        "gemini-3.8-flash-low",
        "gemini-4-neon-low",
        "gemini-4-neon-high",
    };
    const int idx = kPresets.indexOf(m_acpModel);
    const QString next = kPresets.at((idx + 1) % kPresets.size());
    setAcpModel(next);
}

void Backend::setSkipDefaultCustomizations(bool skip) {
    if (m_skipDefaultCustomizations == skip)
        return;
    m_skipDefaultCustomizations = skip;
    saveSettings();
    emit skipDefaultCustomizationsChanged();

    // Restart idle ACP subprocess so the next turn picks up the updated CLI flag.
    if (!m_busy && m_acpProcess.state() != QProcess::NotRunning)
        stopAcpProcess();
}

void Backend::toggleSkipDefaultCustomizations() {
    setSkipDefaultCustomizations(!m_skipDefaultCustomizations);
}

void Backend::cyclePalette() {
    if (m_paletteMode == "gold")
        m_paletteMode = "cyan";
    else if (m_paletteMode == "cyan")
        m_paletteMode = "omarchy";
    else
        m_paletteMode = "gold";
    saveSettings();
    emit paletteModeChanged();
}
