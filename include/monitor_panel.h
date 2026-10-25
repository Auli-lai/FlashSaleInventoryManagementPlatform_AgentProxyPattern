#ifndef MONITOR_PANEL_H
#define MONITOR_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QProgressBar>
#include <QFrame>
#include <QMap>
#include <QStringList>
#include <memory>
#include <functional>

class RedisClient;

// ═══════════════════════════════════════════════════════════════
// 实时业务监控面板：KPI 卡片 + 库存概览 + 销售排行
// ═══════════════════════════════════════════════════════════════
class MonitorPanel : public QWidget {
    Q_OBJECT
public:
    explicit MonitorPanel(QWidget* parent = nullptr);

    void setClient(RedisClient* client);
    void startAutoRefresh(int intervalMs = 3000);
    void stopAutoRefresh();

private slots:
    void refresh();

private:
    RedisClient* m_client = nullptr;
    QTimer* m_timer = nullptr;

    // ── KPI 卡片 ──
    struct KpiCard {
        QFrame* frame = nullptr;
        QLabel* titleLabel = nullptr;
        QLabel* valueLabel = nullptr;
    };
    KpiCard m_ordersCard;
    KpiCard m_revenueCard;
    KpiCard m_visitorsCard;
    KpiCard m_pendingCard;
    KpiCard m_flashCountCard;

    // ── 库存概览表格 ──
    QTableWidget* m_stockTable = nullptr;
    QLabel* m_stockTitleLabel = nullptr;

    // ── 销售排行表格 ──
    QTableWidget* m_rankingTable = nullptr;
    QLabel* m_rankingTitleLabel = nullptr;

    // ── 产品元数据缓存（首次加载后复用）──
    struct MonitorProduct {
        QString productId;
        QString productName;
        QString flashPrice;
        QString saleNum;       // e.g. "1"
        int flashStockCapacity = 0;
    };
    QList<MonitorProduct> m_products;
    bool m_productsCached = false;
    QString m_saleNum = "1";   // 秒杀活动编号

    // ── UI 构建 ──
    void setupUi();
    KpiCard createKpiCard(const QString& title, const QString& icon,
                          const QString& color, QGridLayout* grid,
                          int row, int col);

    // ── 数据加载（2 阶段异步）──
    void loadPhaseOne();
    void loadPhaseTwo(const QString& ordersVal, const QString& revenueVal,
                      const QString& visitorsVal, int pendingCount,
                      const QStringList& productIds);
    void cacheProductMetadata(const QStringList& productIds,
                              std::function<void()> onDone);
    void loadStockAndRanking(int pendingCount,
                             const QString& ordersVal,
                             const QString& revenueVal,
                             const QString& visitorsVal);

    // ── UI 更新 ──
    void updateKpiDisplay(const QString& ordersVal, const QString& revenueVal,
                          const QString& visitorsVal, int pendingCount,
                          int flashProductCount);
    void updateStockTable(const QMap<QString, int>& stockValues,
                          const QMap<QString, int>& soldValues);
    void updateRankingTable(const QStringList& entries,
                            const QStringList& scores);
};

#endif // MONITOR_PANEL_H
