#include "business_panel.h"
#include "redis_client.h"
#include "resp_codec.h"
#include "log.h"
#include <QGroupBox>
#include <QHBoxLayout>
#include <QScrollBar>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdlib>
#include <ctime>

// ═══════════════════════════════════════════════════════════════
//  UI 搭建
// ═══════════════════════════════════════════════════════════════

BusinessPanel::BusinessPanel(QWidget* parent) : QWidget(parent) {
    std::srand(std::time(nullptr));
    setupUi();
}

void BusinessPanel::setClient(RedisClient* client) {
    m_client = client;
    if (m_client && m_client->isConnected()) {
        m_statusLabel->setText("已连接，加载数据中...");
        m_statusLabel->setStyleSheet("color: #FF9800; padding: 2px;");
        refresh();
    } else {
        m_statusLabel->setText("等待连接...");
        m_statusLabel->setStyleSheet("color: #888; font-style: italic; padding: 2px;");
    }
}

void BusinessPanel::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // ── 状态栏 ──────────────────────────────────────
    auto* statusRow = new QHBoxLayout();
    m_statusLabel = new QLabel("等待连接...");
    m_statusLabel->setStyleSheet(
        "color: #888; font-style: italic; padding: 2px;");
    m_refreshBtn = new QPushButton("🔄 刷新数据");
    m_refreshBtn->setToolTip("手动重新加载秒杀活动和商品数据");
    m_refreshBtn->setMaximumWidth(120);
    statusRow->addWidget(m_statusLabel, 1);
    statusRow->addWidget(m_refreshBtn);
    mainLayout->addLayout(statusRow);

    // ── 秒杀活动选择 ──────────────────────────────────
    auto* saleGroup = new QGroupBox("秒杀活动");
    auto* saleLayout = new QVBoxLayout(saleGroup);

    auto* topRow = new QHBoxLayout();
    topRow->addWidget(new QLabel("活动:"));
    m_flashSaleCombo = new QComboBox();
    m_flashSaleCombo->setMinimumWidth(250);
    topRow->addWidget(m_flashSaleCombo, 1);

    topRow->addWidget(new QLabel("商品:"));
    m_productCombo = new QComboBox();
    m_productCombo->setMinimumWidth(200);
    topRow->addWidget(m_productCombo, 1);
    topRow->addStretch();
    saleLayout->addLayout(topRow);

    // ── 商品实时信息 ──────────────────────────────────
    auto* infoRow = new QHBoxLayout();
    m_flashPriceLabel = new QLabel("秒杀价: -");
    m_flashPriceLabel->setStyleSheet("font-weight: bold; color: #e53935; font-size: 14px;");
    m_stockLabel = new QLabel("剩余库存: -");
    m_soldLabel = new QLabel("已售: -");
    infoRow->addWidget(m_flashPriceLabel);
    infoRow->addWidget(m_stockLabel);
    infoRow->addWidget(m_soldLabel);
    infoRow->addStretch();
    saleLayout->addLayout(infoRow);

    // 库存进度条
    m_stockProgress = new QProgressBar();
    m_stockProgress->setRange(0, 100);
    m_stockProgress->setValue(0);
    m_stockProgress->setTextVisible(true);
    m_stockProgress->setFormat("库存消耗: %p%");
    saleLayout->addWidget(m_stockProgress);

    mainLayout->addWidget(saleGroup);

    // ── 下单操作 ──────────────────────────────────────
    auto* orderGroup = new QGroupBox("模拟下单");
    auto* orderLayout = new QHBoxLayout(orderGroup);

    orderLayout->addWidget(new QLabel("用户 ID:"));
    m_userIdEdit = new QLineEdit();
    m_userIdEdit->setPlaceholderText("输入用户ID或点击随机生成");
    m_userIdEdit->setMinimumWidth(120);
    orderLayout->addWidget(m_userIdEdit);

    m_randomUserBtn = new QPushButton("🎲 随机");
    m_randomUserBtn->setToolTip("随机生成一个用户ID (10001-99999)");
    orderLayout->addWidget(m_randomUserBtn);

    orderLayout->addSpacing(20);

    m_orderBtn = new QPushButton("⚡ 模拟下单");
    m_orderBtn->setStyleSheet(
        "QPushButton { background-color: #4CAF50; color: white; "
        "font-weight: bold; padding: 6px 16px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #388E3C; }"
        "QPushButton:disabled { background-color: #9E9E9E; }");
    m_orderBtn->setMinimumWidth(120);
    orderLayout->addWidget(m_orderBtn);

    orderLayout->addWidget(new QLabel("  压测数量:"));
    m_batchCountSpin = new QSpinBox();
    m_batchCountSpin->setRange(1, 100000);
    m_batchCountSpin->setValue(100);
    m_batchCountSpin->setSuffix(" 单");
    m_batchCountSpin->setMinimumWidth(90);
    orderLayout->addWidget(m_batchCountSpin);

    m_batchBtn = new QPushButton("🔥 批量压测");
    m_batchBtn->setStyleSheet(
        "QPushButton { background-color: #FF5722; color: white; "
        "font-weight: bold; padding: 6px 16px; border-radius: 4px; }"
        "QPushButton:hover { background-color: #D84315; }"
        "QPushButton:disabled { background-color: #9E9E9E; }");
    m_batchBtn->setMinimumWidth(150);
    orderLayout->addWidget(m_batchBtn);

    orderLayout->addStretch();
    mainLayout->addWidget(orderGroup);

    // ── 操作日志 ──────────────────────────────────────
    auto* logGroup = new QGroupBox("操作日志");
    auto* logLayout = new QVBoxLayout(logGroup);

    m_logView = new QTextEdit();
    m_logView->setReadOnly(true);
    m_logView->setFont(QFont("monospace", 9));
    m_logView->setStyleSheet(
        "QTextEdit { background-color: #1e1e1e; color: #d4d4d4; "
        "border: 1px solid #333; padding: 4px; }");
    m_logView->document()->setMaximumBlockCount(500);
    logLayout->addWidget(m_logView);

    // 清空日志按钮
    auto* logBtnRow = new QHBoxLayout();
    logBtnRow->addStretch();
    auto* clearLogBtn = new QPushButton("清空日志");
    connect(clearLogBtn, &QPushButton::clicked, [this]() { m_logView->clear(); });
    logBtnRow->addWidget(clearLogBtn);
    logLayout->addLayout(logBtnRow);

    mainLayout->addWidget(logGroup, 1);  // stretch factor 1: 占剩余空间

    // ── 信号连接 ──────────────────────────────────────
    connect(m_flashSaleCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BusinessPanel::onFlashSaleChanged);
    connect(m_productCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &BusinessPanel::onProductChanged);
    connect(m_orderBtn, &QPushButton::clicked,
            this, &BusinessPanel::onPlaceOrder);
    connect(m_batchBtn, &QPushButton::clicked,
            this, &BusinessPanel::onBatchStressTest);
    connect(m_randomUserBtn, &QPushButton::clicked,
            this, &BusinessPanel::onRandomUser);
    connect(m_refreshBtn, &QPushButton::clicked,
            this, &BusinessPanel::refresh);

    // 初始状态
    m_orderBtn->setEnabled(false);
    m_batchBtn->setEnabled(false);
}

// ═══════════════════════════════════════════════════════════════
//  数据加载
// ═══════════════════════════════════════════════════════════════

void BusinessPanel::refresh() {
    if (!m_client || !m_client->isConnected()) {
        m_statusLabel->setText("未连接 — 请先启动服务器");
        m_statusLabel->setStyleSheet("color: #f44747; font-weight: bold; padding: 2px;");
        return;
    }
    m_statusLabel->setText("正在加载秒杀活动...");
    m_statusLabel->setStyleSheet("color: #FF9800; padding: 2px;");
    appendLog("正在从 Redis 加载业务数据...");
    loadFlashSales();
}

void BusinessPanel::loadFlashSales() {
    // 扫描所有秒杀活动 key: flash:sale:*
    m_client->sendCommand(
        QStringList() << "KEYS" << "flash:sale:*",
        [this](const RespValue& v) {
            if (v.type != RespValue::ARRAY) {
                m_statusLabel->setText("数据加载失败 — 请点击刷新重试");
                m_statusLabel->setStyleSheet("color: #f44747; font-weight: bold; padding: 2px;");
                appendLog("❌ KEYS 命令响应异常，请确认种子数据已写入", true);
                return;
            }

            // 收集所有 sale key
            QStringList saleKeys;
            for (const auto& item : v.array_val) {
                if (item.type == RespValue::STRING)
                    saleKeys << QString::fromStdString(item.str_val);
            }

            if (saleKeys.isEmpty()) {
                m_statusLabel->setText("无秒杀数据 — 请确认已连接并启动服务器");
                m_statusLabel->setStyleSheet("color: #f44747; font-weight: bold; padding: 2px;");
                appendLog("⚠ 未找到 flash:sale:* 数据，请检查种子数据是否写入成功", true);
                return;
            }

            appendLog(QString("找到 %1 个秒杀活动，正在加载详情...").arg(saleKeys.size()));

            m_flashSaleCombo->blockSignals(true);
            m_flashSaleCombo->clear();

            // 共享计数器：追踪所有 HGET 完成
            auto pending = std::make_shared<int>(saleKeys.size());

            for (const QString& key : saleKeys) {
                m_client->sendCommand(
                    QStringList() << "HGET" << key << "title",
                    [this, key, pending](const RespValue& titleVal) {
                        QString title = titleVal.type == RespValue::STRING
                            ? QString::fromStdString(titleVal.str_val)
                            : key;
                        m_flashSaleCombo->addItem(
                            QString("%1  [%2]").arg(title, key), key);

                        (*pending)--;
                        // 所有 HGET 完成后统一处理
                        if (*pending == 0) {
                            m_flashSaleCombo->blockSignals(false);
                            if (m_flashSaleCombo->count() > 0) {
                                appendLog(QString("✅ 已加载 %1 个秒杀活动")
                                    .arg(m_flashSaleCombo->count()));
                                m_statusLabel->setText("数据就绪");
                                m_statusLabel->setStyleSheet(
                                    "color: #4CAF50; font-weight: bold; padding: 2px;");
                                onFlashSaleChanged(0);
                            }
                        }
                    });
            }
        });
}

void BusinessPanel::loadProductsForSale(const QString& saleId) {
    QString saleNum = saleId.section(':', -1);
    appendLog(QString("正在加载全部商品..."));

    // 扫描全部 product:* 键（不再依赖秒杀活动的 product_ids 字段）
    m_client->sendCommand(
        QStringList() << "KEYS" << "product:*",
        [this, saleNum](const RespValue& v) {
            if (v.type != RespValue::ARRAY) {
                appendLog("❌ 获取商品列表失败", true);
                return;
            }

            // 过滤：只保留 product:XXXX（排除 flash:X:product:XXXX）
            QStringList pids;
            for (const auto& item : v.array_val) {
                if (item.type != RespValue::STRING) continue;
                QString key = QString::fromStdString(item.str_val);
                if (key.startsWith("product:") && !key.contains(":product:"))
                    pids << key.mid(8);  // "product:1001" → "1001"
            }

            if (pids.isEmpty()) {
                m_productCombo->blockSignals(false);
                appendLog("⚠ 没有找到商品", true);
                return;
            }

            appendLog(QString("找到 %1 件商品，正在加载详情...").arg(pids.size()));

            m_products.clear();
            m_productCombo->blockSignals(true);
            m_productCombo->clear();

            auto pending = std::make_shared<int>(pids.size());

            for (const QString& pid : pids) {
                QString productKey = QString("product:%1").arg(pid);

                // 获取商品基础信息
                m_client->sendCommand(
                    QStringList() << "HGETALL" << productKey,
                    [this, pid, saleNum, pending](const RespValue& pv) {
                        FlashProduct fp;
                        fp.productId = pid;
                        int baseStock = 0;

                        if (pv.type == RespValue::ARRAY) {
                            auto& arr = pv.array_val;
                            for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                                QString f = QString::fromStdString(arr[i].str_val);
                                QString val = QString::fromStdString(arr[i+1].str_val);
                                if (f == "name")     fp.productName = val;
                                else if (f == "price")    fp.flashPrice = val;
                                else if (f == "stock")    baseStock = val.toInt();
                                else if (f == "sold")     fp.sold = val.toInt();
                            }
                        }

                        if (fp.productName.isEmpty())
                            fp.productName = pid;

                        // 查秒杀价（如果有 flash entry）
                        QString flashKey =
                            QString("flash:%1:product:%2").arg(saleNum, pid);
                        m_client->sendCommand(
                            QStringList() << "HGET" << flashKey << "flash_price",
                            [this, fp, pid, saleNum, baseStock,
                             pending](const RespValue& fv) mutable {
                                // flash_price 覆盖 base price
                                if (fv.type == RespValue::STRING) {
                                    // 有 flash entry → 秒杀商品
                                    fp.flashPrice =
                                        QString::fromStdString(fv.str_val);
                                    fp.isFlash = true;

                                    // 确保库存计数器存在（仅秒杀商品）
                                    QString counterKey =
                                        QString("flash:%1:product:%2:stock")
                                            .arg(saleNum, pid);
                                    m_client->sendCommand(
                                        QStringList() << "EXISTS" << counterKey,
                                        [this, fp, counterKey, baseStock]
                                        (const RespValue& ev) mutable {
                                            if (ev.type != RespValue::INTEGER
                                                || ev.int_val != 1) {
                                                m_client->sendCommand(
                                                    QStringList() << "SET" << counterKey
                                                        << QString::number(baseStock));
                                                fp.flashStock = baseStock;
                                            }
                                        });
                                }

                                // 追加到列表
                                m_products.append(fp);
                                QString label = fp.isFlash
                                    ? QString("%1 ⚡ [%2]")
                                        .arg(fp.productName, fp.productId)
                                    : QString("%1  [%2]")
                                        .arg(fp.productName, fp.productId);
                                m_productCombo->addItem(label, fp.productId);

                                (*pending)--;
                                if (*pending == 0) {
                                    m_productCombo->blockSignals(false);
                                    appendLog(QString("✅ 已加载 %1 件商品")
                                        .arg(m_products.size()));
                                    if (!m_products.isEmpty())
                                        updateProductInfo();
                                }
                            });
                    });
            }
        });
}

void BusinessPanel::updateProductInfo() {
    int idx = m_productCombo->currentIndex();
    if (idx < 0 || idx >= m_products.size()) {
        m_flashPriceLabel->setText("秒杀价: -");
        m_stockLabel->setText("剩余库存: -");
        m_soldLabel->setText("已售: -");
        m_stockProgress->setValue(0);
        m_orderBtn->setEnabled(false);
        m_batchBtn->setEnabled(false);
        return;
    }

    const FlashProduct& fp = m_products[idx];

    // 普通商品不允许秒杀下单
    if (!fp.isFlash) {
        m_flashPriceLabel->setText(
            QString("价格: ¥%1").arg(fp.flashPrice));
        m_stockLabel->setText("普通商品 (不支持秒杀)");
        m_soldLabel->setText("");
        m_stockProgress->setValue(0);
        m_orderBtn->setEnabled(false);
        m_batchBtn->setEnabled(false);
        return;
    }

    // 先从 Redis 读取最新库存（String counter）
    QString saleNum = m_flashSaleCombo->currentData().toString().section(':', -1);
    QString stockKey = QString("flash:%1:product:%2:stock")
                           .arg(saleNum, fp.productId);

    m_client->sendCommand(
        QStringList() << "GET" << stockKey,
        [this, fp, stockKey, saleNum](const RespValue& v) {
            int currentStock = fp.flashStock;  // fallback
            if (v.type == RespValue::STRING) {
                currentStock = QString::fromStdString(v.str_val).toInt();
            } else if (v.is_nil()) {
                currentStock = fp.flashStock;
            }

            // 从 Redis 实时取容量（避免 m_products 缓存过时）
            QString flashKey = QString("flash:%1:product:%2")
                .arg(saleNum, fp.productId);
            m_client->sendCommand(
                QStringList() << "HGET" << flashKey << "flash_stock",
                [this, fp, currentStock, flashKey](const RespValue& fsv) {
                    int capacity = fp.flashStock;  // fallback
                    if (fsv.type == RespValue::STRING)
                        capacity = QString::fromStdString(fsv.str_val).toInt();

                    // 只在库存超过容量时才扩容（容量只增不减）
                    if (currentStock > capacity) {
                        capacity = currentStock;
                        m_client->sendCommand(
                            QStringList() << "HSET" << flashKey << "flash_stock"
                                          << QString::number(capacity));
                    }

                    int consumed = capacity - currentStock;
                    if (consumed < 0) consumed = 0;

                    m_flashPriceLabel->setText(
                        QString("秒杀价: ¥%1").arg(fp.flashPrice));
                    m_stockLabel->setText(
                        QString("剩余库存: %1 / %2")
                            .arg(currentStock).arg(capacity));
                    m_soldLabel->setText(
                        QString("已售: %1").arg(consumed));

                    int pct = capacity > 0
                        ? (consumed * 100 / capacity) : 0;
                    m_stockProgress->setValue(pct);

                    bool hasStock = (currentStock > 0);
                    m_orderBtn->setEnabled(hasStock);
                    m_batchBtn->setEnabled(hasStock);
                });
        });
}

// ═══════════════════════════════════════════════════════════════
//  事件处理
// ═══════════════════════════════════════════════════════════════

void BusinessPanel::onFlashSaleChanged(int index) {
    if (index < 0) return;
    QString saleId = m_flashSaleCombo->itemData(index).toString();
    loadProductsForSale(saleId);
}

void BusinessPanel::onProductChanged(int index) {
    if (index < 0) return;
    updateProductInfo();
}

void BusinessPanel::onRandomUser() {
    int uid = 10001 + (std::rand() % 90000);
    m_userIdEdit->setText(QString::number(uid));
}

// ═══════════════════════════════════════════════════════════════
//  下单核心逻辑
//
//  使用 Redis DECR 原子操作防超卖:
//    1. DECR flash:N:product:M:stock  ← 原子减库存
//    2. 返回值 >= 0 → 成功，记录订单
//    3. 返回值 < 0  → 失败，INCR 回滚
//
//  这是真实秒杀系统中最经典的 "Decr-and-Check" 模式
// ═══════════════════════════════════════════════════════════════

void BusinessPanel::executeOrder(const QString& userId, int productIdx) {
    if (!m_client || !m_client->isConnected()) return;
    if (productIdx < 0 || productIdx >= m_products.size()) return;

    const FlashProduct& fp = m_products[productIdx];
    QString saleNum = m_flashSaleCombo->currentData().toString().section(':', -1);
    QString stockKey = QString("flash:%1:product:%2:stock")
                           .arg(saleNum, fp.productId);

    // ── Step 1: 原子 DECR ────────────────────────────
    m_client->sendCommand(
        QStringList() << "DECR" << stockKey,
        [this, userId, fp, saleNum, stockKey](const RespValue& v) {
            if (v.type != RespValue::INTEGER) {
                appendLog("系统错误: DECR 返回异常", true);
                return;
            }

            int newStock = static_cast<int>(v.int_val);

            if (newStock >= 0) {
                // ── 秒杀成功 ──────────────────────────
                QString now = QDateTime::currentDateTime()
                                  .toString("yyyy-MM-dd HH:mm:ss");

                // 构造订单 JSON
                QJsonObject order;
                order["order_id"] =
                    QString("ORD%1%2")
                        .arg(QDateTime::currentMSecsSinceEpoch())
                        .arg(userId);
                order["user_id"] = userId;
                order["product_id"] = fp.productId;
                order["product_name"] = fp.productName;
                order["flash_price"] = fp.flashPrice;
                order["time"] = now;
                QByteArray orderJson =
                    QJsonDocument(order).toJson(QJsonDocument::Compact);

                // 记录订单到队列
                m_client->sendCommand(
                    QStringList() << "LPUSH" << "orders:pending"
                                  << QString::fromUtf8(orderJson));

                // 更新统计数据
                m_client->sendCommand(
                    QStringList() << "INCR" << "stats:daily:orders");
                m_client->sendCommand(
                    QStringList() << "INCRBY" << "stats:daily:revenue"
                                  << fp.flashPrice);

                // 更新销量排行（ZSet: ZSCORE + ZADD 替代 ZINCRBY）
                m_client->sendCommand(
                    QStringList() << "ZSCORE" << "sales:rank" << fp.productId,
                    [this, fp](const RespValue& scoreVal) {
                        int newScore = 1;
                        if (scoreVal.type == RespValue::STRING) {
                            newScore = QString::fromStdString(
                                scoreVal.str_val).toInt() + 1;
                        }
                        m_client->sendCommand(
                            QStringList() << "ZADD" << "sales:rank"
                                          << QString::number(newScore)
                                          << fp.productId);
                    });

                // 日志
                appendLog(QString(
                    "<span style='color:#4CAF50'>✅ 秒杀成功</span> "
                    "用户 <b>%1</b> 购买了 <b>%2</b> "
                    "秒杀价 ¥%3  |  剩余库存: %4")
                    .arg(userId, fp.productName, fp.flashPrice)
                    .arg(newStock));

                emit dataChanged();

            } else {
                // ── 库存不足，回滚 ────────────────────
                m_client->sendCommand(
                    QStringList() << "INCR" << stockKey);

                appendLog(QString(
                    "<span style='color:#f44747'>❌ 秒杀失败</span> "
                    "用户 <b>%1</b> 购买 <b>%2</b> 失败："
                    "<span style='color:#FF5722'>库存不足</span>")
                    .arg(userId, fp.productName));
            }

            // 刷新库存显示
            updateProductInfo();
        });
}

void BusinessPanel::onPlaceOrder() {
    // 生成或获取用户 ID
    QString userId = m_userIdEdit->text().trimmed();
    if (userId.isEmpty()) {
        onRandomUser();
        userId = m_userIdEdit->text();
    }

    int idx = m_productCombo->currentIndex();
    if (idx < 0) {
        appendLog("请先选择秒杀商品", true);
        return;
    }

    const FlashProduct& fp = m_products[idx];
    if (!fp.isFlash) {
        appendLog("普通商品不支持秒杀下单", true);
        return;
    }
    appendLog(QString(
        "<span style='color:#569cd6'>▸ 用户 %1 正在抢购 %2...</span>")
        .arg(userId, fp.productName));

    executeOrder(userId, idx);
}

void BusinessPanel::onBatchStressTest() {
    int idx = m_productCombo->currentIndex();
    if (idx < 0) {
        appendLog("请先选择秒杀商品", true);
        return;
    }

    int batchCount = m_batchCountSpin->value();
    if (batchCount <= 0) {
        appendLog("压测数量必须大于 0", true);
        return;
    }

    const FlashProduct& fp = m_products[idx];
    if (!fp.isFlash) {
        appendLog("普通商品不支持批量秒杀", true);
        return;
    }
    appendLog(QString(
        "<span style='color:#FF9800'>━━━ 开始压测：%1 个用户抢购 %2 ━━━</span>")
        .arg(batchCount).arg(fp.productName));

    m_orderBtn->setEnabled(false);
    m_batchBtn->setEnabled(false);
    m_batchCountSpin->setEnabled(false);

    // 用 shared_ptr 管理计数器（lambda 捕获用）
    auto counters = std::make_shared<std::pair<int,int>>(0, 0);

    for (int i = 0; i < batchCount; ++i) {
        int uid = 10001 + (std::rand() % 90000);
        QString userId = QString::number(uid);

        // 对于批量操作，我们不能等待每个回调，直接发送 DECR
        // 使用简化的 fire-and-count 模式
        QString saleNum = m_flashSaleCombo->currentData()
                              .toString().section(':', -1);
        QString stockKey = QString("flash:%1:product:%2:stock")
                               .arg(saleNum, fp.productId);

        m_client->sendCommand(
            QStringList() << "DECR" << stockKey,
            [this, userId, fp, counters, stockKey, batchCount](const RespValue& v) {
                if (v.type == RespValue::INTEGER) {
                    int newStock = static_cast<int>(v.int_val);
                    if (newStock >= 0) {
                        (*counters).first++;  // success
                        // 记录订单（批量模式下简化日志）
                        QJsonObject order;
                        order["order_id"] =
                            QString("ORD%1%2")
                                .arg(QDateTime::currentMSecsSinceEpoch())
                                .arg(userId);
                        order["user_id"] = userId;
                        order["product_id"] = fp.productId;
                        order["product_name"] = fp.productName;
                        order["flash_price"] = fp.flashPrice;
                        order["time"] = QDateTime::currentDateTime()
                                            .toString("yyyy-MM-dd HH:mm:ss");
                        QByteArray orderJson =
                            QJsonDocument(order)
                                .toJson(QJsonDocument::Compact);

                        m_client->sendCommand(
                            QStringList() << "LPUSH" << "orders:pending"
                                          << QString::fromUtf8(orderJson));
                        m_client->sendCommand(
                            QStringList() << "INCR" << "stats:daily:orders");
                        m_client->sendCommand(
                            QStringList() << "INCRBY" << "stats:daily:revenue"
                                          << fp.flashPrice);
                    } else {
                        (*counters).second++;  // fail: out of stock
                        // 回滚
                        m_client->sendCommand(
                            QStringList() << "INCR" << stockKey);
                    }
                }

                // 最后一个响应时输出汇总
                int total = (*counters).first + (*counters).second;
                if (total == batchCount) {
                    appendLog(QString(
                        "<span style='color:#FF9800'>━━━ 压测完成 ━━━</span>"));
                    if ((*counters).second > 0) {
                        appendLog(QString(
                            "  <span style='color:#4CAF50'>✅ 成功: %1 单</span>  "
                            "<span style='color:#f44747'>❌ 失败(库存不足): %2 单</span>")
                            .arg((*counters).first).arg((*counters).second));
                    } else {
                        appendLog(QString(
                            "  <span style='color:#4CAF50'>✅ 成功: %1 单</span>")
                            .arg((*counters).first));
                    }

                    // 重新启用按钮并刷新
                    QMetaObject::invokeMethod(
                        this,
                        [this]() {
                            m_orderBtn->setEnabled(true);
                            m_batchBtn->setEnabled(true);
                            m_batchCountSpin->setEnabled(true);
                            updateProductInfo();
                            emit dataChanged();
                        },
                        Qt::QueuedConnection);
                }
            });
    }
}

// ═══════════════════════════════════════════════════════════════
//  日志
// ═══════════════════════════════════════════════════════════════

void BusinessPanel::appendLog(const QString& msg, bool isError) {
    Q_UNUSED(isError);
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logView->append(
        QString("<span style='color:#888'>[%1]</span> %2")
            .arg(timestamp, msg));
    // 滚动到底部
    QScrollBar* sb = m_logView->verticalScrollBar();
    sb->setValue(sb->maximum());
}
