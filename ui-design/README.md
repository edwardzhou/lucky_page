# Lucky Studio · 幸运翻页机 UI 重设计

基于 `lucky-page_spiffs_contents.zip` 中的六页设备控制台，完成专业应用风格重设计。保留原有页面地址和设备接口，并修正奖池库存关联、WiFi 重复 ID、小票版式状态等问题。

## 直接体验

解压后，在现代桌面浏览器打开根目录的 **`preview.html`**。这是包含全部页面、样式、脚本与模拟接口的单文件交互预览，无需安装依赖或连接设备。

可以体验模拟开奖、奖池编辑、小票模板、品牌素材、行程标定和 WiFi 管理。演示环境有明确提示；操作不会驱动执行器或真实打印。模拟 Logo 上传只保存在浏览器，不传至设备。

侧栏“重置演示数据”可恢复初始样例。真实设备的随机、打印和机械行为仍须联调。

## 包内文件

| 文件 / 目录 | 用途 |
| --- | --- |
| `preview.html` | 完整单文件交互预览 |
| `index.html` | 打开预览的快捷入口 |
| `spiffs_contents/` | 可用于设备集成的 HTML / CSS / JS |
| `docs/UI-analysis-and-redesign.md` | 原 UI 分析、设计规范、页面方案与接口边界 |
| `docs/validation.md` | 自动检查、浏览器验证和真机验收范围 |
| `docs/file-manifest.json` | 文件大小与 SHA-256 清单 |
| `reference/original-source.zip` | 上传源码的干净备份，原文件内容不变 |
| `tools/build_preview.py` | 从设备前端源码重新生成离线预览 |
| `tools/verify.py` | 静态结构、语法及模拟协议验证 |
| `tools/test-demo.mjs` | 27 条环境隔离与模拟接口断言 |
| `tools/icons.json` | 本地 SVG 图标路径资料 |

## 设备集成

1. 保留当前设备固件与文件系统备份。
2. 按现有流程将 `spiffs_contents/` 内资源打包到 SPIFFS。根目录预览和文档不需要装入设备。
3. 若固件按白名单提供静态资源，加入新增的 `workspace.js`、`prizes.js`、`demo.js`、`page-enhancements.js`，并确认 JavaScript / CSS MIME 类型。
4. 访问设备原有首页地址，**不添加 `?demo=1`**。默认使用真实同源 `/api/*`，不会在接口失败时回退到假数据。
5. 在目标设备验收机械归零、开奖、奖池保存、库存、三种版式、打印、Logo 和 WiFi。

六个原有页面地址保持：`index.html`、`prizes.html`、`receipt.html`、`logo.html`、`calibrate.html`、`wifi.html`。

本次输入没有固件源码，不能确认静态文件路由、分区余量、接口版本、执行器时序、打印机兼容性及网络切换后的实际表现。因此这是经过前端和模拟交互验证的 UI 集成候选版本。

## 开发与验证

修改源码后生成单文件预览：

```sh
python3 tools/build_preview.py
```

运行检查（需要 Python 3 和 Node.js；设备运行不依赖 Node.js）：

```sh
python3 tools/verify.py
```

如需通过 HTTP 调试设备目录，在 `spiffs_contents/` 运行：

```sh
python3 -m http.server 8000
```

打开 `http://localhost:8000/index.html?demo=1` 使用模拟接口。取消 `?demo=1` 会访问同源真实 API，普通静态服务器将显示设备连接失败。

直接双击设备目录中的 HTML 不适合完整调试。离线评审请使用根目录的 `preview.html`。
