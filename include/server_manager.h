#ifndef SERVER_MANAGER_H
#define SERVER_MANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QString>

class ServerManager : public QObject {
    Q_OBJECT
public:
    explicit ServerManager(QObject* parent = nullptr);
    ~ServerManager();

    void start(const QString& serverPath = "");
    void stop();
    bool isRunning() const;

    void setServerPath(const QString& path) { m_serverPath = path; }
    QString serverPath() const { return m_serverPath; }
    int port() const { return m_serverPort; }

signals:
    void started(int port);                // server 启动成功（仅发送一次）
    void stopped();
    void outputReceived(const QString& line);
    void errorOccurred(const QString& msg);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onFallbackTimer();

private:
    QProcess* m_process = nullptr;
    QString m_serverPath;
    int m_serverPort = 0;
    QString m_outputBuffer;
    bool m_startedEmitted = false;  // 防止双重发送 started 信号

    bool tryFindServerPath();
    bool tryParsePort(const QString& line);
    void emitStartedOnce();         // 确保 started 只发一次
};

#endif // SERVER_MANAGER_H
