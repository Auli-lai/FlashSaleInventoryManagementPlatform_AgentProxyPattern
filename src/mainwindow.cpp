#include "mainwindow.h"
#include "server_manager.h"
#include "redis_client.h"
#include "key_browser.h"
#include "value_viewer.h"
#include "console_panel.h"
#include "stats_dashboard.h"
#include "business_data.h"
#include "business_panel.h"
#include "log.h"

#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QToolBar>
#include <QSplitter>
#include <QStatusBar>
#include <QLabel>
#include <QMessageBox>
#include <QInputDialog>
#include <QApplication>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    m_serverMgr      = new ServerManager(this);
    m_client         = new RedisClient(this);
    m_keyBrowser     = new KeyBrowser(this);
    m_valueViewer    = new ValueViewer(this);
    m_console        = new ConsolePanel(this);
    m_statsDashboard = new StatsDashboard(this);
    m_businessPanel  = new BusinessPanel(this);

    m_connectionStatus = new QLabel("Disconnected");
    m_dbLabel          = new QLabel("DB0");

    setupMenuBar();
    setupToolBar();
    setupCentralWidget();
    setupStatusBar();
    setupConnections();

    updateConnectionState();
    Log::info("MainWindow initialized");
}

MainWindow::~MainWindow() {
    if (m_serverMgr->isRunning()) m_serverMgr->stop();
}

// ═══════════════════════════════════════════ 菜单栏 ══════

void MainWindow::setupMenuBar() {
    QMenu* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&Connect to Server...",
                        this, &MainWindow::onConnectToServer);
    fileMenu->addSeparator();
    fileMenu->addAction("E&xit", qApp, &QApplication::quit,
                        QKeySequence::Quit);

    QMenu* viewMenu = menuBar()->addMenu("&View");
    viewMenu->addAction("&Refresh Keys (F5)", [this]() {
        m_keyBrowser->refresh();
    }, QKeySequence::Refresh);

    QMenu* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&About", this, &MainWindow::onAbout);
}

// ═══════════════════════════════════════════ 工具栏 ══════

void MainWindow::setupToolBar() {
    QToolBar* toolbar = addToolBar("Main");
    toolbar->setMovable(false);

    m_startServerAction = toolbar->addAction("Start Server");
    m_stopServerAction  = toolbar->addAction("Stop Server");
    toolbar->addSeparator();
    m_connectAction    = toolbar->addAction("Connect");
    m_disconnectAction = toolbar->addAction("Disconnect");
    toolbar->addSeparator();
    m_refreshAction    = toolbar->addAction("Refresh");

    connect(m_startServerAction, &QAction::triggered,
            this, &MainWindow::onStartServer);
    connect(m_stopServerAction, &QAction::triggered,
            this, &MainWindow::onStopServer);
    connect(m_connectAction, &QAction::triggered,
            this, &MainWindow::onConnectToServer);
    connect(m_disconnectAction, &QAction::triggered,
            this, &MainWindow::onDisconnectFromServer);
    connect(m_refreshAction, &QAction::triggered, [this]() {
        m_keyBrowser->refresh();
    });
}

// ═══════════════════════════════════════════ 中央布局 ══════

void MainWindow::setupCentralWidget() {
    m_tabWidget = new QTabWidget(this);
    m_tabWidget->addTab(m_valueViewer, "Value");
    m_tabWidget->addTab(m_businessPanel, "业务操作");
    m_tabWidget->addTab(m_statsDashboard, "Stats");

    QSplitter* hSplitter = new QSplitter(Qt::Horizontal, this);
    hSplitter->addWidget(m_keyBrowser);
    hSplitter->addWidget(m_tabWidget);
    hSplitter->setStretchFactor(0, 3);
    hSplitter->setStretchFactor(1, 7);

    QSplitter* vSplitter = new QSplitter(Qt::Vertical, this);
    vSplitter->addWidget(hSplitter);
    vSplitter->addWidget(m_console);
    vSplitter->setStretchFactor(0, 7);
    vSplitter->setStretchFactor(1, 3);

    setCentralWidget(vSplitter);
}

// ═══════════════════════════════════════════ 状态栏 ══════

void MainWindow::setupStatusBar() {
    statusBar()->addWidget(m_connectionStatus);
    statusBar()->addPermanentWidget(m_dbLabel);
}

// ═══════════════════════════════════════════ 信号连接 ══════

void MainWindow::setupConnections() {
    // ── ServerManager ──
    connect(m_serverMgr, &ServerManager::started,
            this, &MainWindow::onServerStarted);
    connect(m_serverMgr, &ServerManager::stopped,
            this, &MainWindow::onServerStopped);
    connect(m_serverMgr, &ServerManager::outputReceived,
            this, &MainWindow::onServerOutput);
    connect(m_serverMgr, &ServerManager::errorOccurred,
            this, [this](const QString& msg) {
        m_console->appendOutput(msg, true);
    });

    // ── RedisClient ──
    connect(m_client, &RedisClient::connected,
            this, &MainWindow::onClientConnected);
    connect(m_client, &RedisClient::disconnected,
            this, &MainWindow::onClientDisconnected);
    connect(m_client, &RedisClient::errorOccurred,
            this, &MainWindow::onClientError);

    // ── Console: 命令 => 直接发给 client（appendCommand 已在 onReturnPressed 中调用）──
    connect(m_console, &ConsolePanel::commandEntered, [this](const QStringList& args) {
        m_client->sendCommand(args);
    });

    // ── Key 选中 → ValueViewer 加载 ──
    connect(m_keyBrowser, &KeyBrowser::keySelected,
            this, &MainWindow::onKeySelected);

    // ── 设置 client（各组件直接使用 m_client）──
    m_keyBrowser->setClient(m_client);
    m_valueViewer->setClient(m_client);
    m_console->setClient(m_client);
    m_statsDashboard->setClient(m_client);
    m_businessPanel->setClient(m_client);

    // 业务操作导致数据变更 → 自动刷新 Key Browser
    connect(m_businessPanel, &BusinessPanel::dataChanged, [this]() {
        m_keyBrowser->refresh();
    });

    // Key Browser 中商品的增删 → 自动刷新业务面板商品列表
    connect(m_keyBrowser, &KeyBrowser::keysChanged, [this]() {
        m_businessPanel->refresh();
    });
}

// ═══════════════════════════════════════════ Server 管理 ══════

void MainWindow::onStartServer() {
    m_console->appendOutput("Starting mini_redis...");
    m_startServerAction->setEnabled(false);
    m_serverMgr->start();
}

void MainWindow::onStopServer() {
    m_serverMgr->stop();
}

void MainWindow::onServerStarted(int port) {
    m_serverRunning = true;
    m_console->appendOutput(
        QString("mini_redis started on port %1").arg(port));
    // 自动连接（connectToHost 内部已有防重连保护）
    m_client->connectToHost("127.0.0.1", port);
}

void MainWindow::onServerStopped() {
    m_serverRunning = false;
    m_console->appendOutput("mini_redis stopped");
    updateConnectionState();
}

void MainWindow::onServerOutput(const QString&) { /* 可选不显示 */ }

// ═══════════════════════════════════════════ 客户端连接 ══════

void MainWindow::onConnectToServer() {
    bool ok;
    QString text = QInputDialog::getText(this,
        "Connect to Redis Server",
        "Address (host:port):",
        QLineEdit::Normal,
        m_client->isConnected()
            ? QString("%1:%2").arg(m_client->host()).arg(m_client->port())
            : "127.0.0.1:6379",
        &ok);
    if (!ok || text.isEmpty()) return;

    QStringList parts = text.split(':');
    QString host = parts.value(0, "127.0.0.1");
    int port = parts.value(1, "6379").toInt();

    m_console->appendOutput(
        QString("Connecting to %1:%2...").arg(host).arg(port));
    m_client->connectToHost(host, port);  // 有防重连保护
}

void MainWindow::onDisconnectFromServer() {
    m_client->disconnectFromHost();
}

void MainWindow::onClientConnected() {
    m_clientConnected = true;
    updateConnectionState();

    QString info = QString("Connected %1:%2")
                       .arg(m_client->host()).arg(m_client->port());
    m_connectionStatus->setText(info);
    m_console->appendOutput(info);

    // 初始化：先 SELECT，再写入业务种子数据，最后刷新 Key Browser
    m_client->sendCommand(QStringList() << "SELECT" << "0");
    BusinessData::initIfEmpty(m_client, [this]() {
        m_keyBrowser->refresh();
        m_businessPanel->refresh();
    });
    m_statsDashboard->startAutoRefresh(5000);
}

void MainWindow::onClientDisconnected() {
    m_clientConnected = false;
    updateConnectionState();
    m_connectionStatus->setText("Disconnected");
    m_console->appendOutput("Disconnected from server");
    m_statsDashboard->stopAutoRefresh();
    m_valueViewer->clear();
}

void MainWindow::onClientError(const QString& msg) {
    m_console->appendOutput("Error: " + msg, true);
    statusBar()->showMessage(msg, 5000);
}

// ── Key 选中后加载 ──
void MainWindow::onKeySelected(const QString& key, int db,
                                const QString& type) {
    m_currentDb = db;
    m_dbLabel->setText(QString("DB%1").arg(db));
    m_valueViewer->loadKey(key, db, type);
}

// ═══════════════════════════════════════════ 状态 ══════

void MainWindow::updateConnectionState() {
    bool srv = m_serverRunning;
    bool con = m_clientConnected;

    m_startServerAction->setEnabled(!srv);
    m_stopServerAction->setEnabled(srv);
    m_connectAction->setEnabled(!con);
    m_disconnectAction->setEnabled(con);
    m_refreshAction->setEnabled(con);
    m_keyBrowser->setEnabled(con);
    m_valueViewer->setEnabled(con);
    m_statsDashboard->setEnabled(con);
    m_businessPanel->setEnabled(con);
}

void MainWindow::onAbout() {
    QMessageBox::about(this, "About MiniRedis Desktop",
        "<h3>MiniRedis Desktop Manager v1.0.0</h3>"
        "<p>Redis-compatible data management client built with Qt5 + C++11</p>"
        "<p>Features: One-click server start, Key browser, Multi-type editor, "
        "Command console, Performance dashboard</p>"
        "<p>GitHub: https://github.com/Auli-lai/Mini_Redis</p>");
}
