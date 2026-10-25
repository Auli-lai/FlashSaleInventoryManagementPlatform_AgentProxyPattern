#ifndef CONSOLE_PANEL_H
#define CONSOLE_PANEL_H

#include <QWidget>
#include <QTextEdit>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QStringList>
#include <QCompleter>
#include "resp_codec.h"

class RedisClient;

class ConsolePanel : public QWidget {
    Q_OBJECT
public:
    explicit ConsolePanel(QWidget* parent = nullptr);

    void setClient(RedisClient* client);

    void appendOutput(const QString& text, bool isError = false);
    void appendCommand(const QString& cmd);

signals:
    void commandEntered(const QStringList& args);

private slots:
    void onReturnPressed();
    void onResponse(const RespValue& value);  // 显示所有响应

private:
    RedisClient* m_client = nullptr;
    QTextEdit* m_output;
    QLineEdit* m_input;

    QStringList m_history;
    int m_historyIndex = -1;

    QStringList m_commandList;
    QCompleter* m_completer = nullptr;

    void setupCompleter();
    void navigateHistory(int direction);

    // 将 RespValue 格式化为可读字符串
    QString formatResponse(const RespValue& value, int indent = 0);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
};

#endif // CONSOLE_PANEL_H
