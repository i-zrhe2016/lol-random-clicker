# 随机鼠标点击工具

在指定屏幕区域内随机点击鼠标右键，间隔 15~30 秒，支持跨程序控制。

---

## 下载使用

直接运行 `random_clicker.exe`，无需安装任何依赖。

> 首次运行会弹出 UAC 提权窗口，点击「是」即可（需要管理员权限才能控制其他程序）。

---

## 使用步骤

1. **双击** `random_clicker.exe`
2. 弹出 UAC 提权确认，点击「是」
3. 控制台显示 **10 秒倒计时**，此时切换到目标程序窗口
4. 倒计时结束后弹出**全屏拉框界面**（显示当前屏幕内容）
5. **按住鼠标左键拖动**选择点击区域，松开确认
6. 控制台显示 3 秒倒计时后开始随机点击
7. 按 **Ctrl+C** 停止

---

## 命令行参数

```
random_clicker.exe [x1 y1 x2 y2] [次数] [最小间隔ms] [最大间隔ms]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| x1 y1 | 区域左上角坐标 | 手动拉框 |
| x2 y2 | 区域右下角坐标 | 手动拉框 |
| 次数 | 点击总次数，0 = 无限循环 | 0 |
| 最小间隔ms | 每次点击最小等待毫秒 | 15000 |
| 最大间隔ms | 每次点击最大等待毫秒 | 30000 |

**示例：**

```bat
# 手动拉框，无限循环，间隔 15~30 秒
random_clicker.exe

# 指定坐标，点击 20 次，间隔 10~20 秒
random_clicker.exe 100 100 800 600 20 10000 20000
```

---

## 注意事项

- 拉框界面按 **ESC** 可取消退出
- 拉框时可以看到屏幕内容，选框外区域会加半透明遮罩
- 拉框界面右上角实时显示当前选区坐标和尺寸
- 支持高分屏（已声明 PerMonitorV2 DPI 感知，坐标不会偏移）

---

## 编译（Linux 交叉编译）

```bash
# 安装依赖
sudo apt install mingw-w64

# 编译资源文件
x86_64-w64-mingw32-windres random_clicker.rc -o random_clicker_res.o

# 编译 exe
x86_64-w64-mingw32-gcc random_clicker_win.c random_clicker_res.o \
  -o random_clicker.exe -lgdi32 -luser32 -lmsimg32 -mwindows -mconsole
```
