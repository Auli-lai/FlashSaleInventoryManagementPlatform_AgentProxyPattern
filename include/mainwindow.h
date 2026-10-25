#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTabWidget>
#include <QSplitter>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>

class ServerManager;
class RedisClient;
class KeyBrowser;
class ValueViewer;
class ConsolePanel;
class StatsDashboard;
class BusinessPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

private slots:
    // Server
    void onStartServer();
    void onStopServer();
    void onServerStarted(int port);
    void onServerStopped();
    void onServerOutput(const QString& line);

    // Client
    void onConnectToServer();
    void onDisconnectFromServer();
    void onClientConnected();
    void onClientDisconnected();
    void onClientError(const QString& msg);

    // Key selection
    void onKeySelected(const QString& key, int db, const QString& type);

    // Misc
    void onAbout();

private:
    void setupMenuBar();
    void setupToolBar();
    void setupCentralWidget();
    void setupStatusBar();
    void setupConnections();
    void updateConnectionState();

    ServerManager*    m_serverMgr;
    RedisClient*      m_client;
    KeyBrowser*       m_keyBrowser;
    ValueViewer*      m_valueViewer;
    ConsolePanel*     m_console;
    StatsDashboard*   m_statsDashboard;
    BusinessPanel*    m_businessPanel;
    QTabWidget*       m_tabWidget;

    QAction* m_startServerAction;
    QAction* m_stopServerAction;
    QAction* m_connectAction;
    QAction* m_disconnectAction;
    QAction* m_refreshAction;

    QLabel* m_connectionStatus;
    QLabel* m_dbLabel;

    bool m_serverRunning   = false;
    bool m_clientConnected = false;
    int  m_currentDb       = 0;
};

#endif // MAINWINDOW_H
