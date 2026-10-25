#include "key_browser.h"
#include "redis_client.h"
#include "resp_codec.h"
#include <QHeaderView>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QDebug>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QDialog>
#include <QFormLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QIntValidator>
#include <QLabel>
#include <QFont>

KeyBrowser::KeyBrowser(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* toolbar = new QHBoxLayout();
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText("Filter keys (e.g. product:*)");
    m_addProductBtn = new QPushButton("➕ 添加商品", this);
    m_addProductBtn->setToolTip("添加新商品到数据库");
    m_refreshBtn = new QPushButton("🔄", this);
    m_refreshBtn->setToolTip("Refresh key list (F5)");
    m_refreshBtn->setMaximumWidth(40);
    toolbar->addWidget(m_filterEdit);
    toolbar->addWidget(m_addProductBtn);
    toolbar->addWidget(m_refreshBtn);
    layout->addLayout(toolbar);

    m_tree = new QTreeWidget(this);
    m_tree->setHeaderLabels({"Key / Database"});
    m_tree->setRootIsDecorated(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->header()->setStretchLastSection(true);
    layout->addWidget(m_tree);

    connect(m_refreshBtn, &QPushButton::clicked, this, &KeyBrowser::refresh);
    connect(m_addProductBtn, &QPushButton::clicked, this, &KeyBrowser::onAddProduct);
    connect(m_tree, &QTreeWidget::itemClicked,
            this, &KeyBrowser::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked,
            this, &KeyBrowser::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested,
            this, &KeyBrowser::onContextMenu);
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, &KeyBrowser::onFilterChanged);
}

void KeyBrowser::setClient(RedisClient* client) {
    m_client = client;
}

void KeyBrowser::refresh() {
    m_tree->clear();
    m_loadedKeys.clear();
    ensureDbRoot(m_currentDb);
    loadKeysForDb(m_currentDb);
}

// ── 使用 KEYS 命令加载，用回调处理响应 ──
void KeyBrowser::loadKeysForDb(int db) {
    if (!m_client || !m_client->isConnected()) return;

    m_client->sendCommand(
        QStringList() << "SELECT" << QString::number(db),
        nullptr);

    QTreeWidgetItem* dbRoot = ensureDbRoot(db);

    // 顶层分类
    QTreeWidgetItem* productNormalCat = ensureCategory(
        "📦 普通商品", dbRoot);
    QTreeWidgetItem* productFlashCat = ensureCategory(
        "⚡ 秒杀商品", dbRoot);
    QTreeWidgetItem* flashCat = ensureCategory(
        "⚙️ 秒杀配置", dbRoot);
    QTreeWidgetItem* orderCat = ensureCategory(
        "📋 订单队列", dbRoot);
    QTreeWidgetItem* statsCat = ensureCategory(
        "📊 运营统计", dbRoot);
    QTreeWidgetItem* rankCat = ensureCategory(
        "🏆 销售排行", dbRoot);
    QTreeWidgetItem* otherCat = ensureCategory(
        "📁 其他", dbRoot);

    // 秒杀配置下的子分类
    QTreeWidgetItem* flashSaleCfg = ensureCategory(
        "📋 活动配置", flashCat);
    QTreeWidgetItem* flashDetails = ensureCategory(
        "📦 秒杀详情", flashCat);
    QTreeWidgetItem* flashCounters = ensureCategory(
        "🔢 库存计数器", flashCat);

    m_client->sendCommand(
        QStringList() << "KEYS" << "*",
        [this, db, dbRoot, productNormalCat, productFlashCat,
         flashCat, flashSaleCfg, flashDetails, flashCounters,
         orderCat, statsCat, rankCat, otherCat]
        (const RespValue& value) {
            if (value.type != RespValue::ARRAY) return;

            for (const auto& item : value.array_val) {
                if (item.type != RespValue::STRING) continue;
                QString key = QString::fromStdString(item.str_val);
                if (m_loadedKeys.contains(key)) continue;
                m_loadedKeys.insert(key);

                // 跳过内部辅助 key
                if (key == "product:all") continue;

                QString cat = categoryForKey(key);

                // ── 商品：异步判断 flash 类型 ──────────────
                if (cat == "product") {
                    QString pid = key.mid(8);
                    // 检查是否有秒杀条目
                    m_client->sendCommand(
                        QStringList() << "EXISTS"
                            << QString("flash:1:product:%1").arg(pid),
                        [this, key, pid, db,
                         productNormalCat, productFlashCat]
                        (const RespValue& ev) {
                            bool isFlash = (ev.type == RespValue::INTEGER
                                            && ev.int_val == 1);
                            QTreeWidgetItem* parent =
                                isFlash ? productFlashCat : productNormalCat;

                            auto* treeItem = new QTreeWidgetItem(parent, {key});
                            treeItem->setIcon(0, iconForType("unknown"));
                            treeItem->setData(0, Qt::UserRole, key);
                            treeItem->setData(0, Qt::UserRole + 2, db);
                            treeItem->setData(0, Qt::UserRole + 3, isFlash);

                            // TYPE → 图标 + 友好名称
                            m_client->sendCommand(
                                QStringList() << "TYPE" << key,
                                [this, treeItem, key, isFlash]
                                (const RespValue& typeVal) {
                                    QString type = "unknown";
                                    if (typeVal.type == RespValue::STRING) {
                                        type = QString::fromStdString(typeVal.str_val);
                                        treeItem->setIcon(0, iconForType(type));
                                        treeItem->setData(0, Qt::UserRole + 1, type);
                                    }
                                    // 商品：名称 + 库存
                                    loadProductDisplay(treeItem, key, isFlash);
                                });
                        });
                    continue;
                }

                // ── 非商品：直接创建 ──────────────────────
                QTreeWidgetItem* parent = otherCat;
                if (cat == "flash") {
                    if (key.startsWith("flash:sale:"))
                        parent = flashSaleCfg;
                    else if (key.endsWith(":stock"))
                        parent = flashCounters;
                    else
                        parent = flashDetails;
                } else if (cat == "order") {
                    parent = orderCat;
                } else if (cat == "stats") {
                    parent = statsCat;
                } else if (cat == "rank") {
                    parent = rankCat;
                }

                auto* treeItem = new QTreeWidgetItem(parent, {key});
                treeItem->setIcon(0, iconForType("unknown"));
                treeItem->setData(0, Qt::UserRole, key);
                treeItem->setData(0, Qt::UserRole + 2, db);

                // TYPE → 图标 + 友好名称
                m_client->sendCommand(
                    QStringList() << "TYPE" << key,
                    [this, treeItem, key](const RespValue& typeVal) {
                        QString type = "unknown";
                        if (typeVal.type == RespValue::STRING) {
                            type = QString::fromStdString(typeVal.str_val);
                            treeItem->setIcon(0, iconForType(type));
                            treeItem->setData(0, Qt::UserRole + 1, type);
                        }
                        loadNonProductDisplay(treeItem, key);
                    });
            }
        });
}

// ── 商品友好名称：名称 + 库存 + 类型标记 ──
void KeyBrowser::loadProductDisplay(QTreeWidgetItem* treeItem,
                                     const QString& key, bool isFlash) {
    m_client->sendCommand(
        QStringList() << "HGET" << key << "name",
        [this, treeItem, key, isFlash](const RespValue& nv) {
            QString name;
            if (nv.type == RespValue::STRING)
                name = QString::fromStdString(nv.str_val);
            m_client->sendCommand(
                QStringList() << "HGET" << key << "stock",
                [treeItem, key, name, isFlash](const RespValue& sv) {
                    QString stock = "?";
                    if (sv.type == RespValue::STRING)
                        stock = QString::fromStdString(sv.str_val);
                    QString tag = isFlash ? " ⚡秒杀" : "";
                    if (name.isEmpty())
                        treeItem->setText(0,
                            QString("%1%2  [库存: %3]")
                                .arg(key, tag, stock));
                    else
                        treeItem->setText(0,
                            QString("%1 → %2%3  [库存: %4]")
                                .arg(key, name, tag, stock));
                });
        });
}

// ── 非商品 key 的友好名称 ──
void KeyBrowser::loadNonProductDisplay(QTreeWidgetItem* treeItem,
                                        const QString& key) {
    QString cat = categoryForKey(key);

    if (key.startsWith("flash:sale:")) {
        m_client->sendCommand(
            QStringList() << "HGET" << key << "title",
            [treeItem, key](const RespValue& tv) {
                if (tv.type == RespValue::STRING)
                    treeItem->setText(0,
                        QString("%1 → %2")
                            .arg(key)
                            .arg(QString::fromStdString(tv.str_val)));
            });
        return;
    }

    if (cat == "flash" && key.endsWith(":stock")) {
        m_client->sendCommand(
            QStringList() << "GET" << key,
            [treeItem, key](const RespValue& gv) {
                if (gv.type == RespValue::STRING)
                    treeItem->setText(0,
                        QString("%1  =  %2")
                            .arg(key)
                            .arg(QString::fromStdString(gv.str_val)));
            });
        return;
    }

    if (cat == "flash" && key.contains(":product:")) {
        m_client->sendCommand(
            QStringList() << "HGET" << key << "flash_stock",
            [treeItem, key](const RespValue& fv) {
                QString fs = "?";
                if (fv.type == RespValue::STRING)
                    fs = QString::fromStdString(fv.str_val);
                treeItem->setText(0,
                    QString("%1  [秒杀库存: %2]").arg(key, fs));
            });
        return;
    }

    if (cat == "order") {
        m_client->sendCommand(
            QStringList() << "LLEN" << key,
            [treeItem, key](const RespValue& lv) {
                if (lv.type == RespValue::INTEGER)
                    treeItem->setText(0,
                        QString("%1  [%2 单待处理]")
                            .arg(key).arg(lv.int_val));
            });
        return;
    }

    if (cat == "stats") {
        m_client->sendCommand(
            QStringList() << "GET" << key,
            [treeItem, key](const RespValue& gv) {
                if (gv.type == RespValue::STRING) {
                    QString label = "值";
                    if (key.contains("revenue"))      label = "¥";
                    else if (key.contains("orders"))  label = "单";
                    else if (key.contains("visitors"))label = "人";
                    treeItem->setText(0,
                        QString("%1  [%2%3]")
                            .arg(key, label,
                                QString::fromStdString(gv.str_val)));
                }
            });
        return;
    }

    if (cat == "rank") {
        m_client->sendCommand(
            QStringList() << "ZCARD" << key,
            [treeItem, key](const RespValue& cv) {
                if (cv.type == RespValue::INTEGER)
                    treeItem->setText(0,
                        QString("%1  [%2 个商品上榜]")
                            .arg(key).arg(cv.int_val));
            });
        return;
    }
}

// ── Tree 交互 ──

void KeyBrowser::onItemClicked(QTreeWidgetItem* item, int) {
    if (item && item->childCount() == 0 && item->parent()) {
        QString key = item->data(0, Qt::UserRole).toString();
        if (key.isEmpty()) return;
        QString type = item->data(0, Qt::UserRole + 1).toString();
        if (type.isEmpty()) type = "string";
        int db = item->data(0, Qt::UserRole + 2).toInt();
        emit keySelected(key, db, type);
    }
}

void KeyBrowser::onItemDoubleClicked(QTreeWidgetItem* item, int col) {
    onItemClicked(item, col);
}

void KeyBrowser::onContextMenu(const QPoint& pos) {
    QTreeWidgetItem* item = m_tree->itemAt(pos);
    if (!item || item->childCount() > 0 || !item->parent()) return;

    QString key = item->data(0, Qt::UserRole).toString();
    if (key.isEmpty()) return;

    QMenu menu(this);
    menu.addAction("Open", [this, item]() {
        onItemDoubleClicked(item, 0);
    });
    menu.addAction("Copy Key Name", [key]() {
        QApplication::clipboard()->setText(key);
    });
    menu.addSeparator();
    // 商品快速操作
    if (key.startsWith("product:") && !key.contains(":product:")) {
        // 类型切换
        bool isFlash = item->data(0, Qt::UserRole + 3).toBool();
        QString pid = key.mid(8);
        if (isFlash) {
            menu.addAction("🔽 设为普通商品", [this, key, pid]() {
                // 删除秒杀条目 + 计数器
                QString fKey = QString("flash:1:product:%1").arg(pid);
                m_client->sendCommand(QStringList() << "DEL" << fKey);
                m_client->sendCommand(
                    QStringList() << "DEL"
                        << QString("flash:1:product:%1:stock").arg(pid));
                QTimer::singleShot(100, this, [this]() {
                    refresh();
                    emit keysChanged();
                });
            });
        } else {
            menu.addAction("🔺 设为秒杀商品", [this, key, pid]() {
                // 查询基础信息后创建秒杀条目
                m_client->sendCommand(
                    QStringList() << "HGETALL" << key,
                    [this, pid, key](const RespValue& pv) {
                        int baseStock = 0;
                        QString basePrice;
                        if (pv.type == RespValue::ARRAY) {
                            auto& arr = pv.array_val;
                            for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                                QString f = QString::fromStdString(arr[i].str_val);
                                QString v = QString::fromStdString(arr[i+1].str_val);
                                if (f == "price") basePrice = v;
                                else if (f == "stock") baseStock = v.toInt();
                            }
                        }
                        if (basePrice.isEmpty()) basePrice = "0";
                        QString fKey = QString("flash:1:product:%1").arg(pid);
                        m_client->sendCommand(
                            QStringList() << "HSET" << fKey
                                << "flash_price" << basePrice
                                << "flash_stock" << QString::number(baseStock)
                                << "sold" << "0");
                        m_client->sendCommand(
                            QStringList() << "SET"
                                << QString("flash:1:product:%1:stock").arg(pid)
                                << QString::number(baseStock));
                        QTimer::singleShot(100, this, [this]() {
                            refresh();
                            emit keysChanged();
                        });
                    });
            });
        }
        menu.addSeparator();

        menu.addAction("Adjust Stock...", [this, key]() {
            m_client->sendCommand(
                QStringList() << "HGET" << key << "stock",
                [this, key](const RespValue& v) {
                    int curStock = 0;
                    if (v.type == RespValue::STRING)
                        curStock = QString::fromStdString(v.str_val).toInt();
                    // 延迟弹窗：避免在 RESP 回调里嵌套 event loop
                    QTimer::singleShot(0, this, [this, key, curStock]() {
                        bool ok;
                        int newStock = QInputDialog::getInt(this,
                            QString("Adjust Stock — %1").arg(key),
                            "New stock value:",
                            curStock, 0, 999999, 1, &ok);
                        if (ok && newStock != curStock) {
                            // 1) 更新基础商品库存
                            m_client->sendCommand(
                                QStringList() << "HSET" << key << "stock"
                                              << QString::number(newStock),
                                [this, key, newStock](const RespValue&) {
                                    // 2) 同步更新秒杀库存计数器
                                    QString pid = key.mid(8); // "product:1001" → "1001"
                                    m_client->sendCommand(
                                        QStringList() << "KEYS"
                                            << QString("flash:*:product:%1:stock").arg(pid),
                                        [this, pid, newStock](const RespValue& kv) {
                                            if (kv.type == RespValue::ARRAY) {
                                                for (const auto& item : kv.array_val) {
                                                    if (item.type != RespValue::STRING) continue;
                                                    QString ck = QString::fromStdString(item.str_val);
                                                    // SET counter to new stock
                                                    m_client->sendCommand(
                                                        QStringList() << "SET" << ck
                                                                      << QString::number(newStock));
                                                    // Also expand flash_stock capacity
                                                    QString fpk = ck; fpk.chop(6); // remove ":stock"
                                                    m_client->sendCommand(
                                                        QStringList() << "HSET" << fpk
                                                                      << "flash_stock"
                                                                      << QString::number(newStock));
                                                }
                                            }
                                            QTimer::singleShot(0, this,
                                                [this]() { refresh(); });
                                        });
                                });
                        }
                    });
                });
        });
    }
    menu.addSeparator();
    menu.addAction("Delete", [this, key]() {
        if (QMessageBox::question(this, "Confirm Delete",
                QString("Delete key '%1'?").arg(key))
            == QMessageBox::Yes) {
            m_client->sendCommand(QStringList() << "DEL" << key);
            // 如果是基础商品，同时删除关联的秒杀数据
            if (key.startsWith("product:") && !key.contains(":product:")) {
                QString pid = key.mid(8);
                m_client->sendCommand(QStringList() << "DEL"
                    << QString("flash:1:product:%1").arg(pid));
                m_client->sendCommand(QStringList() << "DEL"
                    << QString("flash:1:product:%1:stock").arg(pid));
            }
            refresh();
            emit keysChanged();
        }
    });
    menu.addAction("Rename...", [this, key]() {
        bool ok;
        QString newName = QInputDialog::getText(this, "Rename Key",
            "New name:", QLineEdit::Normal, key, &ok);
        if (ok && !newName.isEmpty() && newName != key) {
            m_client->sendCommand(
                QStringList() << "RENAME" << key << newName);
            refresh();
        }
    });
    menu.addAction("Set TTL...", [this, key]() {
        bool ok;
        int seconds = QInputDialog::getInt(this, "Set TTL",
            "Expiration (seconds):", 3600, -1, 2147483647, 1, &ok);
        if (ok) {
            m_client->sendCommand(
                QStringList() << "EXPIRE" << key
                              << QString::number(seconds));
        }
    });

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// ═══════════════════════════════════════════════════════════════
//  添加商品对话框
// ═══════════════════════════════════════════════════════════════

void KeyBrowser::onAddProduct() {
    if (!m_client || !m_client->isConnected()) {
        QMessageBox::warning(this, "Not Connected",
            "请先启动服务器并连接。");
        return;
    }

    // 先找到下一个可用的 product ID
    m_client->sendCommand(
        QStringList() << "KEYS" << "product:*",
        [this](const RespValue& v) {
            int maxId = 1000;
            if (v.type == RespValue::ARRAY) {
                for (const auto& item : v.array_val) {
                    if (item.type != RespValue::STRING) continue;
                    QString key = QString::fromStdString(item.str_val);
                    // 提取 product:XXXX 中的数字部分
                    if (key.startsWith("product:") &&
                        !key.contains(":product:")) {
                        int id = key.mid(8).toInt();
                        if (id > maxId) maxId = id;
                    }
                }
            }
            int nextId = maxId + 1;
            QString newKey = QString("product:%1").arg(nextId);

            // 延迟弹窗
            QTimer::singleShot(0, this, [this, newKey, nextId]() {
                showAddProductDialog(newKey);
            });
        });
}

void KeyBrowser::showAddProductDialog(const QString& newKey) {
    QDialog dlg(this);
    dlg.setWindowTitle("添加新商品");
    dlg.setMinimumWidth(400);

    auto* form = new QFormLayout(&dlg);

    auto* idLabel = new QLabel(newKey);
    idLabel->setStyleSheet("color: #888; font-weight: bold;");
    form->addRow("商品 ID:", idLabel);

    auto* nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText("例如: iPhone 15 Pro Max");
    form->addRow("名称 *:", nameEdit);

    auto* priceEdit = new QLineEdit();
    priceEdit->setPlaceholderText("例如: 8999");
    priceEdit->setValidator(new QIntValidator(0, 99999999, priceEdit));
    form->addRow("价格 *:", priceEdit);

    auto* stockEdit = new QLineEdit();
    stockEdit->setPlaceholderText("例如: 200");
    stockEdit->setValidator(new QIntValidator(0, 999999, stockEdit));
    form->addRow("库存 *:", stockEdit);

    auto* soldEdit = new QLineEdit();
    soldEdit->setText("0");
    soldEdit->setValidator(new QIntValidator(0, 999999, soldEdit));
    form->addRow("已售:", soldEdit);

    auto* categoryCombo = new QComboBox();
    categoryCombo->setEditable(true);
    categoryCombo->addItems({
        "手机数码", "电脑办公", "耳机音箱",
        "智能穿戴", "平板电脑", "生活电器",
        "图书音像", "食品生鲜", "服装鞋帽"
    });
    form->addRow("品类 *:", categoryCombo);

    auto* brandEdit = new QLineEdit();
    brandEdit->setPlaceholderText("例如: Apple");
    form->addRow("品牌 *:", brandEdit);

    auto* typeCombo = new QComboBox();
    typeCombo->addItems({"普通商品", "秒杀商品"});
    typeCombo->setCurrentIndex(0);
    form->addRow("商品类型:", typeCombo);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&]() {
        // ── 校验必填项 ──────────────────────────────
        QString name = nameEdit->text().trimmed();
        QString price = priceEdit->text().trimmed();
        QString stock = stockEdit->text().trimmed();
        QString sold = soldEdit->text().trimmed();
        QString category = categoryCombo->currentText().trimmed();
        QString brand = brandEdit->text().trimmed();
        bool isFlash = (typeCombo->currentIndex() == 1);

        if (name.isEmpty() || price.isEmpty() || stock.isEmpty() ||
            category.isEmpty() || brand.isEmpty()) {
            QMessageBox::warning(&dlg, "输入不完整",
                "以下字段为必填项，请检查：\n\n"
                "  · 名称\n"
                "  · 价格\n"
                "  · 库存\n"
                "  · 品类\n"
                "  · 品牌");
            return;  // 不关闭对话框
        }

        // ── 写入基础商品 ────────────────────────────
        m_client->sendCommand(
            QStringList() << "HSET" << newKey
                << "name"     << name
                << "price"    << price
                << "stock"    << stock
                << "sold"     << (sold.isEmpty() ? "0" : sold)
                << "category" << category
                << "brand"    << brand);
        // ── 秒杀商品：额外创建 flash entry + 计数器 ──
        if (isFlash) {
            QString pid = newKey.mid(8);
            m_client->sendCommand(
                QStringList() << "HSET"
                    << QString("flash:1:product:%1").arg(pid)
                    << "flash_price" << price
                    << "flash_stock" << stock
                    << "sold" << (sold.isEmpty() ? "0" : sold));
            m_client->sendCommand(
                QStringList() << "SET"
                    << QString("flash:1:product:%1:stock").arg(pid)
                    << stock);
        }

        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        QTimer::singleShot(100, this, [this]() {
            refresh();
            emit keysChanged();  // 通知业务面板刷新商品列表
        });
    }
}

void KeyBrowser::onFilterChanged(const QString& text) {
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* dbRoot = m_tree->topLevelItem(i);
        for (int j = 0; j < dbRoot->childCount(); ++j) {
            QTreeWidgetItem* cat = dbRoot->child(j);
            bool anyVisible = false;
            for (int k = 0; k < cat->childCount(); ++k) {
                QTreeWidgetItem* keyItem = cat->child(k);
                bool match = keyItem->text(0).contains(
                    text, Qt::CaseInsensitive);
                keyItem->setHidden(!match);
                if (match) anyVisible = true;
            }
            cat->setHidden(!anyVisible);
        }
    }
}

QTreeWidgetItem* KeyBrowser::ensureDbRoot(int db) {
    QString dbName = QString("DB%1").arg(db);
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        if (m_tree->topLevelItem(i)->text(0) == dbName) {
            return m_tree->topLevelItem(i);
        }
    }
    auto* root = new QTreeWidgetItem(m_tree, {dbName});
    root->setData(0, Qt::UserRole, db);
    root->setExpanded(true);
    return root;
}

QTreeWidgetItem* KeyBrowser::ensureCategory(const QString& title,
                                              QTreeWidgetItem* parent) {
    for (int i = 0; i < parent->childCount(); ++i) {
        if (parent->child(i)->text(0) == title)
            return parent->child(i);
    }
    auto* cat = new QTreeWidgetItem(parent, {title});
    cat->setExpanded(true);
    cat->setFlags(cat->flags() & ~Qt::ItemIsSelectable);
    QFont boldFont = cat->font(0);
    boldFont.setBold(true);
    cat->setFont(0, boldFont);
    return cat;
}

QString KeyBrowser::categoryForKey(const QString& key) const {
    if (key.startsWith("product:") && !key.contains(":product:"))
        return "product";
    if (key.startsWith("flash:"))
        return "flash";
    if (key.startsWith("orders:"))
        return "order";
    if (key.startsWith("stats:"))
        return "stats";
    if (key.startsWith("sales:"))
        return "rank";
    return QString();  // other: stay under dbRoot
}

QIcon KeyBrowser::iconForType(const QString& type) const {
    QPixmap pix(16, 16);
    QPainter painter(&pix);
    if (type == "string")      painter.fillRect(0,0,16,16, QColor("#4CAF50"));
    else if (type == "list")   painter.fillRect(0,0,16,16, QColor("#2196F3"));
    else if (type == "hash")   painter.fillRect(0,0,16,16, QColor("#FF9800"));
    else if (type == "set")    painter.fillRect(0,0,16,16, QColor("#9C27B0"));
    else if (type == "zset")   painter.fillRect(0,0,16,16, QColor("#F44336"));
    else                       painter.fillRect(0,0,16,16, QColor("#9E9E9E"));
    painter.end();
    return QIcon(pix);
}
