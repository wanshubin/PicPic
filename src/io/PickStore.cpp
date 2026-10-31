// ============================================================================
//  PickStore.cpp —— 选图侧车 JSON 读写
// ============================================================================

#include "PickStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace rawio {

namespace {

constexpr const char* kSidecarName = ".rawedit.pick.json";
constexpr int kSchemaVersion = 1;

QString normalizeKey(const QString& relativePath)
{
    // 统一分隔符并去掉开头的 "./"，保证 Windows / POSIX 下键一致
    QString key = QDir::fromNativeSeparators(relativePath);
    while (key.startsWith(QStringLiteral("./"))) {
        key.remove(0, 2);
    }
    return key;
}

}  // namespace

PickStore::PickStore(QString directory)
    : m_directory(std::move(directory))
{
    m_directory = QDir(m_directory).absolutePath();
}

QString PickStore::sidecarFileName()
{
    return QString::fromLatin1(kSidecarName);
}

QString PickStore::sidecarPath(const QString& directory)
{
    return QDir(directory).absoluteFilePath(sidecarFileName());
}

bool PickStore::load()
{
    m_entries.clear();
    m_log.clear();

    QFile file(filePath());
    if (!file.exists()) {
        return true;  // 新目录：空存储
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        // 侧车损坏时不要覆盖用户的原始数据，交由上层提示
        return false;
    }

    const QJsonObject root = document.object();
    const QJsonObject files = root.value(QStringLiteral("files")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QJsonObject object = it.value().toObject();
        PickEntry entry;
        entry.star = std::clamp(object.value(QStringLiteral("star")).toInt(0), 0, 5);
        entry.accepted = object.value(QStringLiteral("accepted")).toBool(false);
        entry.rejected = object.value(QStringLiteral("rejected")).toBool(false);
        entry.colorLabel = object.value(QStringLiteral("label")).toString();
        if (!entry.isEmpty()) {
            m_entries.insert(it.key(), entry);
        }
    }

    const QJsonArray log = root.value(QStringLiteral("log")).toArray();
    m_log.reserve(log.size());
    for (const QJsonValue& value : log) {
        const QJsonObject object = value.toObject();
        PickLogEntry entry;
        entry.time = QDateTime::fromString(
            object.value(QStringLiteral("time")).toString(),
            QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        entry.action = object.value(QStringLiteral("action")).toString();
        entry.source = object.value(QStringLiteral("source")).toString();
        entry.target = object.value(QStringLiteral("target")).toString();
        // 颜色为后加的字段：旧侧车没有这个键，读成空串即可
        entry.color = object.value(QStringLiteral("color")).toString();
        if (!entry.action.isEmpty()) {
            m_log.append(entry);
        }
    }
    return true;
}

bool PickStore::save() const
{
    if (m_directory.isEmpty()) {
        return false;
    }

    QJsonObject files;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it.value().isEmpty()) {
            continue;
        }
        QJsonObject object;
        if (it.value().star > 0) {
            object.insert(QStringLiteral("star"), it.value().star);
        }
        if (it.value().accepted) {
            object.insert(QStringLiteral("accepted"), true);
        }
        if (it.value().rejected) {
            object.insert(QStringLiteral("rejected"), true);
        }
        if (!it.value().colorLabel.isEmpty()) {
            object.insert(QStringLiteral("label"), it.value().colorLabel);
        }
        files.insert(it.key(), object);
    }

    QJsonArray log;
    for (const PickLogEntry& entry : m_log) {
        QJsonObject object;
        object.insert(QStringLiteral("time"),
                      entry.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        object.insert(QStringLiteral("action"), entry.action);
        object.insert(QStringLiteral("source"), entry.source);
        object.insert(QStringLiteral("target"), entry.target);
        if (!entry.color.isEmpty()) {
            object.insert(QStringLiteral("color"), entry.color);
        }
        log.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), kSchemaVersion);
    root.insert(QStringLiteral("files"), files);
    root.insert(QStringLiteral("log"), log);

    // QSaveFile：写入失败时不破坏已有侧车
    QSaveFile file(filePath());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.commit();
}

PickEntry PickStore::entry(const QString& relativePath) const
{
    return m_entries.value(normalizeKey(relativePath), PickEntry{});
}

PickEntry& PickStore::mutableEntry(const QString& relativePath)
{
    const QString key = normalizeKey(relativePath);
    return m_entries[key];
}

bool PickStore::setStar(const QString& relativePath, int star)
{
    PickEntry& entry = mutableEntry(relativePath);
    entry.star = std::clamp(star, 0, 5);
    pruneEmpty();
    return save();
}

bool PickStore::setAccepted(const QString& relativePath, bool accepted)
{
    PickEntry& entry = mutableEntry(relativePath);
    entry.accepted = accepted;
    if (accepted) {
        // 接受与拒绝互斥，避免列表筛选时同时命中两种状态
        entry.rejected = false;
    }
    pruneEmpty();
    return save();
}

bool PickStore::setRejected(const QString& relativePath, bool rejected)
{
    PickEntry& entry = mutableEntry(relativePath);
    entry.rejected = rejected;
    if (rejected) {
        entry.accepted = false;
    }
    pruneEmpty();
    return save();
}

bool PickStore::setColorLabel(const QString& relativePath, const QString& label)
{
    PickEntry& entry = mutableEntry(relativePath);
    entry.colorLabel = label;
    pruneEmpty();
    return save();
}

int PickStore::clearColorLabel(const QString& label)
{
    if (label.isEmpty()) {
        return 0;
    }
    int cleared = 0;
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if (it.value().colorLabel == label) {
            it.value().colorLabel.clear();
            ++cleared;
        }
    }
    if (cleared > 0) {
        pruneEmpty();
        save();
    }
    return cleared;
}

bool PickStore::renameEntry(const QString& oldRelativePath,
                            const QString& newRelativePath)
{
    const auto it = m_entries.find(oldRelativePath);
    if (it == m_entries.end()) {
        return true;  // 没有标记也算成功
    }
    const PickEntry entry = it.value();
    m_entries.erase(it);
    m_entries.insert(newRelativePath, entry);
    return save();
}

bool PickStore::addLog(const QString& action, const QString& source, const QString& target,
                       const QString& color)
{
    PickLogEntry entry;
    entry.time = QDateTime::currentDateTime();
    entry.action = action;
    entry.source = source;
    entry.target = target;
    entry.color = color;
    m_log.append(entry);

    // 只保留最近的记录，避免侧车文件无限增长
    while (m_log.size() > kMaxLogEntries) {
        m_log.removeFirst();
    }
    return save();
}

void PickStore::clearLog()
{
    m_log.clear();
    save();
}

QString PickStore::formatLogLine(const PickLogEntry& entry)
{
    // 日志只保留挑片时最需要的信息：时间、动作、图片名、落到了哪个子目录、什么颜色。
    // 目标存的是完整文件路径，取它所在目录的名字（"最低级目录"）；
    // 完整路径仍然保留在侧车里，需要时可以直接查。
    const QString folder = QFileInfo(entry.target).dir().dirName();
    const QString targetName = folder.isEmpty() ? entry.target : folder;
    const QString fileName = QFileInfo(entry.source).fileName();
    QString line;
    if (entry.action == QStringLiteral("删除")) {
        // 改 / 清标记会删掉旧目标目录里的副本，日志里要能和复制区分开
        line = QStringLiteral("[%1] 删除 %2 的副本（%3）")
                   .arg(entry.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
                        fileName, targetName);
    } else if (entry.action == QStringLiteral("跳过")) {
        // 目标目录里已有内容完全相同的副本，本次没有重复写入
        line = QStringLiteral("[%1] 跳过 %2（%3 已有相同文件）")
                   .arg(entry.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
                        fileName, targetName);
    } else {
        line = QStringLiteral("[%1] %2 %3 → %4")
                   .arg(entry.time.toString(QStringLiteral("MM-dd HH:mm:ss")),
                        entry.action.isEmpty() ? QStringLiteral("复制") : entry.action,
                        fileName, targetName);
    }
    if (!entry.color.isEmpty()) {
        line += QLatin1Char(' ') + entry.color;
    }
    return line;
}

void PickStore::pruneEmpty()
{
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        if (it.value().isEmpty()) {
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace rawio
