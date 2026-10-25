#include "stats_dashboard.h"
#include "redis_client.h"
#include "resp_codec.h"
#include <QHeaderView>
#include <QFont>
#include <QTime>
#include <QDebug>

StatsDashboard::StatsDashboard(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    m_titleLabel = new QLabel("Server Info (waiting for connection...)");
    m_titleLabel->setFont(QFont("", 12, QFont::Bold));
    layout->addWidget(m_titleLabel);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({"Metric", "Value"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &StatsDashboard::refresh);
}

void StatsDashboard::setClient(RedisClient* client) {
    m_client = client;
}

void StatsDashboard::startAutoRefresh(int intervalMs) {
    if (m_client && m_client->isConnected()) {
        refresh();
        m_timer->start(intervalMs);
    }
}

void StatsDashboard::stopAutoRefresh() {
    m_timer->stop();
}

void StatsDashboard::refresh() {
    if (!m_client || !m_client->isConnected()) return;

    // 使用回调接收 INFO 响应（不依赖 responseReceived 信号）
    m_client->sendCommand(
        QStringList() << "INFO",
        [this](const RespValue& value) {
            if (value.type == RespValue::STRING) {
                parseInfoOutput(QString::fromStdString(value.str_val));
            }
        });
}

void StatsDashboard::parseInfoOutput(const QString& infoText) {
    m_table->setRowCount(0);

    QStringList lines = infoText.split("\r\n", Qt::SkipEmptyParts);
    if (lines.isEmpty()) {
        lines = infoText.split("\n", Qt::SkipEmptyParts);
    }

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;
        if (trimmed.endsWith('\r')) trimmed.chop(1);

        if (trimmed.startsWith('#')) {
            QString sectionName = trimmed.mid(1).trimmed();
            int row = m_table->rowCount();
            m_table->insertRow(row);
            auto* sectionItem = new QTableWidgetItem(sectionName);
            sectionItem->setFont(QFont("", -1, QFont::Bold));
            sectionItem->setBackground(QColor("#e0e0e0"));
            m_table->setItem(row, 0, sectionItem);
            m_table->setSpan(row, 0, 1, 2);
        } else if (trimmed.contains(':')) {
            int colonIdx = trimmed.indexOf(':');
            QString key = trimmed.left(colonIdx).trimmed();
            QString value = trimmed.mid(colonIdx + 1).trimmed();
            int row = m_table->rowCount();
            m_table->insertRow(row);
            m_table->setItem(row, 0, new QTableWidgetItem(key));
            m_table->setItem(row, 1, new QTableWidgetItem(value));
        }
    }

    m_titleLabel->setText(
        QString("Server Info (last update: %1)")
            .arg(QTime::currentTime().toString("HH:mm:ss")));
}
