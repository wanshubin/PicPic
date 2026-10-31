// ============================================================================
//  PickStore.h —— 选图标记的侧车存储（不修改 RAW / EXIF）
//
//  需求 §5.1：星级、接受 / 拒绝标记只写进源目录下的
//  `.rawedit.pick.json`，原始文件保持原样。
//
//  文件结构（v1）：
//    {
//      "version": 1,
//      "files": {
//        "_DSC3990.NEF": { "star": 3, "accepted": true, "rejected": false }
//      },
//      "log": [
//        { "time": "2026-10-25 20:10:04", "action": "复制",
//          "source": "...", "target": "...", "color": "红色" }
//      ]
//    }
//
//  键为相对源目录的路径（子目录用 '/' 分隔），因此递归扫描时同一个
//  侧车文件可以覆盖整棵目录树；默认非递归时它就是"图片同目录"。
// ============================================================================

#pragma once

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace rawio {

/// 单张图片的标记
struct PickEntry {
    int star = 0;              ///< 0 = 未评分，1~5
    bool accepted = false;     ///< 接受（保留）
    bool rejected = false;     ///< 拒绝（待清理）
    QString colorLabel;        ///< 颜色标签（Red / Yellow / Green / Blue / Purple）

    /// 是否为空（没有任何标记）。空条目不会写进侧车文件。
    bool isEmpty() const
    {
        return star <= 0 && !accepted && !rejected && colorLabel.isEmpty();
    }
};

/// 分拣日志条目
struct PickLogEntry {
    QDateTime time;
    QString action;
    QString source;
    QString target;
    QString color;  ///< 目标目录对应的标记颜色名（界面显示用，可为空）
};

/// 一个源目录对应的标记存储
class PickStore {
public:
    explicit PickStore(QString directory);

    /// 侧车文件名（需求中的占位名替换为本软件的实际命名）
    static QString sidecarFileName();

    /// 指定目录下的侧车完整路径
    static QString sidecarPath(const QString& directory);

    /// 读取侧车；文件不存在时视为空存储。解析失败会保留空状态并返回 false。
    bool load();

    /// 原子写入侧车（QSaveFile）。
    bool save() const;

    QString directory() const { return m_directory; }
    QString filePath() const { return sidecarPath(m_directory); }

    /// 查询标记（键为相对路径）
    PickEntry entry(const QString& relativePath) const;

    // 标记修改后立即落盘：挑片过程中随时可能切走或崩溃，
    // 不能等到关闭窗口才保存。
    // @return 是否写入成功（源目录只读等情况下为 false，内存状态仍然生效）
    bool setStar(const QString& relativePath, int star);
    bool setAccepted(const QString& relativePath, bool accepted);
    bool setRejected(const QString& relativePath, bool rejected);
    bool setColorLabel(const QString& relativePath, const QString& label);

    /// 把所有使用这个颜色标签的条目恢复为未标记。
    /// 批量操作只落盘一次，避免逐张写侧车。
    /// @return 被清理的条目数量
    int clearColorLabel(const QString& label);

    /// 文件重命名后迁移标记（F10 批量重命名）
    bool renameEntry(const QString& oldRelativePath, const QString& newRelativePath);

    /// 追加一条分拣日志（最多保留 kMaxLogEntries 条）
    /// @param color 目标目录的标记颜色名，仅用于日志显示
    /// @return 是否写入成功
    bool addLog(const QString& action, const QString& source, const QString& target,
                const QString& color = QString());

    const QVector<PickLogEntry>& logEntries() const { return m_log; }
    void clearLog();

    /// 把日志格式化为可显示的单行文本
    static QString formatLogLine(const PickLogEntry& entry);

    static constexpr int kMaxLogEntries = 500;

private:
    /// 取出一条可修改的标记；不存在时创建
    PickEntry& mutableEntry(const QString& relativePath);

    /// 清理空条目，避免侧车里堆积无意义的 {}
    void pruneEmpty();

    QString m_directory;
    QHash<QString, PickEntry> m_entries;
    QVector<PickLogEntry> m_log;
};

}  // namespace rawio
