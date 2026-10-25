#include <QApplication>
#include "mainwindow.h"
#include "log.h"

int main(int argc, char* argv[]) {
    Log::init("MiniRedisDesktop.log");
    Log::info("MiniRedisDesktop v1.0.0 starting...");

    QApplication app(argc, argv);
    app.setApplicationName("MiniRedisDesktop");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("MiniRedis");

    // 全局现代风格样式表
    app.setStyleSheet(
        "QMainWindow { background-color: #f5f5f5; }"
        "QToolBar { background: #e8e8e8; border-bottom: 1px solid #ccc; "
        "           padding: 4px; spacing: 6px; }"
        "QToolBar QToolButton { padding: 4px 10px; border: 1px solid transparent; "
        "                      border-radius: 3px; }"
        "QToolBar QToolButton:hover { background: #d0d0d0; border: 1px solid #aaa; }"
        "QTreeWidget { border: 1px solid #ccc; background: white; "
        "              alternate-background-color: #f9f9f9; }"
        "QTreeWidget::item { padding: 3px; }"
        "QTreeWidget::item:selected { background: #0078d4; color: white; }"
        "QTableWidget { border: 1px solid #ccc; background: white; "
        "               gridline-color: #e0e0e0; }"
        "QTableWidget::item { padding: 3px; }"
        "QTableWidget::item:selected { background: #0078d4; color: white; }"
        "QSplitter::handle { background: #ccc; width: 1px; }"
        "QStatusBar { background: #e8e8e8; border-top: 1px solid #ccc; }"
        "QMenuBar { background: #e8e8e8; border-bottom: 1px solid #ccc; }"
        "QMenuBar::item:selected { background: #0078d4; color: white; }"
        "QTabWidget::pane { border: 1px solid #ccc; }"
        "QTabBar::tab { padding: 6px 16px; border: 1px solid #ccc; "
        "              border-bottom: none; background: #e8e8e8; }"
        "QTabBar::tab:selected { background: white; }"
        "QPushButton { padding: 4px 14px; border: 1px solid #aaa; "
        "              border-radius: 3px; background: #f0f0f0; }"
        "QPushButton:hover { background: #e0e0e0; }"
        "QPushButton:pressed { background: #d0d0d0; }"
        "QLineEdit { border: 1px solid #aaa; border-radius: 2px; "
        "           padding: 4px; }"
    );

    MainWindow mainWindow;
    mainWindow.setWindowTitle("MiniRedis Desktop Manager");
    mainWindow.resize(1100, 700);
    mainWindow.show();

    int ret = app.exec();
    Log::info("MiniRedisDesktop exiting (code=%d)", ret);
    Log::close();
    return ret;
}
