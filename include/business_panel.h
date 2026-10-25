#ifndef BUSINESS_PANEL_H
#define BUSINESS_PANEL_H

#include <QWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QGroupBox>
#include <QTimer>

class RedisClient;

// 业务操作面板 —— 秒杀下单、库存管理、批量压测
class BusinessPanel : public QWidget {
    Q_OBJECT
public:
    explicit BusinessPanel(QWidget* parent = nullptr);

    void setClient(RedisClient* client);
    void refresh();  // 从 Redis 加载秒杀活动数据

signals:
    void dataChanged();            // 数据变更（触发 KeyBrowser 刷新）

private slots:
    void onFlashSaleChanged(int index);
    void onProductChanged(int index);
    void onPlaceOrder();           // 单次模拟下单
    void onBatchStressTest();      // 批量压测（100 单）
    void onRandomUser();

private:
    RedisClient* m_client = nullptr;

    // ── 状态栏 ──
    QLabel* m_statusLabel;
    QPushButton* m_refreshBtn;

    // ── 活动/商品选择 ──
    QComboBox* m_flashSaleCombo;
    QComboBox* m_productCombo;

    // ── 实时信息 ──
    QLabel* m_flashPriceLabel;
    QLabel* m_stockLabel;
    QLabel* m_soldLabel;
    QProgressBar* m_stockProgress;

    // ── 下单区 ──
    QLineEdit* m_userIdEdit;
    QPushButton* m_randomUserBtn;
    QPushButton* m_orderBtn;
    QPushButton* m_batchBtn;
    QSpinBox* m_batchCountSpin;

    // ── 操作日志 ──
    QTextEdit* m_logView;

    // ── 缓存数据 ──
    struct FlashProduct {
        QString productId;
        QString productName;
        QString flashPrice;
        int flashStock = 0;
        int sold = 0;
        bool isFlash = false;  // 是否秒杀商品
    };
    QList<FlashProduct> m_products;  // 当前选中活动的商品列表

    void setupUi();
    void appendLog(const QString& msg, bool isError = false);
    void loadFlashSales();           // 加载秒杀活动列表
    void loadProductsForSale(const QString& saleId);
    void updateProductInfo();        // 更新库存/进度显示

    // ── 下单核心逻辑 ──
    void executeOrder(const QString& userId, int productIdx);
};

#endif // BUSINESS_PANEL_H
