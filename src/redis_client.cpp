#include "redis_client.h"
#include "log.h"
#include <QDebug>

RedisClient::RedisClient(QObject* parent)
    : QObject(parent)
{
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,
            this, &RedisClient::onConnected);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &RedisClient::onDisconnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &RedisClient::onReadyRead);
    connect(m_socket, &QAbstractSocket::errorOccurred,
            this, &RedisClient::onSocketError);
}

RedisClient::~RedisClient() {
    disconnectFromHost();
}

void RedisClient::connectToHost(const QString& host, int port) {
    // 防止重复连接
    if (m_socket->state() == QAbstractSocket::ConnectingState ||
        m_socket->state() == QAbstractSocket::ConnectedState) {
        Log::info("Already connecting/connected, ignoring duplicate connectToHost");
        return;
    }

    m_host = host;
    m_port = port;
    m_readBuffer.clear();
    m_callbacks.clear();  // 清空未处理的回调
    Log::info("Connecting to %s:%d...", host.toStdString().c_str(), port);
    m_socket->connectToHost(host, port);
}

void RedisClient::disconnectFromHost() {
    m_callbacks.clear();
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->disconnectFromHost();
    }
}

bool RedisClient::isConnected() const {
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

// ── 不带回调的发送（响应通过 responseReceived 信号广播）──
void RedisClient::sendCommand(const QStringList& args) {
    sendCommand(args, nullptr);
}

// ── 带回调的发送（响应直接交给回调）──
void RedisClient::sendCommand(const QStringList& args, ResponseCallback callback) {
    if (!isConnected()) {
        if (callback) {
            // 即使未连接也调用回调，传一个 NIL 表示失败
            RespValue nil;
            nil.type = RespValue::NIL;
            callback(nil);
        }
        emit errorOccurred("Not connected to server");
        return;
    }

    std::vector<std::string> stdArgs;
    for (const QString& arg : args) {
        stdArgs.push_back(arg.toStdString());
    }

    std::string resp = resp_encode(stdArgs);
    m_socket->write(resp.c_str(), resp.size());

    // 将回调入队
    m_callbacks.push_back(callback);

    QString cmdLine = args.join(' ');
    qDebug() << ">> " << cmdLine;
}

void RedisClient::sendRaw(const QString& raw) {
    QStringList args = raw.split(' ', Qt::SkipEmptyParts);
    sendCommand(args);
}

// ── 私有槽 ──

void RedisClient::onConnected() {
    Log::info("Connected to %s:%d",
              m_host.toStdString().c_str(), m_port);
    emit connected();
}

void RedisClient::onDisconnected() {
    Log::info("Disconnected from %s:%d",
              m_host.toStdString().c_str(), m_port);
    m_readBuffer.clear();
    m_callbacks.clear();
    emit disconnected();
}

void RedisClient::onReadyRead() {
    m_readBuffer.append(m_socket->readAll());
    tryParseResponses();
}

void RedisClient::onSocketError(QAbstractSocket::SocketError error) {
    Q_UNUSED(error);
    QString msg = m_socket->errorString();
    Log::error("Socket error: %s", msg.toStdString().c_str());
    emit errorOccurred(msg);
}

void RedisClient::tryParseResponses() {
    while (m_readBuffer.size() > 0) {
        auto result = resp_decode(m_readBuffer.constData(),
                                   m_readBuffer.size());
        if (result.complete) {
            m_readBuffer.remove(0, result.consumed);

            // ── 关键：路由响应 ──
            // 如果回调队列非空，队首回调接收此响应（FIFO 顺序匹配）
            if (!m_callbacks.empty()) {
                ResponseCallback cb = m_callbacks.front();
                m_callbacks.pop_front();
                if (cb) {
                    cb(result.value);  // 交给回调处理
                }
                // 如果回调是 nullptr，响应 fall through 到广播
                else {
                    emit responseReceived(result.value);
                }
            } else {
                // 没有回调在等待 → 广播（控制台显示用）
                emit responseReceived(result.value);
            }
        } else if (!result.error.empty()) {
            Log::error("RESP decode error: %s", result.error.c_str());
            emit errorOccurred(QString::fromStdString(result.error));
            return;
        } else {
            // 数据不完整，等待下次 readyRead
            break;
        }
    }
}
