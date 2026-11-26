#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "theme.h"
#include "translator.h"

class JarvisTests : public QObject {
    Q_OBJECT

private slots:
    void greetingChangesByTimeOfDay() {
        QVERIFY(Backend::greetingForHour(9).contains("Good morning"));
        QVERIFY(Backend::greetingForHour(14).contains("Good afternoon"));
        QVERIFY(Backend::greetingForHour(22).contains("Good evening"));
    }

    void formatsToolNames() {
        QCOMPARE(Backend::formatToolLabel("run_command"), QString("RUN COMMAND"));
        QCOMPARE(Backend::formatToolLabel("edit_file"), QString("EDIT FILE"));
        QCOMPARE(Backend::formatToolLabel(""), QString());
    }

    void hookEventsDriveStateMachine() {
        QTemporaryDir dataDir, cfgDir;
        Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);

        backend.handleHookJson(R"({"hookEventName":"SessionStart","cwd":"/tmp/demo"})");
        QCOMPARE(backend.state(), QString("listening"));
        QVERIFY(backend.caption().contains("demo"));

        backend.handleHookJson(R"({"hookEventName":"PreInvocation"})");
        QCOMPARE(backend.state(), QString("thinking"));

        backend.handleHookJson(R"({"hookEventName":"PostToolUse","toolName":"run_command"})");
        QCOMPARE(backend.state(), QString("working"));
        QCOMPARE(backend.activeTool(), QString("RUN COMMAND"));

        backend.handleHookJson(R"({"hookEventName":"PostToolUse","toolName":"run_command","error":"exit 1"})");
        QCOMPARE(backend.state(), QString("error"));

        backend.handleHookJson(R"({"hookEventName":"Stop","terminationReason":"NO_TOOL_CALL"})");
        QCOMPARE(backend.state(), QString("idle"));
        QVERIFY(backend.activeTool().isEmpty());
    }

    void orbSizeClampsAndPersists() {
        QTemporaryDir dataDir, cfgDir;
        {
            Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);
            QCOMPARE(backend.orbSize(), Backend::kDefaultOrbSize);

            backend.adjustOrbSize(1000);
            QCOMPARE(backend.orbSize(), Backend::kMaxOrbSize);

            backend.adjustOrbSize(-1000);
            QCOMPARE(backend.orbSize(), Backend::kMinOrbSize);

            backend.setOrbSize(280);
            QCOMPARE(backend.orbSize(), 280);
        }

        Backend reloaded(dataDir.path(), cfgDir.path(), /*headless=*/true);
        QCOMPARE(reloaded.orbSize(), 280);
    }

    void followsOmarchyDefaultAgentAndAllowsOverride() {
        QTemporaryDir dataDir, cfgDir, omarchyDir;
        const QString agentFile = omarchyDir.filePath("agent");
        {
            QFile f(agentFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("codex\n");
        }

        Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true, agentFile);
        QCOMPARE(backend.selectedAgent(), QString("auto"));
        QCOMPARE(backend.omarchyDefaultAgent(), QString("codex"));
        QCOMPARE(backend.effectiveAgent(), QString("codex"));
        QCOMPARE(backend.effectiveAgentName(), QString("Codex"));
        QCOMPARE(backend.effectiveAgentShort(), QString("CODEX"));

        // Switching Omarchy's default agent updates J.A.R.V.I.S. automatically.
        {
            QFile f(agentFile);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write("claude\n");
        }
        QTRY_COMPARE(backend.effectiveAgent(), QString("claude"));
        QCOMPARE(backend.effectiveAgentName(), QString("Claude Code"));

        // Explicit user selection overrides Omarchy default and persists.
        backend.selectAgent("agy");
        QCOMPARE(backend.effectiveAgent(), QString("agy"));

        Backend reloaded(dataDir.path(), cfgDir.path(), /*headless=*/true, agentFile);
        QCOMPARE(reloaded.selectedAgent(), QString("agy"));
        QCOMPARE(reloaded.effectiveAgent(), QString("agy"));
    }

    void settingsPersistAcrossInstances() {
        QTemporaryDir dataDir, cfgDir;
        {
            Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);
            QCOMPARE(backend.alwaysOnTop(), false);
            QCOMPARE(backend.muted(), false);
            QCOMPARE(backend.paletteMode(), QString("gold"));

            backend.toggleAlwaysOnTop();
            backend.toggleMuted();
            backend.cyclePalette();

            QCOMPARE(backend.alwaysOnTop(), true);
            QCOMPARE(backend.muted(), true);
            QCOMPARE(backend.paletteMode(), QString("cyan"));
        }

        Backend reloaded(dataDir.path(), cfgDir.path(), /*headless=*/true);
        QCOMPARE(reloaded.alwaysOnTop(), true);
        QCOMPARE(reloaded.muted(), true);
        QCOMPARE(reloaded.paletteMode(), QString("cyan"));
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFB020"));
        QCOMPARE(theme.accentForeground(), QString("black"));
    }

    void themeReadsAccent() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        QFile colors(dir.path() + "/theme/colors.toml");
        QVERIFY(colors.open(QIODevice::WriteOnly));
        colors.write("accent = \"#112233\"\n");
        colors.close();

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "TOP").isNull());
    }

    void micToggleStartsOffAndTogglesCleanly() {
        QTemporaryDir dataDir, cfgDir;
        Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);
        QCOMPARE(backend.micEnabled(), false);
        backend.toggleMic();
        QCOMPARE(backend.micEnabled(), true);
        backend.toggleMic();
        QCOMPARE(backend.micEnabled(), false);
    }

    void acpApprovalFlowAndVoiceMatching() {
        QTemporaryDir dataDir, cfgDir;
        Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);
        backend.setAutoApprove(false);
        QSignalSpy reqSpy(&backend, &Backend::approvalRequested);

        // Cache tool_call metadata first, then trigger session/request_permission.
        backend.handleAcpLine(
            R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s1","update":{"sessionUpdate":"tool_call","toolCallId":"call_1","title":"Run shell command","kind":"execute","status":"pending","rawInput":{"CommandLine":"rm -rf /tmp/demo","BypassSandbox":true}}}})");
        QCOMPARE(backend.state(), QString("working"));

        backend.handleAcpLine(
            R"({"jsonrpc":"2.0","id":7,"method":"session/request_permission","params":{"sessionId":"s1","toolCall":{"toolCallId":"call_1"},"options":[{"optionId":"opt_once","name":"Allow once","kind":"allow_once"},{"optionId":"opt_always","name":"Always allow","kind":"allow_always"},{"optionId":"opt_deny","name":"Deny","kind":"reject_once"}]}})");

        QCOMPARE(reqSpy.count(), 1);
        QCOMPARE(backend.approvalPending(), true);
        QCOMPARE(backend.state(), QString("approval"));
        QCOMPARE(backend.pendingApproval().value("title").toString(), QString("Run shell command"));
        QVERIFY(backend.pendingApproval().value("detail").toString().contains("rm -rf /tmp/demo"));
        QVERIFY(backend.pendingApproval().value("detail").toString().contains("outside sandbox"));

        const QVariantList opts = backend.pendingApproval().value("options").toList();
        QCOMPARE(opts.size(), 3);
        QCOMPARE(Backend::matchVoiceApprovalOption("Yes, approve that", opts), QString("opt_once"));
        QCOMPARE(Backend::matchVoiceApprovalOption("Always allow it", opts), QString("opt_always"));
        QCOMPARE(Backend::matchVoiceApprovalOption("No, deny it", opts), QString("opt_deny"));

        backend.respondToApproval("opt_once");
        QCOMPARE(backend.approvalPending(), false);
        QCOMPARE(backend.state(), QString("working"));

        // Verify streaming agent_message_chunk appends to messages.
        backend.handleAcpLine(
            R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s1","update":{"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Operation complete, sir."}}}})");
        QVERIFY(!backend.messages().isEmpty());
        QCOMPARE(backend.messages().last().toMap().value("text").toString(), QString("Operation complete, sir."));
    }

    void autoApproveDefaultsOffAndOptInBypassesApprovalPrompt() {
        QTemporaryDir dataDir, cfgDir;
        {
            Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);
            QCOMPARE(backend.autoApprove(), false);
            QVERIFY(!backend.buildAcpArgs().contains("--dangerously-skip-permissions"));

            backend.setAutoApprove(true);
            QCOMPARE(backend.autoApprove(), true);
            QVERIFY(backend.buildAcpArgs().contains("--dangerously-skip-permissions"));

            QSignalSpy reqSpy(&backend, &Backend::approvalRequested);

            backend.handleAcpLine(
                R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"s1","update":{"sessionUpdate":"tool_call","toolCallId":"call_auto","title":"Run shell command","kind":"execute","status":"pending","rawInput":{"CommandLine":"echo hi"}}}})");

            backend.handleAcpLine(
                R"({"jsonrpc":"2.0","id":9,"method":"session/request_permission","params":{"sessionId":"s1","toolCall":{"toolCallId":"call_auto"},"options":[{"optionId":"opt_once","name":"Allow once","kind":"allow_once"},{"optionId":"opt_always","name":"Always allow","kind":"allow_always"},{"optionId":"opt_deny","name":"Deny","kind":"reject_once"}]}})");

            QCOMPARE(reqSpy.count(), 0);
            QCOMPARE(backend.approvalPending(), false);
            QCOMPARE(backend.state(), QString("working"));
            QVERIFY(!backend.messages().isEmpty());
            QCOMPARE(backend.messages().last().toMap().value("meta").toString(), QString("AUTO-APPROVED"));

            const QVariantList opts = {
                QVariantMap{{"optionId", "opt_deny"}, {"kind", "reject_once"}},
                QVariantMap{{"optionId", "opt_once"}, {"kind", "allow_once"}},
                QVariantMap{{"optionId", "opt_always"}, {"kind", "allow_always"}},
            };
            QCOMPARE(Backend::pickAutoApproveOption(opts), QString("opt_always"));
        }

        Backend reloaded(dataDir.path(), cfgDir.path(), /*headless=*/true);
        QCOMPARE(reloaded.autoApprove(), true);
    }

    void acpSpeedModelAndFastCustomizationsDefaultAndPersist() {
        QTemporaryDir dataDir, cfgDir;
        {
            Backend backend(dataDir.path(), cfgDir.path(), /*headless=*/true);
            QCOMPARE(backend.acpModel(), QString("gemini-3.8-flash-low"));
            QCOMPARE(backend.acpModelShort(), QString("FLASH LOW"));
            QCOMPARE(backend.skipDefaultCustomizations(), true);

            const QStringList defaultArgs = backend.buildAcpArgs();
            QVERIFY(defaultArgs.contains("acp"));
            QVERIFY(defaultArgs.contains("--stdio"));
            QVERIFY(!defaultArgs.contains("--dangerously-skip-permissions"));
            QVERIFY(defaultArgs.contains("--include-default-customizations=false"));

            backend.cycleAcpModel();
            QCOMPARE(backend.acpModel(), QString("gemini-4-neon-low"));
            QCOMPARE(backend.acpModelShort(), QString("NEON LOW"));

            backend.cycleAcpModel();
            QCOMPARE(backend.acpModel(), QString("gemini-4-neon-high"));

            backend.cycleAcpModel();
            QCOMPARE(backend.acpModel(), QString("gemini-3.8-flash-low"));

            backend.setAcpModel("gemini-4-neon-low");
            backend.setSkipDefaultCustomizations(false);
            QVERIFY(!backend.buildAcpArgs().contains("--include-default-customizations=false"));
        }

        Backend reloaded(dataDir.path(), cfgDir.path(), /*headless=*/true);
        QCOMPARE(reloaded.acpModel(), QString("gemini-4-neon-low"));
        QCOMPARE(reloaded.skipDefaultCustomizations(), false);
    }
};

QTEST_MAIN(JarvisTests)
#include "jarvis_tests.moc"
