# 翻页笔鼠标控制器

> 将演示翻页笔的上键、下键、方框键映射为 Windows 鼠标操作，免驱动、无依赖、单文件。

[![Build & Release](https://github.com/Lisk809/pen-clicker/actions/workflows/release.yml/badge.svg)](https://github.com/Lisk809/pen-clicker/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-informational)
![Arch](https://img.shields.io/badge/arch-x64-lightgrey)

---

## 功能概览

| 按键 | 模式关闭时 | 模式开启时 |
|------|-----------|-----------|
| 上键 `Page Up` | 正常翻页 | 移动鼠标 ↑ / ← （按住持续加速） |
| 下键 `Page Down` | 正常翻页 | 移动鼠标 ↓ / → （按住持续加速） |
| 方框键 `.` 单击 | 正常输出 | 鼠标**左键**单击 |
| 方框键 `.` 双击 | 正常输出 | 切换移动轴 ↕ 垂直 / ↔ 水平 |
| 方框键 `.` 长按 | 正常输出 | 鼠标**右键**单击 |
| `Ctrl`+`Shift`+`F8` | — | 随时**开启 / 关闭**控制模式（全局热键） |

移动采用**加速曲线**：按下瞬间步长为 18 px，持续按住约 500 ms 后加速至 75 px，松开立即复位。

---

## 快速开始

### 直接下载

前往 [Releases](https://github.com/Lisk809/pen-clicker/releases/latest) 下载最新的 `pen_mouse.exe`，双击运行即可（会自动弹出 UAC 管理员权限请求）。

### 从源码编译

**MSVC（推荐）**

```bat
cl pen_mouse.cpp ^
   user32.lib gdi32.lib shell32.lib advapi32.lib ^
   /O2 /W3 /utf-8 /MT ^
   /Fe:pen_mouse.exe ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
```

**MinGW / GCC**

```bash
g++ -O2 -o pen_mouse.exe pen_mouse.cpp \
    -lgdi32 -luser32 -lshell32 -ladvapi32 \
    -mwindows -municode
```

> 无任何第三方依赖，仅使用 Win32 API 和 C++ 标准库。

---

## 系统要求

- **操作系统**：Windows 10 / 11，x64
- **权限**：管理员（`WH_KEYBOARD_LL` 低级键盘钩子必须）；程序启动时若非管理员身份会自动触发 UAC 提升
- **运行库**：无（使用 `/MT` 静态链接 CRT，单文件分发）

---

## 按键检测

不同品牌翻页笔的方框键（黑屏键 / 激光键）所发送的虚拟键码因厂商而异：

| 品牌 / 型号 | 上键 | 下键 | 方框键 |
|-------------|------|------|--------|
| 罗技 R400 / R800 | `Page Up` | `Page Down` | `b` 或 `.` |
| 国产杂牌（常见） | `Page Up` | `Page Down` | `F5` 或 `Esc` |
| 默认配置 | `Page Up` | `Page Down` | `.`（句点） |

若默认配置与你的翻页笔不符，使用程序界面内的**「按键检测」**功能：点击对应按钮后按一下翻页笔，程序自动识别并更新绑定，**无需重新编译**。

也可以直接修改源码第 70–72 行的虚拟键码常量：

```cpp
static std::atomic<DWORD> g_k_up  {VK_PRIOR};       // 上键   Page Up
static std::atomic<DWORD> g_k_dn  {VK_NEXT};          // 下键   Page Down
static std::atomic<DWORD> g_k_act {VK_OEM_PERIOD};    // 方框键 .
```

常用虚拟键码参考：`VK_F5`（0x74）、`VK_ESCAPE`（0x1B）、`VK_OEM_COMMA`（`,`）、`VK_OEM_2`（`/`）。

---

## 自动化构建

仓库已配置 GitHub Actions 工作流（`.github/workflows/release.yml`），推送 Tag 即自动编译发布：

```bash
git tag v1.0.0
git push origin v1.0.0
```

工作流将：
1. 在 `windows-latest` 上使用 MSVC 编译
2. 静态链接 CRT（`/MT`），生成独立 exe
3. 打包 `pen_mouse-v1.0.0-win-x64.zip`（含 exe 和 README）
4. 自动生成 Changelog（两个 Tag 之间的提交记录）
5. 创建 GitHub Release 并上传产物

版本号含 `dev` / `alpha` / `beta` 时自动标记为 **Pre-release**。

也可在 Actions 页面手动触发并自定义版本号。

---

## 实现原理

```
翻页笔（USB HID / 蓝牙）
        │  发送标准键盘事件
        ▼
  WH_KEYBOARD_LL 低级键盘钩子
  （运行在主消息线程，可拦截或放行）
        │
        ├─ 上键 / 下键按下  ──► 设置 atomic 标志 + 记录时间戳
        │
        ├─ 方框键释放       ──► SetTimer 判定单击 / 双击 / 长按
        │                        单击 → mouse_event(LEFT)
        │                        双击 → 切换移动轴
        │                        长按 → mouse_event(RIGHT)
        │
        └─ Ctrl+Shift+F8   ──► PostMessage 触发开关

  独立移动线程（~60 fps）
        │  读取 atomic 标志 + 时间戳计算加速步长
        ▼
    SetCursorPos(x + dx, y + dy)
```

全部共享状态均使用 `std::atomic<>`，无锁、无互斥量。

---

## 文件结构

```
.
├── pen_mouse.cpp              # 全部源码（单文件，约 700 行）
├── README.md
└── .github/
    └── workflows/
        └── release.yml        # 自动编译 & 发布工作流
```

---

## License

[MIT](LICENSE) © 2025
