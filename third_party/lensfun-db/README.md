# lensfun 镜头数据库

本目录是 lensfun 官方镜头数据库（`version_1` 格式），随程序一起分发，
用于镜头畸变、暗角与横向色差校正。

- **来源**：lensfun 0.3.4 源码包 `data/db`
  （<https://github.com/lensfun/lensfun>）
- **数据库版本**：`version_1`，共 56 个 XML 文件 + `timestamp.txt`
- **许可**：Creative Commons Attribution-ShareAlike 3.0
  （见同目录 `cc-by-sa-3.0.txt`）

## 更新方式

1. 从 lensfun 上游仓库获取最新 `data/db`（或 lensfun-db 发布包）；
2. 用新内容整体替换 `version_1/` 目录；
3. 同步更新 `timestamp.txt` 与上方记录的版本信息；
4. 重新构建，CMake 会把数据库复制到可执行文件旁的 `lensfun/version_1`。

程序启动渲染时会按以下顺序查找数据库：

1. 环境变量 `RAWEDIT_LENSFUN_DB` 指向的目录；
2. 可执行文件同级的 `lensfun/version_1`；
3. 可执行文件上两级的 `share/lensfun/version_1`（Linux 安装布局）；
4. Windows：`%ProgramData%\lensfun\version_1`；
5. Linux：`/usr/share/lensfun/version_1`、`/usr/local/share/lensfun/version_1`；
6. lensfun 自身的默认查找路径（兜底）。
