// ============================================================================
//  PortableMode.h —— 免安装（便携）模式判定
//
//  免安装包在 exe 同目录放一个 portable.ini 作为标记：
//   - QSettings 改用 INI 文件并存到程序目录，不写注册表；
//   - 日志写入程序目录下的 logs/，随整个文件夹一起拷贝或删除。
//  开发构建没有该标记，行为与原来一致（注册表 + 用户 AppData 目录）。
// ============================================================================

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace rawapp {

/// 便携模式标记文件：exe 同目录下的 portable.ini
inline QString portableMarkerPath()
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("portable.ini"));
}

/// 当前是否以便携模式运行
inline bool isPortableMode()
{
    return QFileInfo::exists(portableMarkerPath());
}

/// 便携模式下的数据目录（程序目录）；非便携模式返回空，由调用方回退到系统路径
inline QString portableDataDir()
{
    return isPortableMode() ? QCoreApplication::applicationDirPath() : QString();
}

}  // namespace rawapp
