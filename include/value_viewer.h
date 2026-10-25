#ifndef VALUE_VIEWER_H
#define VALUE_VIEWER_H

#include <QWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QListWidget>
#include <QVBoxLayout>

class RedisClient;

class ValueViewer : public QWidget {
    Q_OBJECT
public:
    explicit ValueViewer(QWidget* parent = nullptr);

    void setClient(RedisClient* client);
    void loadKey(const QString& key, int db, const QString& type);
    void clear();

signals:
    void statusMessage(const QString& msg);

private slots:
    void onSaveString();
    void onSaveList();
    void onSaveHash();
    void onSaveSet();
    void onSaveZSet();
    void onDeleteKey();

    // 编辑器行操作
    void onListAddRow();
    void onListRemoveRow();
    void onHashAddRow();
    void onHashRemoveRow();
    void onSetAddMember();
    void onSetRemoveMember();
    void onZSetAddRow();
    void onZSetRemoveRow();

private:
    RedisClient* m_client = nullptr;
    QStackedWidget* m_stack;

    QString m_currentKey;
    int m_currentDb = 0;
    QString m_currentType;

    // 信息栏
    QLabel* m_keyLabel;
    QLabel* m_typeLabel;
    QLabel* m_ttlLabel;
    QPushButton* m_deleteBtn;
    QPushButton* m_saveBtn;
    QPushButton* m_refreshBtn;

    // 编辑器 widgets
    QPlainTextEdit* m_stringEdit;
    QTableWidget* m_listTable;
    QPushButton* m_listAddBtn;
    QPushButton* m_listRemoveBtn;
    QTableWidget* m_hashTable;
    QPushButton* m_hashAddBtn;
    QPushButton* m_hashRemoveBtn;
    QListWidget* m_setList;
    QPushButton* m_setAddBtn;
    QPushButton* m_setRemoveBtn;
    QTableWidget* m_zsetTable;
    QPushButton* m_zsetAddBtn;
    QPushButton* m_zsetRemoveBtn;

    void setupUi();
    void setupListEditor();
    void setupHashEditor();
    void setupSetEditor();
    void setupZSetEditor();
    void showInfoBar(bool visible);
    void updateInfoBar(const QString& key, const QString& type,
                       const QString& ttl = "");
};

#endif // VALUE_VIEWER_H
