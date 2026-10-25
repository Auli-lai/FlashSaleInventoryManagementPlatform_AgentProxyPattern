#include "console_panel.h"
#include "redis_client.h"
#include <QKeyEvent>
#include <QScrollBar>
#include <QAbstractItemView>

ConsolePanel::ConsolePanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_output = new QTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(QFont("monospace", 10));
    m_output->setStyleSheet(
        "QTextEdit { background-color: #1e1e1e; color: #d4d4d4; "
        "border: 1px solid #333; padding: 4px; }");
    m_output->document()->setMaximumBlockCount(1000);
    layout->addWidget(m_output);

    m_input = new QLineEdit(this);
    m_input->setFont(QFont("monospace", 10));
    m_input->setPlaceholderText("> Redis command, e.g. PING / SET key value / GET key");
    m_input->setStyleSheet(
        "QLineEdit { background-color: #2d2d2d; color: #d4d4d4; "
        "border: 1px solid #555; padding: 4px; }");
    layout->addWidget(m_input);

    setFocusPolicy(Qt::StrongFocus);
    m_input->installEventFilter(this);

    setupCompleter();

    connect(m_input, &QLineEdit::returnPressed,
            this, &ConsolePanel::onReturnPressed);
}

void ConsolePanel::setClient(RedisClient* client) {
    if (m_client) {
        disconnect(m_client, &RedisClient::responseReceived,
                   this, &ConsolePanel::onResponse);
    }

    m_client = client;

    if (m_client) {
        // 直接监听所有响应，显示在控制台
        connect(m_client, &RedisClient::responseReceived,
                this, &ConsolePanel::onResponse);
    }
}

void ConsolePanel::appendOutput(const QString& text, bool isError) {
    QString color = isError ? "#f44747" : "#6a9955";
    m_output->append(QString("<span style='color:%1'>%2</span>")
                         .arg(color, text.toHtmlEscaped()));
    QScrollBar* sb = m_output->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void ConsolePanel::appendCommand(const QString& cmd) {
    m_output->append(QString("<span style='color:#569cd6'>&gt; %1</span>")
                         .arg(cmd.toHtmlEscaped()));
}

// ── 接收所有响应并显示 ──
void ConsolePanel::onResponse(const RespValue& value) {
    QString text = formatResponse(value);
    bool isErr = value.is_error();
    appendOutput(text, isErr);
}

// ── 回车发送命令 ──
void ConsolePanel::onReturnPressed() {
    QString text = m_input->text().trimmed();
    if (text.isEmpty()) return;

    m_history.append(text);
    m_historyIndex = m_history.size();

    QStringList args = text.split(' ', Qt::SkipEmptyParts);
    appendCommand(text);
    emit commandEntered(args);

    m_input->clear();
}

// ── 格式化响应 ──
QString ConsolePanel::formatResponse(const RespValue& value, int indent) {
    QString pad(indent * 2, ' ');

    switch (value.type) {
    case RespValue::STRING:
        return pad + QString::fromStdString(value.str_val);
    case RespValue::ERROR:
        return pad + "(error) " + QString::fromStdString(value.str_val);
    case RespValue::INTEGER:
        return pad + "(integer) " + QString::number(value.int_val);
    case RespValue::NIL:
        return pad + "(nil)";
    case RespValue::ARRAY: {
        if (value.array_val.empty()) {
            return pad + "(empty array)";
        }
        // 紧凑显示：如果元素都是简单类型，用一行
        bool allSimple = true;
        for (auto& v : value.array_val) {
            if (v.type == RespValue::ARRAY) { allSimple = false; break; }
        }
        if (allSimple) {
            QStringList items;
            for (auto& v : value.array_val) {
                items << formatResponse(v).trimmed();
            }
            return pad + items.join("\n" + pad);
        }
        // 嵌套数组：分行
        QStringList lines;
        lines << pad + "[";
        for (size_t i = 0; i < value.array_val.size(); ++i) {
            lines << formatResponse(value.array_val[i], indent + 1);
        }
        lines << pad + "]";
        return lines.join("\n");
    }
    }
    return pad + "?";
}

bool ConsolePanel::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_input && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up) {
            navigateHistory(-1);
            return true;
        } else if (keyEvent->key() == Qt::Key_Down) {
            navigateHistory(1);
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ConsolePanel::navigateHistory(int direction) {
    if (m_history.isEmpty()) return;
    m_historyIndex += direction;
    if (m_historyIndex < 0) m_historyIndex = 0;
    if (m_historyIndex >= m_history.size())
        m_historyIndex = m_history.size() - 1;
    m_input->setText(m_history.at(m_historyIndex));
}

void ConsolePanel::setupCompleter() {
    m_commandList << "PING" << "SET" << "GET" << "DEL" << "EXISTS" << "KEYS" << "TYPE"
        << "INCR" << "INCRBY" << "DECR" << "DECRBY"
        << "LPUSH" << "RPUSH" << "LPOP" << "RPOP" << "LRANGE" << "LLEN"
        << "HSET" << "HGET" << "HDEL" << "HGETALL" << "HEXISTS" << "HLEN"
        << "SADD" << "SREM" << "SMEMBERS" << "SISMEMBER" << "SCARD"
        << "ZADD" << "ZREM" << "ZSCORE" << "ZRANGE" << "ZRANK" << "ZCARD"
        << "EXPIRE" << "TTL" << "PERSIST"
        << "DBSIZE" << "FLUSHDB" << "INFO" << "CONFIG" << "CLIENT"
        << "SELECT" << "SCAN" << "RENAME" << "HELLO" << "COMMAND"
        << "PUBLISH" << "SUBSCRIBE" << "MULTI" << "EXEC" << "DISCARD"
        << "AUTH" << "QUIT" << "ECHO";
    m_commandList.sort();

    m_completer = new QCompleter(m_commandList, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_input->setCompleter(m_completer);
}
