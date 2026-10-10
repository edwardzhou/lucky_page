#ifndef _RECEIPT_DEFAULTS_H_
#define _RECEIPT_DEFAULTS_H_

#include "printer/receipt_params.h"
#include "printer/receipt_template.h"

/// 从 Settings 加载小票模板（单一来源）：
/// - `receipt_tmpl` 非空 → 解析 v3 JSON（解析失败则回退迁移，绝不让设备因坏配置打不出票）
/// - `receipt_tmpl` 为空 → 用旧 header/footer 迁移（v2 → v3 无损，设计 §7.3b）
ReceiptTemplateV3 LoadTemplateFromSettings();

/// 保存模板到它自己的模式槽（make_active=true 时同时切换生效模式）——设计 §7.3d
bool SaveTemplateToSettings(const ReceiptTemplateV3& t, bool make_active);

/// 升级迁移：把旧单槽模板归入其所属模式的槽
void MigrateTemplateSlots();

/// 把某个模式（0/1/2）或全部（-1）重置为出厂预设，并落盘（设计 §7.3e）
bool ResetTemplatesToPreset(int mode);

/// 参数来源：商家名/标题等取自 Settings，票号等奖项由调用方补齐
ReceiptParams MakeParamsFromSettings();

/// 内置演示模板（1=经典 2=简洁 3=促销）；越界返回默认模板。
/// 现场用串口 `pdemo <n>` 一键切换，便于快速确认版式效果。
ReceiptTemplateV3 BuiltinTemplate(int variant);

/// 自检页参数（固定票号/时间，便于与预期逐字节核对）
ReceiptParams MakeSampleParams();

#endif  // _RECEIPT_DEFAULTS_H_
