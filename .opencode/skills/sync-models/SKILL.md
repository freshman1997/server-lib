---
name: sync-models
description: >
  从 dodjoy 路由 API 拉取最新模型列表，与 opencode config.json 对比后自动
  同步：新增上线模型（含 context/output/modalities 参数）、移除下线模型、
  保留已有模型的参数配置不变。参数来源按优先级依次为：脚本内置默认值 →
  本地 model_cache.json → litellm GitHub 数据库（精确/模糊匹配+vision推断）
  → dodjoy LLM 联网查询 → 兜底值。当用户想要同步/更新/查询 dodjoy 可用
  模型、添加新模型、移除下线模型，或询问"dodjoy 有哪些模型"时使用此 skill。
  触发词包括：「同步模型」「更新模型列表」「dodjoy 模型」「config 模型配置」
  「模型上线/下线」。即使用户没有明确说"同步"，只要涉及 dodjoy 路由的模型
  管理，都应使用此 skill。
---

# Skill: sync-models

从 dodjoy 路由 API 拉取最新模型列表，与本地 `~/.config/opencode/config.json`
对比，自动补全新增模型、移除下线模型，并保留已有模型的详细参数配置。

## 使用前提

脚本会按以下优先级读取 API Key，无需额外配置：

1. **config.json**（推荐）：`provider.dodjoy.options.apiKey` 字段
2. **环境变量**（fallback）：`OPENAI_API_KEY`

只要 config.json 中已配置有效的 apiKey，直接执行脚本即可。

## 执行方式

```bash
node {baseDir}/scripts/sync_models.mjs
```

将 `{baseDir}` 替换为本 skill 的实际安装路径，例如：
- **全局安装（Windows）**：`%USERPROFILE%\.config\opencode\skills\sync-models`
- **全局安装（Linux/Mac）**：`~/.config/opencode/skills/sync-models`
- **项目安装**：`.opencode/skills/sync-models`

## 脚本行为说明

1. 向 `https://airouter.dodjoy.com/v1/models` 发起请求，获取当前可用模型列表（**唯一来源**）
2. 读取 `~/.config/opencode/config.json` 中 `provider.dodjoy.models` 节点
3. 计算差异：
   - **新增**：远端有、config 没有 → 自动查询参数后补全
   - **移除**：config 有、远端没有 → 从 config 删除
   - **保留**：两边都有 → 保持 config 中已有的参数不变
4. 将更新后的 models 写回 config.json

## context/output 参数查询优先级

对于需要获取参数的模型，按以下顺序查询，命中即停止：

| 优先级 | 来源 | 说明 |
|--------|------|------|
| 1 | `MODEL_DEFAULTS`（脚本内置） | 已知模型的硬编码值，最快 |
| 2 | `model_cache.json`（本地缓存） | 上次查询结果，无网络消耗 |
| 3 | litellm GitHub 数据库 | 拉取 `model_prices_and_context_window.json`，支持精确/模糊匹配，同时推断 vision 能力 |
| 4 | dodjoy LLM API 联网查询 | 让 LLM 自述参数，保守设为 text-only |
| 5 | 兜底值 | `context=128000, output=32000` |

来源 3、4 查到的结果会**自动写入 `model_cache.json`**，下次同步时直接命中缓存。

## 内置默认参数

脚本内置了已知模型的 `context` / `output` / `modalities` 默认值（基于官方文档）。

已知的模型默认参数（可在脚本 `MODEL_DEFAULTS` 中手动维护）：

| 系列 | context | output |
|------|---------|--------|
| Claude Sonnet 4.x | 200,000 | 64,000 |
| Claude Haiku 4.x | 200,000 | 32,000 |
| Gemini 2.5 / 3 | 1,048,576 | 65,536 |
| GPT-5.x (dodjoy) | 400,000~1,000,000 | 128,000 |
| Kimi K2.x | 128,000 | 32,000 |
| GLM-4.x / 5 | 128,000 | 32,000~128,000 |
| MiniMax M2.x | 204,800 | 64,000 |

## 输出示例

```
正在从 https://airouter.dodjoy.com/v1/models 拉取模型列表...
拉取到 19 个模型。

新增模型：
  gemini-3-pro-preview
  some-unknown-model

已移除模型（下线）：
  MiniMax-M2.1

正在解析新模型参数...
  🔍 未知模型 "gemini-3-pro-preview"，查询 litellm 数据库...
  📡 正在从 litellm 拉取模型参数数据库...
  ✅ litellm 数据库加载完成（3000+ 条记录）
  ✅ litellm 命中（fuzzy，key="gemini/gemini-3-pro-preview"）：context=2097152, output=8192, vision=true
  🔍 未知模型 "some-unknown-model"，查询 litellm 数据库...
  🌐 litellm 未收录，尝试 LLM 联网查询...
  ✅ LLM 查询成功：context=128000, output=32000

参数来源统计：builtin:17, litellm:1, network:1

✅ config.json 已更新（+2 / -1）。
   路径：~/.config/opencode/config.json
```

## 常见问题

- **`未找到 API Key`**：检查 config.json 中 `provider.dodjoy.options.apiKey` 是否已填写，或设置环境变量 `OPENAI_API_KEY`
- **HTTP 401**：key 无效或已过期
- **HTTP 403**：账号没有模型列表权限
- **`未找到 provider.dodjoy`**：config.json 结构不对，检查是否存在 `provider.dodjoy` 节点
- **litellm 拉取失败**：网络问题，脚本会自动降级到 LLM 联网查询，不影响整体流程
