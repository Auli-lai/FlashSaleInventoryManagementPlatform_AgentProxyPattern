#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QStringList>
#include <functional>
#include <deque>
#include "resp_codec.h"

// 响应回调：组件发送命令后可注册回调来接收专属响应
using ResponseCallback = std::function<void(const RespValue&)>;

class RedisClient : public QObject {
    Q_OBJECT
public:
    explicit RedisClient(QObject* parent = nullptr);
    ~RedisClient();

    // ── 连接管理 ──
    void connectToHost(const QString& host, int port);
    void disconnectFromHost();
    bool isConnected() const;

    // ── 命令发送 ──
    // 不带回调：响应通过 responseReceived 信号广播（控制台显示用）
    void sendCommand(const QStringList& args);
    // 带回调：响应直接交给回调处理，不广播（KeyBrowser/ValueViewer 等专用）
    void sendCommand(const QStringList& args, ResponseCallback callback);

    void sendRaw(const QString& raw);

    QString host() const { return m_host; }
    int port() const { return m_port; }

signals:
    void connected();
    void disconnected();
    // 只有不带回调的命令的响应才通过此信号广播
    void responseReceived(const RespValue& value);
    void errorOccurred(const QString& message);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError error);

private:
    QTcpSocket* m_socket;
    QByteArray m_readBuffer;
    QString m_host;
    int m_port = 0;

    // 回调队列：每个带回调的 sendCommand 对应一个回调，FIFO
    std::deque<ResponseCallback> m_callbacks;

    void tryParseResponses();
};

#endif // REDIS_CLIENT_H
