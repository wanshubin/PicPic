// ============================================================================
//  AppLog.cpp —— 应用日志实现
// ============================================================================

#include "AppLog.h"

#include "PortableMode.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

namespace rawapp {

// ===========================================================================
//  Impl
// ===========================================================================

class AppLog::Impl {
public:
    QString logFilePath;
    bool fileLoggingEnabled = true;

    /// 日志文件句柄。按需打开，重开后追加。
    QFile* file = nullptr;

    /// 多线程保护。渲染工作线程也可能写日志，必须加锁。
    QMutex mutex;

    ~Impl()
    {
        if (file != nullptr) {
            file->close();
            delete file;
            file = nullptr;
        }
    }
};

// ===========================================================================
//  单例
// ===========================================================================

AppLog& AppLog::instance()
{
    // C++11 起局部静态变量的初始化是线程安全的，无需额外加锁
    static AppLog instance;
    return instance;
}

AppLog::AppLog()
    : QObject(nullptr)
    , m_impl(new Impl())
{
    // 默认日志路径：用户应用数据目录下的 logs/picpic.log；
    // 便携模式（exe 旁有 portable.ini）则落在程序目录，随文件夹一起带走。
    QString dataDir = portableDataDir();
    if (dataDir.isEmpty()) {
        dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    if (dataDir.isEmpty()) {
        dataDir = QDir::homePath();
    }

    QDir dir(dataDir);
    if (!dir.exists(QStringLiteral("logs"))) {
        dir.mkpath(QStringLiteral("logs"));
    }

    m_impl->logFilePath = dir.absoluteFilePath(QStringLiteral("logs/picpic.log"));
}

AppLog::~AppLog()
{
    delete m_impl;
    m_impl = nullptr;
}

// ===========================================================================
//  配置
// ===========================================================================

void AppLog::setLogFilePath(const QString& path)
{
    QMutexLocker locker(&m_impl->mutex);

    if (m_impl->file != nullptr) {
        m_impl->file->close();
        delete m_impl->file;
        m_impl->file = nullptr;
    }

    m_impl->logFilePath = path;
}

QString AppLog::logFilePath() const
{
    return m_impl->logFilePath;
}

void AppLog::setFileLoggingEnabled(bool enabled)
{
    m_impl->fileLoggingEnabled = enabled;
}

bool AppLog::isFileLoggingEnabled() const
{
    return m_impl->fileLoggingEnabled;
}

// ===========================================================================
//  写入
// ===========================================================================

bool AppLog::ensureFileOpen()
{
    if (m_impl->file != nullptr && m_impl->file->isOpen()) {
        return true;
    }

    if (m_impl->logFilePath.isEmpty()) {
        return false;
    }

    // 确保目录存在（用户可能改动过路径）
    const QFileInfo info(m_impl->logFilePath);
    if (!info.absoluteDir().exists()) {
        QDir().mkpath(info.absolutePath());
    }

    if (m_impl->file == nullptr) {
        m_impl->file = new QFile(m_impl->logFilePath);
    }

    // 追加模式打开；多次运行复用同一日志文件
    return m_impl->file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void AppLog::write(AppLogLevel level, const QString& message)
{
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    QString levelText;
    switch (level) {
    case AppLogLevel::Info: levelText = QStringLiteral("INFO "); break;
    case AppLogLevel::Warning: levelText = QStringLiteral("WARN "); break;
    case AppLogLevel::Error: levelText = QStringLiteral("ERROR"); break;
    }

    const QString line = QStringLiteral("%1 [%2] %3").arg(timestamp, levelText, message);

    // ---- 写文件 ----
    if (m_impl->fileLoggingEnabled) {
        QMutexLocker locker(&m_impl->mutex);
        if (ensureFileOpen()) {
            QTextStream stream(m_impl->file);
            stream << line << '\n';
            stream.flush();
        }
    }

    // ---- 转发到 UI ----
    // 信号在发出线程的事件循环中投递；UI 侧用排队连接可保证线程安全
    emit messageLogged(static_cast<int>(level), message);
}

void AppLog::info(const QString& message)
{
    write(AppLogLevel::Info, message);
}

void AppLog::warning(const QString& message)
{
    write(AppLogLevel::Warning, message);
}

void AppLog::error(const QString& message)
{
    write(AppLogLevel::Error, message);
}

}  // namespace rawapp
