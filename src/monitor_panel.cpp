#include "monitor_panel.h"
#include "redis_client.h"
#include "resp_codec.h"

#include <QHeaderView>
#include <QFont>
#include <QTime>
#include <QSet>
#include <QDebug>

// ═══════════════════════════════════════════════════════════════
// 构造 & 基础设置
// ═══════════════════════════════════════════════════════════════

MonitorPanel::MonitorPanel(QWidget* parent) : QWidget(parent) {
    setupUi();

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MonitorPanel::refresh);
}

void MonitorPanel::setClient(RedisClient* client) {
    m_client = client;
}

void MonitorPanel::startAutoRefresh(int intervalMs) {
    if (m_client && m_client->isConnected()) {
        // 首次立即刷新，重置缓存
        m_productsCached = false;
        m_products.clear();
        refresh();
        m_timer->start(intervalMs);
    }
}

void MonitorPanel::stopAutoRefresh() {
    m_timer->stop();
}

// ═══════════════════════════════════════════════════════════════
// UI 构建
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // ── 标题 ──
    auto* headerLabel = new QLabel("📊 实时业务监控");
    QFont headerFont;
    headerFont.setPointSize(14);
    headerFont.setBold(true);
    headerLabel->setFont(headerFont);
    mainLayout->addWidget(headerLabel);

    // ── KPI 卡片行 ──
    auto* kpiGrid = new QGridLayout();
    kpiGrid->setSpacing(10);

    m_ordersCard   = createKpiCard("今日订单", "📦", "#4CAF50", kpiGrid, 0, 0);
    m_revenueCard  = createKpiCard("今日营收", "💰", "#F44336", kpiGrid, 0, 1);
    m_visitorsCard = createKpiCard("今日访客", "👥", "#2196F3", kpiGrid, 0, 2);
    m_pendingCard  = createKpiCard("待处理订单", "📋", "#FF9800", kpiGrid, 0, 3);
    m_flashCountCard = createKpiCard("秒杀商品", "⚡", "#9C27B0", kpiGrid, 0, 4);

    mainLayout->addLayout(kpiGrid);

    // ── 库存概览 ──
    auto* stockGroup = new QGroupBox("📦 秒杀商品库存概览");
    auto* stockLayout = new QVBoxLayout(stockGroup);

    m_stockTitleLabel = new QLabel("加载中...");
    m_stockTitleLabel->setStyleSheet("color: #666; font-size: 11px;");
    stockLayout->addWidget(m_stockTitleLabel);

    m_stockTable = new QTableWidget(0, 6, this);
    m_stockTable->setHorizontalHeaderLabels(
        {"商品名称", "秒杀价", "剩余库存", "已售", "库存进度", "商品ID"});
    m_stockTable->horizontalHeader()->setStretchLastSection(true);
    m_stockTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_stockTable->verticalHeader()->setVisible(false);
    m_stockTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stockTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_stockTable->setAlternatingRowColors(true);
    m_stockTable->setColumnHidden(5, true);  // 隐藏商品 ID 列
    stockLayout->addWidget(m_stockTable);

    mainLayout->addWidget(stockGroup);

    // ── 销售排行 ──
    auto* rankGroup = new QGroupBox("🏆 销售排行 TOP 10");
    auto* rankLayout = new QVBoxLayout(rankGroup);

    m_rankingTitleLabel = new QLabel("加载中...");
    m_rankingTitleLabel->setStyleSheet("color: #666; font-size: 11px;");
    rankLayout->addWidget(m_rankingTitleLabel);

    m_rankingTable = new QTableWidget(0, 3, this);
    m_rankingTable->setHorizontalHeaderLabels({"排名", "商品", "销量"});
    m_rankingTable->horizontalHeader()->setStretchLastSection(true);
    m_rankingTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_rankingTable->verticalHeader()->setVisible(false);
    m_rankingTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_rankingTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rankingTable->setAlternatingRowColors(true);
    rankLayout->addWidget(m_rankingTable);

    mainLayout->addWidget(rankGroup);
}

MonitorPanel::KpiCard MonitorPanel::createKpiCard(
    const QString& title, const QString& icon, const QString& color,
    QGridLayout* grid, int row, int col) {

    auto* frame = new QFrame();
    frame->setStyleSheet(QString(
        "QFrame {"
        "  background: white;"
        "  border-left: 4px solid %1;"
        "  border-radius: 6px;"
        "  padding: 12px;"
        "}"
    ).arg(color));
    frame->setMinimumHeight(80);

    auto* vbox = new QVBoxLayout(frame);
    vbox->setContentsMargins(8, 6, 8, 6);
    vbox->setSpacing(4);

    auto* titleLabel = new QLabel(QString("%1  %2").arg(icon, title));
    titleLabel->setStyleSheet("color: #888; font-size: 11px; border: none;");
    vbox->addWidget(titleLabel);

    auto* valueLabel = new QLabel("—");
    valueLabel->setStyleSheet(QString(
        "color: %1; font-size: 22px; font-weight: bold; border: none;"
    ).arg(color));
    vbox->addWidget(valueLabel);

    grid->addWidget(frame, row, col);

    KpiCard card;
    card.frame = frame;
    card.titleLabel = titleLabel;
    card.valueLabel = valueLabel;
    return card;
}

// ═══════════════════════════════════════════════════════════════
// 数据加载入口
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::refresh() {
    if (!m_client || !m_client->isConnected()) return;
    loadPhaseOne();
}

// ═══════════════════════════════════════════════════════════════
// 阶段 1：并行获取 KPI + 发现秒杀商品
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::loadPhaseOne() {
    auto pending = std::make_shared<int>(5);

    auto ordersVal   = std::make_shared<QString>("0");
    auto revenueVal  = std::make_shared<QString>("0");
    auto visitorsVal = std::make_shared<QString>("0");
    auto pendingOrders = std::make_shared<int>(0);
    auto productIds  = std::make_shared<QStringList>();
    auto saleNum     = std::make_shared<QString>("1");

    // ── 1. GET stats:daily:orders ──
    m_client->sendCommand(
        QStringList() << "GET" << "stats:daily:orders",
        [this, ordersVal, pending, revenueVal, visitorsVal,
         pendingOrders, productIds, saleNum](const RespValue& v) {
            if (v.type == RespValue::STRING)
                *ordersVal = QString::fromStdString(v.str_val);
            (*pending)--;
            if (*pending == 0) {
                m_saleNum = *saleNum;
                loadPhaseTwo(*ordersVal, *revenueVal, *visitorsVal,
                             *pendingOrders, *productIds);
            }
        });

    // ── 2. GET stats:daily:revenue ──
    m_client->sendCommand(
        QStringList() << "GET" << "stats:daily:revenue",
        [this, revenueVal, pending, ordersVal, visitorsVal,
         pendingOrders, productIds, saleNum](const RespValue& v) {
            if (v.type == RespValue::STRING)
                *revenueVal = QString::fromStdString(v.str_val);
            (*pending)--;
            if (*pending == 0) {
                m_saleNum = *saleNum;
                loadPhaseTwo(*ordersVal, *revenueVal, *visitorsVal,
                             *pendingOrders, *productIds);
            }
        });

    // ── 3. GET stats:daily:visitors ──
    m_client->sendCommand(
        QStringList() << "GET" << "stats:daily:visitors",
        [this, visitorsVal, pending, ordersVal, revenueVal,
         pendingOrders, productIds, saleNum](const RespValue& v) {
            if (v.type == RespValue::STRING)
                *visitorsVal = QString::fromStdString(v.str_val);
            (*pending)--;
            if (*pending == 0) {
                m_saleNum = *saleNum;
                loadPhaseTwo(*ordersVal, *revenueVal, *visitorsVal,
                             *pendingOrders, *productIds);
            }
        });

    // ── 4. LLEN orders:pending ──
    m_client->sendCommand(
        QStringList() << "LLEN" << "orders:pending",
        [this, pendingOrders, pending, ordersVal, revenueVal,
         visitorsVal, productIds, saleNum](const RespValue& v) {
            if (v.type == RespValue::INTEGER)
                *pendingOrders = static_cast<int>(v.int_val);
            (*pending)--;
            if (*pending == 0) {
                m_saleNum = *saleNum;
                loadPhaseTwo(*ordersVal, *revenueVal, *visitorsVal,
                             *pendingOrders, *productIds);
            }
        });

    // ── 5. KEYS flash:*:product:* (发现所有秒杀商品) ──
    m_client->sendCommand(
        QStringList() << "KEYS" << "flash:*:product:*",
        [this, productIds, saleNum, pending, ordersVal, revenueVal,
         visitorsVal, pendingOrders](const RespValue& v) {
            if (v.type == RespValue::ARRAY) {
                QSet<QString> seen;
                for (const auto& item : v.array_val) {
                    if (item.type != RespValue::STRING) continue;
                    QString key = QString::fromStdString(item.str_val);
                    // 过滤掉 stock 计数器 key
                    if (key.endsWith(":stock")) continue;
                    // 解析 flash:N:product:XXXX
                    QStringList parts = key.split(':');
                    if (parts.size() >= 4) {
                        QString pid = parts[3];
                        if (!seen.contains(pid)) {
                            seen.insert(pid);
                            productIds->append(pid);
                        }
                        // 记录 saleNum
                        if (saleNum->isEmpty() || *saleNum == "1")
                            *saleNum = parts[1];
                    }
                }
            }
            (*pending)--;
            if (*pending == 0) {
                m_saleNum = *saleNum;
                loadPhaseTwo(*ordersVal, *revenueVal, *visitorsVal,
                             *pendingOrders, *productIds);
            }
        });
}

// ═══════════════════════════════════════════════════════════════
// 阶段 2：缓存产品元数据 → 加载库存 & 排行
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::loadPhaseTwo(const QString& ordersVal,
                                 const QString& revenueVal,
                                 const QString& visitorsVal,
                                 int pendingCount,
                                 const QStringList& productIds) {
    if (productIds.isEmpty()) {
        // 没有秒杀商品：直接显示空面板
        updateKpiDisplay(ordersVal, revenueVal, visitorsVal,
                         pendingCount, 0);
        m_stockTable->setRowCount(0);
        m_stockTitleLabel->setText(
            QString("暂无秒杀商品 (更新: %1)")
                .arg(QTime::currentTime().toString("HH:mm:ss")));
        m_rankingTable->setRowCount(0);
        m_rankingTitleLabel->setText(
            QString("暂无销售数据 (更新: %1)")
                .arg(QTime::currentTime().toString("HH:mm:ss")));
        return;
    }

    // 检测产品列表是否变化（新增或删除），变化则清缓存重建
    if (m_productsCached && m_products.size() != productIds.size()) {
        m_productsCached = false;
        m_products.clear();
    }

    if (!m_productsCached) {
        // 首次加载 / 缓存失效：先缓存产品元数据
        cacheProductMetadata(productIds, [this, pendingCount,
                                           ordersVal, revenueVal,
                                           visitorsVal]() {
            loadStockAndRanking(pendingCount, ordersVal,
                               revenueVal, visitorsVal);
        });
    } else {
        loadStockAndRanking(pendingCount, ordersVal,
                           revenueVal, visitorsVal);
    }
}

// ═══════════════════════════════════════════════════════════════
// 缓存产品元数据（首次加载）
// 每个产品链式发送 2 条命令：HGETALL product + HGETALL flash
// shared_ptr<int> 计数器 = 产品数，仅在第二个回调中递减
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::cacheProductMetadata(const QStringList& productIds,
                                         std::function<void()> onDone) {
    m_products.clear();
    auto pending = std::make_shared<int>(productIds.size());

    for (const QString& pid : productIds) {
        // ── 第 1 步：HGETALL product:XXXX 获取名称 ──
        m_client->sendCommand(
            QStringList() << "HGETALL" << QString("product:%1").arg(pid),
            [this, pid, pending, onDone](const RespValue& pv) {
                MonitorProduct mp;
                mp.productId = pid;
                mp.saleNum = m_saleNum;
                mp.flashPrice = "0";
                mp.flashStockCapacity = 0;

                if (pv.type == RespValue::ARRAY) {
                    const auto& arr = pv.array_val;
                    for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                        if (arr[i].type != RespValue::STRING) continue;
                        QString field = QString::fromStdString(arr[i].str_val);
                        if (arr[i+1].type != RespValue::STRING) continue;
                        QString val = QString::fromStdString(arr[i+1].str_val);
                        if (field == "name") mp.productName = val;
                    }
                }
                if (mp.productName.isEmpty()) mp.productName = pid;

                // ── 第 2 步：HGETALL flash:N:product:XXXX 获取秒杀价/库存 ──
                m_client->sendCommand(
                    QStringList()
                        << "HGETALL"
                        << QString("flash:%1:product:%2").arg(m_saleNum, pid),
                    [this, mp, pending, onDone](const RespValue& fv) mutable {
                        if (fv.type == RespValue::ARRAY) {
                            const auto& arr = fv.array_val;
                            for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                                if (arr[i].type != RespValue::STRING) continue;
                                QString field = QString::fromStdString(arr[i].str_val);
                                if (arr[i+1].type != RespValue::STRING) continue;
                                QString val = QString::fromStdString(arr[i+1].str_val);
                                if (field == "flash_price")
                                    mp.flashPrice = val;
                                else if (field == "flash_stock")
                                    mp.flashStockCapacity = val.toInt();
                            }
                        }
                        m_products.append(mp);

                        (*pending)--;
                        if (*pending == 0) {
                            m_productsCached = true;
                            onDone();
                        }
                    });
            });
    }
}

// ═══════════════════════════════════════════════════════════════
// 加载库存计数器 & 销售排行
// 每条产品：GET stock counter + HGET sold = 2 条命令
// 额外：1 条 ZREVRANGE 命令
// 总计：m_products.size() * 2 + 1
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::loadStockAndRanking(int pendingCount,
                                        const QString& ordersVal,
                                        const QString& revenueVal,
                                        const QString& visitorsVal) {
    int n = m_products.size();
    auto counter = std::make_shared<int>(n * 2 + 1);

    auto stockValues = std::make_shared<QMap<QString, int>>();
    auto soldValues  = std::make_shared<QMap<QString, int>>();
    auto rankEntries = std::make_shared<QStringList>();
    auto rankScores  = std::make_shared<QStringList>();

    // ── 每条产品：GET counter + HGET sold ──
    for (const auto& mp : m_products) {
        QString stockKey = QString("flash:%1:product:%2:stock")
                               .arg(mp.saleNum, mp.productId);
        QString flashKey = QString("flash:%1:product:%2")
                               .arg(mp.saleNum, mp.productId);

        // GET stock counter
        auto pidStock = std::make_shared<QString>(mp.productId);
        m_client->sendCommand(
            QStringList() << "GET" << stockKey,
            [this, counter, stockValues, soldValues, rankEntries,
             rankScores, pendingCount, ordersVal, revenueVal,
             visitorsVal, pidStock](const RespValue& v) {
                int stock = 0;
                if (v.type == RespValue::STRING)
                    stock = QString::fromStdString(v.str_val).toInt();
                (*stockValues)[*pidStock] = stock;

                (*counter)--;
                if (*counter == 0) {
                    QTimer::singleShot(0, this, [this,
                        ordersVal, revenueVal, visitorsVal,
                        pendingCount, stockValues, soldValues,
                        rankEntries, rankScores]() {
                        updateKpiDisplay(ordersVal, revenueVal, visitorsVal,
                                         pendingCount, m_products.size());
                        updateStockTable(*stockValues, *soldValues);
                        updateRankingTable(*rankEntries, *rankScores);
                    });
                }
            });

        // HGET sold
        auto pidSold = std::make_shared<QString>(mp.productId);
        m_client->sendCommand(
            QStringList() << "HGET" << flashKey << "sold",
            [this, counter, stockValues, soldValues, rankEntries,
             rankScores, pendingCount, ordersVal, revenueVal,
             visitorsVal, pidSold](const RespValue& v) {
                int sold = 0;
                if (v.type == RespValue::STRING)
                    sold = QString::fromStdString(v.str_val).toInt();
                (*soldValues)[*pidSold] = sold;

                (*counter)--;
                if (*counter == 0) {
                    QTimer::singleShot(0, this, [this,
                        ordersVal, revenueVal, visitorsVal,
                        pendingCount, stockValues, soldValues,
                        rankEntries, rankScores]() {
                        updateKpiDisplay(ordersVal, revenueVal, visitorsVal,
                                         pendingCount, m_products.size());
                        updateStockTable(*stockValues, *soldValues);
                        updateRankingTable(*rankEntries, *rankScores);
                    });
                }
            });
    }

    // ── ZREVRANGE sales:rank 0 9 WITHSCORES ──
    m_client->sendCommand(
        QStringList() << "ZREVRANGE" << "sales:rank" << "0" << "9"
                      << "WITHSCORES",
        [this, counter, stockValues, soldValues, rankEntries,
         rankScores, pendingCount, ordersVal, revenueVal,
         visitorsVal](const RespValue& v) {
            if (v.type == RespValue::ARRAY) {
                const auto& arr = v.array_val;
                for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                    if (arr[i].type == RespValue::STRING)
                        rankEntries->append(
                            QString::fromStdString(arr[i].str_val));
                    if (arr[i+1].type == RespValue::STRING)
                        rankScores->append(
                            QString::fromStdString(arr[i+1].str_val));
                }
            }
            (*counter)--;
            if (*counter == 0) {
                QTimer::singleShot(0, this, [this,
                    ordersVal, revenueVal, visitorsVal,
                    pendingCount, stockValues, soldValues,
                    rankEntries, rankScores]() {
                    updateKpiDisplay(ordersVal, revenueVal, visitorsVal,
                                     pendingCount, m_products.size());
                    updateStockTable(*stockValues, *soldValues);
                    updateRankingTable(*rankEntries, *rankScores);
                });
            }
        });
}

// ═══════════════════════════════════════════════════════════════
// UI 更新
// ═══════════════════════════════════════════════════════════════

void MonitorPanel::updateKpiDisplay(const QString& ordersVal,
                                     const QString& revenueVal,
                                     const QString& visitorsVal,
                                     int pendingCount,
                                     int flashProductCount) {
    m_ordersCard.valueLabel->setText(ordersVal);
    m_revenueCard.valueLabel->setText(
        QString("¥%1").arg(revenueVal));
    m_visitorsCard.valueLabel->setText(visitorsVal);
    m_pendingCard.valueLabel->setText(
        QString::number(pendingCount));
    m_flashCountCard.valueLabel->setText(
        QString::number(flashProductCount));
}

void MonitorPanel::updateStockTable(
    const QMap<QString, int>& stockValues,
    const QMap<QString, int>& soldValues) {

    m_stockTable->setRowCount(0);

    if (m_products.isEmpty()) {
        m_stockTitleLabel->setText(
            QString("暂无秒杀商品 (更新: %1)")
                .arg(QTime::currentTime().toString("HH:mm:ss")));
        return;
    }

    for (int row = 0; row < m_products.size(); ++row) {
        const auto& mp = m_products[row];
        m_stockTable->insertRow(row);

        int remaining = stockValues.value(mp.productId, 0);
        int sold = soldValues.value(mp.productId, 0);
        int capacity = mp.flashStockCapacity;

        // 商品名称
        auto* nameItem = new QTableWidgetItem(mp.productName);
        nameItem->setToolTip(mp.productName);
        m_stockTable->setItem(row, 0, nameItem);

        // 秒杀价
        auto* priceItem = new QTableWidgetItem(
            QString("¥%1").arg(mp.flashPrice));
        priceItem->setTextAlignment(Qt::AlignCenter);
        m_stockTable->setItem(row, 1, priceItem);

        // 剩余库存
        auto* stockItem = new QTableWidgetItem(QString::number(remaining));
        stockItem->setTextAlignment(Qt::AlignCenter);
        if (remaining == 0) {
            stockItem->setForeground(QColor("#F44336"));
            auto f = stockItem->font();
            f.setBold(true);
            stockItem->setFont(f);
        } else if (remaining < capacity * 0.2) {
            stockItem->setForeground(QColor("#FF9800"));
        }
        m_stockTable->setItem(row, 2, stockItem);

        // 已售
        auto* soldItem = new QTableWidgetItem(QString::number(sold));
        soldItem->setTextAlignment(Qt::AlignCenter);
        m_stockTable->setItem(row, 3, soldItem);

        // 进度条
        auto* bar = new QProgressBar();
        bar->setRange(0, capacity > 0 ? capacity : 1);
        bar->setValue(sold);
        bar->setTextVisible(true);
        bar->setFormat(QString("%1 / %2").arg(remaining).arg(capacity));

        if (remaining == 0) {
            bar->setStyleSheet(
                "QProgressBar { border: 1px solid #ddd; border-radius: 3px; "
                "background: #f5f5f5; text-align: center; }"
                "QProgressBar::chunk { background: #F44336; border-radius: 3px; }");
        } else if (capacity > 0 && remaining < capacity * 0.2) {
            bar->setStyleSheet(
                "QProgressBar { border: 1px solid #ddd; border-radius: 3px; "
                "background: #f5f5f5; text-align: center; }"
                "QProgressBar::chunk { background: #FF9800; border-radius: 3px; }");
        } else {
            bar->setStyleSheet(
                "QProgressBar { border: 1px solid #ddd; border-radius: 3px; "
                "background: #f5f5f5; text-align: center; }"
                "QProgressBar::chunk { background: #4CAF50; border-radius: 3px; }");
        }
        m_stockTable->setCellWidget(row, 4, bar);

        // 商品 ID（隐藏列）
        m_stockTable->setItem(row, 5, new QTableWidgetItem(mp.productId));

        m_stockTable->setRowHeight(row, 32);
    }

    m_stockTitleLabel->setText(
        QString("共 %1 件秒杀商品 (更新: %2)")
            .arg(m_products.size())
            .arg(QTime::currentTime().toString("HH:mm:ss")));

    // 自适应列宽
    m_stockTable->resizeColumnsToContents();
    m_stockTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
}

void MonitorPanel::updateRankingTable(const QStringList& entries,
                                       const QStringList& scores) {
    m_rankingTable->setRowCount(0);

    if (entries.isEmpty()) {
        m_rankingTable->insertRow(0);
        auto* emptyItem = new QTableWidgetItem("暂无销售数据");
        emptyItem->setTextAlignment(Qt::AlignCenter);
        m_rankingTable->setItem(0, 0, emptyItem);
        m_rankingTable->setSpan(0, 0, 1, 3);
        m_rankingTitleLabel->setText(
            QString("🏆 销售排行 TOP 10 (更新: %1)")
                .arg(QTime::currentTime().toString("HH:mm:ss")));
        return;
    }

    // 建立 productId → productName 映射
    QMap<QString, QString> nameMap;
    for (const auto& mp : m_products) {
        nameMap[mp.productId] = mp.productName;
    }

    for (int i = 0; i < entries.size(); ++i) {
        m_rankingTable->insertRow(i);

        // 排名
        auto* rankItem = new QTableWidgetItem(QString::number(i + 1));
        rankItem->setTextAlignment(Qt::AlignCenter);
        // 前 3 名高亮
        if (i == 0) {
            rankItem->setText("🥇");
            rankItem->setForeground(QColor("#FFD700"));
        } else if (i == 1) {
            rankItem->setText("🥈");
            rankItem->setForeground(QColor("#C0C0C0"));
        } else if (i == 2) {
            rankItem->setText("🥉");
            rankItem->setForeground(QColor("#CD7F32"));
        }
        m_rankingTable->setItem(i, 0, rankItem);

        // 商品名
        QString pid = entries[i];
        QString displayName = nameMap.value(pid, pid);
        auto* nameItem = new QTableWidgetItem(displayName);
        m_rankingTable->setItem(i, 1, nameItem);

        // 销量
        auto* scoreItem = new QTableWidgetItem(scores.value(i, "0"));
        scoreItem->setTextAlignment(Qt::AlignCenter);
        QFont boldFont = scoreItem->font();
        boldFont.setBold(true);
        scoreItem->setFont(boldFont);
        m_rankingTable->setItem(i, 2, scoreItem);

        m_rankingTable->setRowHeight(i, 28);
    }

    m_rankingTitleLabel->setText(
        QString("🏆 销售排行 TOP 10 (更新: %1)")
            .arg(QTime::currentTime().toString("HH:mm:ss")));
}
