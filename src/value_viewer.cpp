#include "value_viewer.h"
#include "redis_client.h"
#include "resp_codec.h"
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QHeaderView>
#include <QDebug>

ValueViewer::ValueViewer(QWidget* parent) : QWidget(parent) {
    setupUi();
}

void ValueViewer::setClient(RedisClient* client) {
    m_client = client;
}

// ═══════════════════════════════════════════ UI 构建 ══════

void ValueViewer::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);

    // 信息栏
    auto* infoBar = new QHBoxLayout();
    m_keyLabel   = new QLabel("Key: -");
    m_typeLabel  = new QLabel("Type: -");
    m_ttlLabel   = new QLabel("TTL: -");
    m_refreshBtn = new QPushButton("Refresh");
    m_saveBtn    = new QPushButton("Save");
    m_deleteBtn  = new QPushButton("Delete");
    m_deleteBtn->setStyleSheet("color: red;");
    infoBar->addWidget(m_keyLabel);
    infoBar->addWidget(m_typeLabel);
    infoBar->addWidget(m_ttlLabel);
    infoBar->addStretch();
    infoBar->addWidget(m_refreshBtn);
    infoBar->addWidget(m_saveBtn);
    infoBar->addWidget(m_deleteBtn);
    mainLayout->addLayout(infoBar);

    // 页面栈
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(new QLabel("Select a key to view its value", this)); // 0: welcome
    m_stringEdit = new QPlainTextEdit();
    m_stack->addWidget(m_stringEdit); // 1: String
    setupListEditor();  // 2
    setupHashEditor();  // 3
    setupSetEditor();   // 4
    setupZSetEditor();  // 5
    m_stack->setCurrentIndex(0);
    mainLayout->addWidget(m_stack);

    // 按钮连接
    connect(m_saveBtn, &QPushButton::clicked, [this]() {
        if (m_currentType == "string") onSaveString();
        else if (m_currentType == "list") onSaveList();
        else if (m_currentType == "hash") onSaveHash();
        else if (m_currentType == "set") onSaveSet();
        else if (m_currentType == "zset") onSaveZSet();
    });
    connect(m_deleteBtn, &QPushButton::clicked,
            this, &ValueViewer::onDeleteKey);
    connect(m_refreshBtn, &QPushButton::clicked, [this]() {
        if (!m_currentKey.isEmpty())
            loadKey(m_currentKey, m_currentDb, m_currentType);
    });

    showInfoBar(false);
}

void ValueViewer::setupListEditor() {
    auto* page = new QWidget();
    auto* l = new QVBoxLayout(page);
    m_listTable = new QTableWidget(0, 2);
    m_listTable->setHorizontalHeaderLabels({"Index", "Value"});
    m_listTable->horizontalHeader()->setStretchLastSection(true);
    m_listTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    l->addWidget(m_listTable);
    auto* bl = new QHBoxLayout();
    m_listAddBtn = new QPushButton("+ Add");
    m_listRemoveBtn = new QPushButton("x Remove");
    bl->addWidget(m_listAddBtn); bl->addWidget(m_listRemoveBtn); bl->addStretch();
    l->addLayout(bl);
    connect(m_listAddBtn, &QPushButton::clicked, this, &ValueViewer::onListAddRow);
    connect(m_listRemoveBtn, &QPushButton::clicked, this, &ValueViewer::onListRemoveRow);
    m_stack->addWidget(page);
}

void ValueViewer::setupHashEditor() {
    auto* page = new QWidget();
    auto* l = new QVBoxLayout(page);
    m_hashTable = new QTableWidget(0, 2);
    m_hashTable->setHorizontalHeaderLabels({"Field", "Value"});
    m_hashTable->horizontalHeader()->setStretchLastSection(true);
    m_hashTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    l->addWidget(m_hashTable);
    auto* bl = new QHBoxLayout();
    m_hashAddBtn = new QPushButton("+ Add Field");
    m_hashRemoveBtn = new QPushButton("x Remove");
    bl->addWidget(m_hashAddBtn); bl->addWidget(m_hashRemoveBtn); bl->addStretch();
    l->addLayout(bl);
    connect(m_hashAddBtn, &QPushButton::clicked, this, &ValueViewer::onHashAddRow);
    connect(m_hashRemoveBtn, &QPushButton::clicked, this, &ValueViewer::onHashRemoveRow);
    m_stack->addWidget(page);
}

void ValueViewer::setupSetEditor() {
    auto* page = new QWidget();
    auto* l = new QVBoxLayout(page);
    m_setList = new QListWidget();
    l->addWidget(m_setList);
    auto* bl = new QHBoxLayout();
    m_setAddBtn = new QPushButton("+ Add Member");
    m_setRemoveBtn = new QPushButton("x Remove");
    bl->addWidget(m_setAddBtn); bl->addWidget(m_setRemoveBtn); bl->addStretch();
    l->addLayout(bl);
    connect(m_setAddBtn, &QPushButton::clicked, this, &ValueViewer::onSetAddMember);
    connect(m_setRemoveBtn, &QPushButton::clicked, this, &ValueViewer::onSetRemoveMember);
    m_stack->addWidget(page);
}

void ValueViewer::setupZSetEditor() {
    auto* page = new QWidget();
    auto* l = new QVBoxLayout(page);
    m_zsetTable = new QTableWidget(0, 2);
    m_zsetTable->setHorizontalHeaderLabels({"Score", "Member"});
    m_zsetTable->horizontalHeader()->setStretchLastSection(true);
    m_zsetTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    l->addWidget(m_zsetTable);
    auto* bl = new QHBoxLayout();
    m_zsetAddBtn = new QPushButton("+ Add");
    m_zsetRemoveBtn = new QPushButton("x Remove");
    bl->addWidget(m_zsetAddBtn); bl->addWidget(m_zsetRemoveBtn); bl->addStretch();
    l->addLayout(bl);
    connect(m_zsetAddBtn, &QPushButton::clicked, this, &ValueViewer::onZSetAddRow);
    connect(m_zsetRemoveBtn, &QPushButton::clicked, this, &ValueViewer::onZSetRemoveRow);
    m_stack->addWidget(page);
}

// ═══════════════════════════════════════════ 加载数据（使用回调）══════

void ValueViewer::loadKey(const QString& key, int db, const QString& type) {
    if (!m_client || !m_client->isConnected()) return;

    m_currentKey  = key;
    m_currentDb   = db;
    m_currentType = type;
    updateInfoBar(key, type);
    showInfoBar(true);

    if (type == "string") {
        m_stack->setCurrentIndex(1);
        m_client->sendCommand(
            QStringList() << "GET" << key,
            [this](const RespValue& v) {
                if (v.type == RespValue::STRING)
                    m_stringEdit->setPlainText(QString::fromStdString(v.str_val));
                else if (v.is_nil())
                    m_stringEdit->clear();
            });
    } else if (type == "list") {
        m_stack->setCurrentIndex(2);
        m_client->sendCommand(
            QStringList() << "LRANGE" << key << "0" << "-1",
            [this](const RespValue& v) {
                m_listTable->setRowCount(0);
                if (v.type == RespValue::ARRAY) {
                    for (size_t i = 0; i < v.array_val.size(); ++i) {
                        m_listTable->insertRow(i);
                        m_listTable->setItem(i, 0,
                            new QTableWidgetItem(QString::number(i)));
                        m_listTable->setItem(i, 1,
                            new QTableWidgetItem(
                                QString::fromStdString(v.array_val[i].str_val)));
                    }
                }
            });
    } else if (type == "hash") {
        m_stack->setCurrentIndex(3);
        m_client->sendCommand(
            QStringList() << "HGETALL" << key,
            [this](const RespValue& v) {
                m_hashTable->setRowCount(0);
                if (v.type == RespValue::ARRAY) {
                    for (size_t i = 0; i + 1 < v.array_val.size(); i += 2) {
                        int row = i / 2;
                        m_hashTable->insertRow(row);
                        m_hashTable->setItem(row, 0,
                            new QTableWidgetItem(
                                QString::fromStdString(v.array_val[i].str_val)));
                        m_hashTable->setItem(row, 1,
                            new QTableWidgetItem(
                                QString::fromStdString(v.array_val[i+1].str_val)));
                    }
                }
            });
    } else if (type == "set") {
        m_stack->setCurrentIndex(4);
        m_client->sendCommand(
            QStringList() << "SMEMBERS" << key,
            [this](const RespValue& v) {
                m_setList->clear();
                if (v.type == RespValue::ARRAY) {
                    for (auto& item : v.array_val)
                        m_setList->addItem(QString::fromStdString(item.str_val));
                }
            });
    } else if (type == "zset") {
        m_stack->setCurrentIndex(5);
        m_client->sendCommand(
            QStringList() << "ZRANGE" << key << "0" << "-1" << "WITHSCORES",
            [this](const RespValue& v) {
                m_zsetTable->setRowCount(0);
                if (v.type == RespValue::ARRAY) {
                    for (size_t i = 0; i + 1 < v.array_val.size(); i += 2) {
                        int row = i / 2;
                        m_zsetTable->insertRow(row);
                        m_zsetTable->setItem(row, 0,
                            new QTableWidgetItem(
                                QString::fromStdString(v.array_val[i+1].str_val))); // score
                        m_zsetTable->setItem(row, 1,
                            new QTableWidgetItem(
                                QString::fromStdString(v.array_val[i].str_val)));  // member
                    }
                }
            });
    }

    // TTL
    m_client->sendCommand(
        QStringList() << "TTL" << key,
        [this](const RespValue& v) {
            if (v.type == RespValue::INTEGER) {
                int ttl = (int)v.int_val;
                if (ttl == -1) m_ttlLabel->setText("TTL: permanent");
                else if (ttl == -2) m_ttlLabel->setText("TTL: (expired)");
                else m_ttlLabel->setText(QString("TTL: %1s").arg(ttl));
            }
        });
}

void ValueViewer::clear() {
    m_currentKey.clear();
    m_stack->setCurrentIndex(0);
    showInfoBar(false);
}

// ═══════════════════════════════════════════ 保存 ══════

void ValueViewer::onSaveString() {
    QString value = m_stringEdit->toPlainText();
    m_client->sendCommand(
        QStringList() << "SET" << m_currentKey << value);
    emit statusMessage("String saved");
}

void ValueViewer::onSaveList() {
    m_client->sendCommand(QStringList() << "DEL" << m_currentKey);
    QStringList values;
    for (int i = 0; i < m_listTable->rowCount(); ++i) {
        auto* item = m_listTable->item(i, 1);
        if (item) values.append(item->text());
    }
    if (!values.isEmpty())
        m_client->sendCommand(
            QStringList() << "RPUSH" << m_currentKey << values);
    emit statusMessage("List saved");
}

void ValueViewer::onSaveHash() {
    m_client->sendCommand(QStringList() << "DEL" << m_currentKey);
    for (int i = 0; i < m_hashTable->rowCount(); ++i) {
        auto* f = m_hashTable->item(i, 0);
        auto* v = m_hashTable->item(i, 1);
        if (f && v && !f->text().isEmpty())
            m_client->sendCommand(
                QStringList() << "HSET" << m_currentKey
                              << f->text() << v->text());
    }
    emit statusMessage("Hash saved");
}

void ValueViewer::onSaveSet() {
    m_client->sendCommand(QStringList() << "DEL" << m_currentKey);
    QStringList members;
    for (int i = 0; i < m_setList->count(); ++i)
        members.append(m_setList->item(i)->text());
    if (!members.isEmpty())
        m_client->sendCommand(
            QStringList() << "SADD" << m_currentKey << members);
    emit statusMessage("Set saved");
}

void ValueViewer::onSaveZSet() {
    m_client->sendCommand(QStringList() << "DEL" << m_currentKey);
    for (int i = 0; i < m_zsetTable->rowCount(); ++i) {
        auto* s = m_zsetTable->item(i, 0);
        auto* m = m_zsetTable->item(i, 1);
        if (s && m)
            m_client->sendCommand(
                QStringList() << "ZADD" << m_currentKey
                              << s->text() << m->text());
    }
    emit statusMessage("ZSet saved");
}

void ValueViewer::onDeleteKey() {
    if (QMessageBox::question(this, "Confirm Delete",
            QString("Delete key '%1'? This cannot be undone.")
                .arg(m_currentKey)) == QMessageBox::Yes) {
        m_client->sendCommand(QStringList() << "DEL" << m_currentKey);
        clear();
        emit statusMessage("Key deleted");
    }
}

// ═══════════════════════════════════════════ 行操作 ══════

void ValueViewer::onListAddRow() {
    int r = m_listTable->rowCount(); m_listTable->insertRow(r);
    m_listTable->setItem(r, 0, new QTableWidgetItem(QString::number(r)));
    m_listTable->setItem(r, 1, new QTableWidgetItem(""));
}
void ValueViewer::onListRemoveRow() {
    int r = m_listTable->currentRow(); if (r >= 0) m_listTable->removeRow(r);
    for (int i = 0; i < m_listTable->rowCount(); ++i)
        m_listTable->item(i, 0)->setText(QString::number(i));
}
void ValueViewer::onHashAddRow() {
    int r = m_hashTable->rowCount(); m_hashTable->insertRow(r);
    m_hashTable->setItem(r, 0, new QTableWidgetItem(""));
    m_hashTable->setItem(r, 1, new QTableWidgetItem(""));
}
void ValueViewer::onHashRemoveRow() {
    int r = m_hashTable->currentRow(); if (r >= 0) m_hashTable->removeRow(r);
}
void ValueViewer::onSetAddMember() {
    m_setList->addItem(""); m_setList->setCurrentRow(m_setList->count()-1);
    m_setList->editItem(m_setList->currentItem());
}
void ValueViewer::onSetRemoveMember() {
    int r = m_setList->currentRow(); if (r >= 0) delete m_setList->takeItem(r);
}
void ValueViewer::onZSetAddRow() {
    int r = m_zsetTable->rowCount(); m_zsetTable->insertRow(r);
    m_zsetTable->setItem(r, 0, new QTableWidgetItem("0.0"));
    m_zsetTable->setItem(r, 1, new QTableWidgetItem(""));
}
void ValueViewer::onZSetRemoveRow() {
    int r = m_zsetTable->currentRow(); if (r >= 0) m_zsetTable->removeRow(r);
}

// ═══════════════════════════════════════════ 辅助 ══════

void ValueViewer::showInfoBar(bool v) {
    m_keyLabel->setVisible(v); m_typeLabel->setVisible(v);
    m_ttlLabel->setVisible(v); m_saveBtn->setVisible(v);
    m_deleteBtn->setVisible(v); m_refreshBtn->setVisible(v);
}
void ValueViewer::updateInfoBar(const QString& key, const QString& type,
                                 const QString& ttl) {
    m_keyLabel->setText(QString("Key: %1").arg(key));
    m_typeLabel->setText(QString("Type: %1").arg(type));
    m_ttlLabel->setText(QString("TTL: %1").arg(ttl.isEmpty() ? "-" : ttl));
}
