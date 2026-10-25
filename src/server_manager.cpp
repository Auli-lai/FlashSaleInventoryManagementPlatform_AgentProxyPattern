#include "server_manager.h"
#include "log.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTcpServer>
#include <QRandomGenerator>

ServerManager::ServerManager(QObject* parent)
    : QObject(parent)
{
}

ServerManager::~ServerManager() {
    stop();
}

static int findAvailablePort() {
    QTcpServer server;
    if (server.listen(QHostAddress::LocalHost, 0)) {
        int port = server.serverPort();
        server.close();
        return port;
    }
    return 16379 + QRandomGenerator::global()->bounded(1000);
}

void ServerManager::start(const QString& serverPath) {
    if (m_process) {
        stop();
    }

    m_startedEmitted = false;

    if (!serverPath.isEmpty()) {
        m_serverPath = serverPath;
    }
    if (m_serverPath.isEmpty() || !QFileInfo::exists(m_serverPath)) {
        if (!tryFindServerPath()) {
            emit errorOccurred(
                "Cannot find mini_redis executable!\n"
                "Please compile mini_redis first, or set path via setServerPath().");
            return;
        }
    }

    Log::info("Starting mini_redis: %s", m_serverPath.toStdString().c_str());

    // 预先分配空闲端口
    m_serverPort = findAvailablePort();
    Log::info("Using port: %d", m_serverPort);

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &ServerManager::onReadyRead);
    connect(m_process,
            static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(
                &QProcess::finished),
            this, &ServerManager::onProcessFinished);

    m_outputBuffer.clear();

    QString portStr = QString::number(m_serverPort);
    m_process->start(m_serverPath, QStringList() << portStr);

    if (!m_process->waitForStarted(3000)) {
        emit errorOccurred("Failed to start mini_redis: " + m_process->errorString());
        delete m_process;
        m_process = nullptr;
        m_serverPort = 0;
        return;
    }

    Log::info("mini_redis process started (PID: %lld)",
              static_cast<long long>(m_process->processId()));

    // 兜底：1 秒后如果还没发 started，用我们已知的端口发
    QTimer::singleShot(1000, this, &ServerManager::onFallbackTimer);
}

void ServerManager::onFallbackTimer() {
    emitStartedOnce();
}

void ServerManager::emitStartedOnce() {
    if (!m_startedEmitted && m_serverPort > 0) {
        m_startedEmitted = true;
        emit started(m_serverPort);
    }
}

void ServerManager::stop() {
    if (!m_process) return;

    Log::info("Stopping mini_redis...");

    m_process->terminate();
    if (!m_process->waitForFinished(3000)) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }

    delete m_process;
    m_process = nullptr;
    m_serverPort = 0;
    m_startedEmitted = false;
    emit stopped();
}

bool ServerManager::isRunning() const {
    return m_process && m_process->state() == QProcess::Running;
}

bool ServerManager::tryFindServerPath() {
    QStringList candidates;
    // 同目录（CMake 构建后两个可执行文件都在 build/bin/ 下）
    candidates << QCoreApplication::applicationDirPath() + "/mini_redis";
    // 独立 mini_redis 项目构建路径（兜底）
    candidates << "./mini_redis";
    candidates << "../mini_redis/build/bin/mini_redis";
    candidates << "../../mini_redis/build/mini_redis";

    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) {
            m_serverPath = QFileInfo(path).absoluteFilePath();
            return true;
        }
    }
    return false;
}

void ServerManager::onReadyRead() {
    m_outputBuffer += QString::fromUtf8(m_process->readAllStandardOutput());

    while (true) {
        int idx = m_outputBuffer.indexOf('\n');
        if (idx == -1) break;

        QString line = m_outputBuffer.left(idx).trimmed();
        m_outputBuffer.remove(0, idx + 1);

        if (!line.isEmpty()) {
            emit outputReceived(line);

            if (tryParsePort(line)) {
                Log::info("Detected server port from output: %d",
                          m_serverPort);
                emitStartedOnce();
            }
        }
    }
}

void ServerManager::onProcessFinished(int exitCode,
                                       QProcess::ExitStatus status) {
    QString msg = QString("mini_redis process exited (code=%1, status=%2)")
                      .arg(exitCode)
                      .arg(status == QProcess::NormalExit ? "normal" : "crash");
    Log::info("%s", msg.toStdString().c_str());
    emit outputReceived(msg);

    bool wasRunning = (m_serverPort > 0);
    m_serverPort = 0;
    m_startedEmitted = false;
    if (wasRunning) {
        emit stopped();
    }
}

bool ServerManager::tryParsePort(const QString& line) {
    QRegularExpression re(
        R"(started on [0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:(\d+))");
    auto match = re.match(line);
    if (match.hasMatch()) {
        int parsedPort = match.captured(1).toInt();
        if (parsedPort > 0) {
            m_serverPort = parsedPort;
            return true;
        }
    }
    return false;
}
