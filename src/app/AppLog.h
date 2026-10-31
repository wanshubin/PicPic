// ============================================================================
//  AppLog.h —— 应用日志（需求 §3 模块 G：日志输出、F3 日志面板）
//
//  职责：
//   1. 提供全局日志写入接口（不依赖 UI，core/io 层也可调用）
//   2. 把日志同时写入磁盘文件，便于事后排查
//   3. 通过信号转发到 UI 的日志面板
//
//  设计：单例 + 信号。core 层不依赖 Qt，因此 core 不写日志到这里；
//  日志主要由 app 与 ui 层以及它们转发的 core 错误信息产生。
// ============================================================================

#pragma once

#include <QObject>
#include <QString>

namespace rawapp {

/// 日志级别（与 ui/LogPanel 的 LogLevel 对应）
enum class AppLogLevel {
    Info,
    Warning,
    Error,
};

/// 应用日志单例
class AppLog : public QObject {
    Q_OBJECT

public:
    ~AppLog() override;

    /// 取得单例
    static AppLog& instance();

    /// 设置日志文件路径。默认为用户数据目录下的 picpic.log
    void setLogFilePath(const QString& path);
    QString logFilePath() const;

    /// 写入日志
    void write(AppLogLevel level, const QString& message);

    /// 便捷方法
    void info(const QString& message);
    void warning(const QString& message);
    void error(const QString& message);

    /// 是否把日志同时写入文件（默认开启）
    void setFileLoggingEnabled(bool enabled);
    bool isFileLoggingEnabled() const;

signals:
    /// 有新日志（UI 日志面板连接此信号）
    void messageLogged(int level, const QString& message);

private:
    AppLog();
    Q_DISABLE_COPY(AppLog)

    /// 确保日志文件可以打开
    bool ensureFileOpen();

    class Impl;
    Impl* m_impl;
};

/// 全局便捷访问宏
inline AppLog& appLog() { return AppLog::instance(); }

}  // namespace rawapp
