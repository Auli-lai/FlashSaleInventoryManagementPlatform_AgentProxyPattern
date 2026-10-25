#ifndef STATS_DASHBOARD_H
#define STATS_DASHBOARD_H

#include <QWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QLabel>

class RedisClient;

class StatsDashboard : public QWidget {
    Q_OBJECT
public:
    explicit StatsDashboard(QWidget* parent = nullptr);

    void setClient(RedisClient* client);
    void startAutoRefresh(int intervalMs = 5000);
    void stopAutoRefresh();

private slots:
    void refresh();

private:
    RedisClient* m_client = nullptr;
    QTableWidget* m_table;
    QLabel* m_titleLabel;
    QTimer* m_timer;

    void parseInfoOutput(const QString& infoText);
};

#endif // STATS_DASHBOARD_H
