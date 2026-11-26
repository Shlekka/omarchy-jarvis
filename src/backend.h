#pragma once

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString caption READ caption NOTIFY captionChanged)
    Q_PROPERTY(QString activeTool READ activeTool NOTIFY activeToolChanged)
    Q_PROPERTY(double audioLevel READ audioLevel NOTIFY audioLevelChanged)
    Q_PROPERTY(int orbSize READ orbSize NOTIFY orbSizeChanged)
    Q_PROPERTY(bool alwaysOnTop READ alwaysOnTop NOTIFY alwaysOnTopChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(bool micEnabled READ micEnabled NOTIFY micEnabledChanged)
    Q_PROPERTY(bool micListening READ micListening NOTIFY micListeningChanged)
    Q_PROPERTY(bool voiceHooksEnabled READ voiceHooksEnabled NOTIFY voiceHooksEnabledChanged)
    Q_PROPERTY(bool autoApprove READ autoApprove NOTIFY autoApproveChanged)
    Q_PROPERTY(QString acpModel READ acpModel NOTIFY acpModelChanged)
    Q_PROPERTY(QString acpModelShort READ acpModelShort NOTIFY acpModelChanged)
    Q_PROPERTY(bool skipDefaultCustomizations READ skipDefaultCustomizations NOTIFY skipDefaultCustomizationsChanged)
    Q_PROPERTY(QString paletteMode READ paletteMode NOTIFY paletteModeChanged)
    Q_PROPERTY(QString selectedAgent READ selectedAgent NOTIFY agentChanged)
    Q_PROPERTY(QString omarchyDefaultAgent READ omarchyDefaultAgent NOTIFY agentChanged)
    Q_PROPERTY(QString effectiveAgent READ effectiveAgent NOTIFY agentChanged)
    Q_PROPERTY(QString effectiveAgentName READ effectiveAgentName NOTIFY agentChanged)
    Q_PROPERTY(QString effectiveAgentShort READ effectiveAgentShort NOTIFY agentChanged)
    Q_PROPERTY(QVariantList availableAgents READ availableAgents NOTIFY agentChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool ttsReady READ ttsReady NOTIFY ttsReadyChanged)
    Q_PROPERTY(bool hookConnected READ hookConnected NOTIFY hookConnectedChanged)
    Q_PROPERTY(QString conversationId READ conversationId NOTIFY conversationIdChanged)
    Q_PROPERTY(QVariantList messages READ messages NOTIFY messagesChanged)
    Q_PROPERTY(bool approvalPending READ approvalPending NOTIFY pendingApprovalChanged)
    Q_PROPERTY(QVariantMap pendingApproval READ pendingApproval NOTIFY pendingApprovalChanged)

public:
    static constexpr int kMinOrbSize = 110;
    static constexpr int kMaxOrbSize = 420;
    static constexpr int kDefaultOrbSize = 216;

    explicit Backend(const QString &dataDir = {}, const QString &configDir = {}, bool headless = false,
                     const QString &omarchyAgentFile = {}, QObject *parent = nullptr);
    ~Backend() override;

    QString state() const { return m_state; }
    QString caption() const { return m_caption; }
    QString activeTool() const { return m_activeTool; }
    double audioLevel() const { return m_audioLevel; }
    int orbSize() const { return m_orbSize; }
    bool alwaysOnTop() const { return m_alwaysOnTop; }
    bool muted() const { return m_muted; }
    bool micEnabled() const { return m_micEnabled; }
    bool micListening() const { return m_micListening; }
    bool voiceHooksEnabled() const { return m_voiceHooksEnabled; }
    bool autoApprove() const { return m_autoApprove; }
    QString acpModel() const { return m_acpModel; }
    QString acpModelShort() const;
    bool skipDefaultCustomizations() const { return m_skipDefaultCustomizations; }
    QString paletteMode() const { return m_paletteMode; }
    QString selectedAgent() const { return m_selectedAgent; }
    QString omarchyDefaultAgent() const { return m_omarchyDefaultAgent; }
    QString effectiveAgent() const;
    QString effectiveAgentName() const;
    QString effectiveAgentShort() const;
    QVariantList availableAgents() const { return m_availableAgents; }
    bool busy() const { return m_busy; }
    bool ttsReady() const { return m_ttsReady; }
    bool hookConnected() const { return m_hookConnected; }
    QString conversationId() const { return m_conversationId; }
    QVariantList messages() const { return m_messages; }
    bool approvalPending() const { return m_approvalPending; }
    QVariantMap pendingApproval() const { return m_pendingApproval; }

    Q_INVOKABLE void sendPrompt(const QString &prompt);
    Q_INVOKABLE void cancelPrompt();
    Q_INVOKABLE void respondToApproval(const QString &optionId);
    Q_INVOKABLE void cancelApproval();
    Q_INVOKABLE void resetConversation();
    Q_INVOKABLE void speak(const QString &text, const QString &chime = "none");
    Q_INVOKABLE void stopSpeaking();
    Q_INVOKABLE void greet();
    Q_INVOKABLE void adjustOrbSize(int delta);
    Q_INVOKABLE void setOrbSize(int size);
    Q_INVOKABLE void resetOrbSize();
    Q_INVOKABLE void syncWindowSize(int width, int height);
    Q_INVOKABLE void toggleAlwaysOnTop();
    Q_INVOKABLE void setAlwaysOnTop(bool pinned);
    Q_INVOKABLE void toggleMuted();
    Q_INVOKABLE void toggleMic();
    Q_INVOKABLE void setMicEnabled(bool enabled);
    Q_INVOKABLE void toggleVoiceHooks();
    Q_INVOKABLE void toggleAutoApprove();
    Q_INVOKABLE void setAutoApprove(bool enabled);
    Q_INVOKABLE void setAcpModel(const QString &modelId);
    Q_INVOKABLE void cycleAcpModel();
    Q_INVOKABLE void setSkipDefaultCustomizations(bool skip);
    Q_INVOKABLE void toggleSkipDefaultCustomizations();
    Q_INVOKABLE void cyclePalette();
    Q_INVOKABLE void selectAgent(const QString &agentId);
    Q_INVOKABLE void refreshAgents();
    Q_INVOKABLE void clearMessages();
    Q_INVOKABLE void handleHookJson(const QByteArray &jsonBytes);
    Q_INVOKABLE void handleAcpLine(const QByteArray &line);

    static QString formatToolLabel(const QString &toolName);
    static QString greetingForHour(int hour);
    static QString readOmarchyDefaultAgent(const QString &path);
    static QString agentDisplayName(const QString &agentId);
    static QString agentShortLabel(const QString &agentId);
    static QString matchVoiceApprovalOption(const QString &transcript, const QVariantList &options);
    static QString pickAutoApproveOption(const QVariantList &options);
    static QString acpModelShortLabel(const QString &modelId);
    QStringList buildAcpArgs() const;

signals:
    void stateChanged();
    void captionChanged();
    void activeToolChanged();
    void audioLevelChanged();
    void orbSizeChanged();
    void alwaysOnTopChanged();
    void mutedChanged();
    void micEnabledChanged();
    void micListeningChanged();
    void voiceHooksEnabledChanged();
    void autoApproveChanged();
    void acpModelChanged();
    void skipDefaultCustomizationsChanged();
    void paletteModeChanged();
    void agentChanged();
    void busyChanged();
    void ttsReadyChanged();
    void hookConnectedChanged();
    void conversationIdChanged();
    void messagesChanged();
    void pendingApprovalChanged();
    void approvalRequested();

private:
    struct SpeechJob {
        int id = 0;
        QString text;
        QString chime;
    };

    struct AudioItem {
        QString wavPath;
        QString spokenText;
        int durationMs = 0;
        QVector<double> envelope;
    };

    void setState(const QString &state);
    void setCaption(const QString &caption, int revertMs = 0);
    void setActiveTool(const QString &tool);
    void setAudioLevel(double level);
    void appendMessage(const QString &role, const QString &text, const QString &meta = {});
    void updateLastAssistantMessage(const QString &text);

    void loadSettings();
    void saveSettings() const;

    void watchOmarchyDefaultAgent();
    void reloadOmarchyDefaultAgent();
    QString resolveAgentExecutable(const QString &agentId, bool *isInstalled = nullptr) const;
    static bool isMiseWrapperScript(const QString &path);
    void applyOmarchyAccountEnv(const QString &agentId, QProcessEnvironment &env) const;

    void startHookServer();
    void onHookConnection();

    void startTtsWorker();
    void onTtsWorkerOutput();
    void startMicWorker();
    void stopMicWorker();
    void sendMicControl(const QString &cmd);
    void syncMicPauseState();
    void onMicWorkerOutput();
    QString resolveMicWorkerScript() const;
    void enqueuePlayback(const AudioItem &item);
    void playNextAudio();
    void onAudioTick();
    void onPlayerFinished();

    void onAgentStdout();
    void onAgentFinished(int exitCode, QProcess::ExitStatus status);
    void processAgyLine(const QByteArray &line);
    void processClaudeLine(const QByteArray &line);
    void processCodexLine(const QByteArray &line);

    void startAcpPrompt(const QString &exe, const QString &promptText);
    void stopAcpProcess();
    void sendAcpJson(const QJsonObject &obj);
    void sendAcpPromptNow(const QString &promptText);
    void onAcpStdout();
    void onAcpFinished(int exitCode, QProcess::ExitStatus status);
    void clearPendingApproval();
    static QString extractApprovalDetail(const QJsonObject &toolCall);

    void applyHyprlandPin(bool desiredPin);
    void applyHyprlandResize(int width, int height);
    QString findHyprlandWindowAddress(bool *currentlyPinned = nullptr) const;
    QString resolveTtsWorkerScript() const;

    QString m_dataDir;
    QString m_configDir;
    QString m_omarchyAgentFile;
    bool m_headless = false;

    QString m_state = "idle";
    QString m_caption;
    QString m_activeTool;
    double m_audioLevel = 0.0;
    int m_orbSize = kDefaultOrbSize;
    bool m_alwaysOnTop = false;
    bool m_muted = false;
    bool m_micEnabled = false;
    bool m_micListening = false;
    bool m_micPaused = false;
    bool m_voiceHooksEnabled = true;
    bool m_autoApprove = false;
    QString m_acpModel = "gemini-3.8-flash-low";
    bool m_skipDefaultCustomizations = true;
    QString m_paletteMode = "gold";
    QString m_selectedAgent = "auto";
    QString m_omarchyDefaultAgent = "agy";
    QVariantList m_availableAgents;
    bool m_busy = false;
    bool m_ttsReady = false;
    bool m_hookConnected = false;
    QString m_conversationId;
    QString m_runningAgent;
    QVariantList m_messages;

    QFileSystemWatcher m_agentWatcher;
    QTimer m_agentDebounce;

    QLocalServer m_hookServer;
    QProcess m_ttsWorker;
    QByteArray m_ttsBuffer;
    QProcess m_micWorker;
    QByteArray m_micBuffer;
    int m_nextSpeechId = 1;
    QHash<int, QString> m_speechTextById;
    QQueue<SpeechJob> m_pendingSpeech;

    QProcess m_audioPlayer;
    QQueue<AudioItem> m_audioQueue;
    AudioItem m_currentAudio;
    QElapsedTimer m_audioClock;
    QTimer m_audioTickTimer;

    QProcess m_agentProcess;
    QByteArray m_agentBuffer;
    QProcess m_acpProcess;
    QByteArray m_acpBuffer;
    bool m_acpInitialized = false;
    int m_acpNextRpcId = 1;
    int m_acpInitRpcId = 0;
    int m_acpSessionRpcId = 0;
    int m_acpSetModelRpcId = 0;
    int m_acpActivePromptId = 0;
    QString m_acpSessionId;
    QString m_pendingAcpPrompt;
    QHash<QString, QJsonObject> m_acpToolCalls;

    bool m_approvalPending = false;
    QVariantMap m_pendingApproval;
    QJsonValue m_pendingApprovalRpcId;

    QString m_streamedResponse;
    bool m_assistantBubbleAdded = false;

    QTimer m_idleRevertTimer;
    QTimer m_pinSyncTimer;
    QTimer m_resizeSyncTimer;
    int m_pendingWindowWidth = 0;
    int m_pendingWindowHeight = 0;
    QElapsedTimer m_lastHookSpeech;
    int m_completionLineIdx = 0;
};
