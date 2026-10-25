#ifndef KEY_BROWSER_H
#define KEY_BROWSER_H

#include <QWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QMap>
#include <QSet>

class RedisClient;

class KeyBrowser : public QWidget {
    Q_OBJECT
public:
    explicit KeyBrowser(QWidget* parent = nullptr);

    void setClient(RedisClient* client);
    void refresh();

signals:
    void keySelected(const QString& key, int db, const QString& type);
    void keysChanged();  // 商品增减时通知业务面板刷新

private slots:
    void onItemClicked(QTreeWidgetItem* item, int column);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);
    void onFilterChanged(const QString& text);
    void onAddProduct();
    void showAddProductDialog(const QString& newKey);

private:
    RedisClient* m_client = nullptr;
    QTreeWidget* m_tree;
    QLineEdit* m_filterEdit;
    QPushButton* m_refreshBtn;
    QPushButton* m_addProductBtn;
    int m_currentDb = 0;
    QSet<QString> m_loadedKeys;  // 防止重复加载

    QTreeWidgetItem* ensureDbRoot(int db);
    QTreeWidgetItem* ensureCategory(const QString& title,
                                     QTreeWidgetItem* parent);
    QIcon iconForType(const QString& type) const;
    QString categoryForKey(const QString& key) const;

    // 用 KEYS 命令加载所有 key（后续可升级为 SCAN 分页）
    void loadKeysForDb(int db);
    void loadProductDisplay(QTreeWidgetItem* item, const QString& key, bool isFlash);
    void loadNonProductDisplay(QTreeWidgetItem* item, const QString& key);
};

#endif // KEY_BROWSER_H
