#!/usr/bin/env node
/**
 * sync_models.mjs
 * 从 dodjoy 路由拉取可用模型列表，与 opencode config.json 中的 models 对比，
 * 补全缺失的模型，移除已下线的模型，并保留已有模型的详细配置。
 *
 * 模型列表唯一来源：dodjoy /v1/models
 * context/output 参数查询优先级：
 *   1. MODEL_DEFAULTS（脚本内置）
 *   2. model_cache.json（本地缓存）
 *   3. litellm model_prices_and_context_window.json（GitHub 远端）
 *   4. dodjoy LLM API 联网查询
 *   5. 兜底值 128000 / 32000
 *
 * 查到的结果（来源 3/4）会自动写入 model_cache.json 并回填 MODEL_DEFAULTS 注释区。
 */

import { readFileSync, writeFileSync, existsSync, unlinkSync } from "fs";
import { homedir, tmpdir } from "os";
import { join, dirname } from "path";
import { fileURLToPath } from "url";
import { execSync, execFileSync } from "child_process";

// ── 路径 ──────────────────────────────────────────────────────────────────────

const __dir      = dirname(fileURLToPath(import.meta.url));
const SKILL_DIR  = join(__dir, "..");
const CACHE_PATH = join(SKILL_DIR, "model_cache.json");
const CONFIG_PATH = join(homedir(), ".config", "opencode", "opencode.json");

const DODJOY_BASE_URL = "https://airouter.dodjoy.com";
const LITELLM_PRICES_URL =
  "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json";

// 查询时读取 config 的默认模型，fallback 到第一个可用模型
let QUERY_MODEL = null; // 延迟初始化，在读取 config 后赋值

// litellm 数据，懒加载（只在需要时拉取一次）
let _litellmData = null;

// ── 探测可用 shell（用于 curl 走系统代理）────────────────────────────────────
// Node 子进程默认不继承 Windows 系统代理，必须通过 bash/sh 来调用 curl

function detectShell() {
  // 优先用 where bash 动态探测，fallback 到常见路径
  try {
    const p = execFileSync("where", ["bash"], { encoding: "utf-8" }).trim().split("\n")[0].trim();
    if (p) return p;
  } catch {}
  const candidates = [
    "D:/git/Git/usr/bin/bash.exe",
    "C:/Program Files/Git/usr/bin/bash.exe",
    "C:/Program Files/Git/bin/bash.exe",
  ];
  for (const c of candidates) {
    try { execFileSync(c, ["--version"], { timeout: 3000 }); return c; } catch {}
  }
  return true; // fallback：让 Node 用默认 shell（cmd.exe），可能代理走不了
}

const SHELL = detectShell();

function curlExec(args, opts = {}) {
  return execSync(`curl ${args}`, { shell: SHELL, encoding: "utf-8", timeout: 35000, ...opts });
}

/**
 * 让 curl 把响应体写入临时文件，再 readFileSync 读回来。
 * 彻底绕开 execSync pipe 的 ENOBUFS 缓冲区上限，适合大 JSON（如 litellm 3MB+）。
 * 返回文件内容字符串，调用方负责 JSON.parse。
 */
function curlToFile(url, opts = {}) {
  const tmp = join(tmpdir(), `opencode_curl_${Date.now()}_${Math.random().toString(36).slice(2)}.json`);
  try {
    execSync(`curl -s --max-time 30 -o "${tmp}" "${url}"`, {
      shell: SHELL,
      timeout: 35000,
      ...opts,
    });
    return readFileSync(tmp, "utf-8");
  } finally {
    try { unlinkSync(tmp); } catch {}
  }
}

// ── litellm 数据拉取（懒加载，全程只拉一次）─────────────────────────────────

/**
 * 拉取 litellm model_prices_and_context_window.json。
 * 用模型 id 做精确匹配，也支持去掉厂商前缀的模糊匹配：
 *   "claude-sonnet-4-6" 能匹配 "anthropic.claude-sonnet-4-6"
 *
 * 返回 { context, output, supportsVision } 或 null。
 */
async function queryLitellm(modelId) {
  // 懒加载
  if (_litellmData === null) {
    console.log(`  📡 正在从 litellm 拉取模型参数数据库...`);
    try {
      const raw = curlToFile(LITELLM_PRICES_URL);
      _litellmData = JSON.parse(raw);
      console.log(`  ✅ litellm 数据库加载完成（${Object.keys(_litellmData).length} 条记录）`);
    } catch (err) {
      console.warn(`  ⚠️  litellm 数据库拉取失败：${err.message?.slice(0, 80)}`);
      _litellmData = {}; // 标记已尝试，避免重复拉取
    }
  }

  if (!_litellmData || Object.keys(_litellmData).length === 0) return null;

  // 1. 精确匹配
  const exact = _litellmData[modelId];
  if (exact?.max_input_tokens) {
    return extractLitellmParams(exact, modelId, "exact");
  }

  // 2. 模糊匹配：在所有 key 里找包含 modelId 的条目（去掉前缀）
  //    例：modelId = "claude-sonnet-4-6" 匹配 "anthropic/claude-sonnet-4-6" 或 "claude-sonnet-4-6"
  const lowerModel = modelId.toLowerCase();
  const candidates = Object.entries(_litellmData).filter(([key]) => {
    const lk = key.toLowerCase();
    // 只要 key 的最后一段（按 / 或 . 分割）等于 modelId，或者 key 以 modelId 结尾
    return lk === lowerModel || lk.endsWith(`/${lowerModel}`) || lk.endsWith(`.${lowerModel}`);
  });

  if (candidates.length > 0) {
    // 优先取 mode === "chat" 的条目，否则取第一个
    const [key, entry] =
      candidates.find(([, v]) => v.mode === "chat") ?? candidates[0];
    if (entry.max_input_tokens) {
      return extractLitellmParams(entry, key, "fuzzy");
    }
  }

  return null;
}

function extractLitellmParams(entry, matchedKey, matchType) {
  const context = entry.max_input_tokens;
  const output  = entry.max_output_tokens ?? entry.max_tokens ?? context;
  // supports_vision → input modalities 包含 image
  const supportsVision = !!(entry.supports_vision);
  return { context, output, supportsVision, matchedKey, matchType };
}

// ── 内置默认参数（基于官方文档 + 已知信息）────────────────────────────────────
// 格式：{ context: number, output: number, input: string[] }

const MODEL_DEFAULTS = {
  // Claude 系列
  "claude-sonnet-4-6":           { context: 200000,  output: 64000,  input: ["text","image"] },
  "claude-sonnet-4-5-20250929":  { context: 200000,  output: 64000,  input: ["text","image"] },
  "claude-haiku-4-5-20251001":   { context: 200000,  output: 32000,  input: ["text","image"] },
  // Gemini 系列
  "gemini-2.5-pro":              { context: 1048576, output: 65536,  input: ["text","image"] },
  "gemini-2.5-flash":            { context: 1048576, output: 65536,  input: ["text","image"] },
  "gemini-3-pro-preview":        { context: 1048576, output: 65536,  input: ["text","image"] },
  "gemini-3-flash-preview":      { context: 1048576, output: 65536,  input: ["text","image"] },
  // GPT 系列（dodjoy 自定义路由）
  "gpt-5-codex":                 { context: 400000,  output: 128000, input: ["text","image"] },
  "gpt-5.2":                     { context: 400000,  output: 128000, input: ["text","image"] },
  "gpt-5.2-chat":                { context: 400000,  output: 128000, input: ["text","image"] },
  "gpt-5.2-codex":               { context: 400000,  output: 128000, input: ["text","image"] },
  "gpt-5.3-codex":               { context: 400000,  output: 128000, input: ["text","image"] },
  "gpt-5.4":                     { context: 1000000, output: 128000, input: ["text","image"] },
  // Kimi 系列（Moonshot AI）
  "kimi-k2.5":                   { context: 128000,  output: 32000,  input: ["text"] },
  "kimi-k2-thinking":            { context: 128000,  output: 32000,  input: ["text"] },
  // GLM 系列（智谱 AI）
  "glm-4.7":                     { context: 128000,  output: 32000,  input: ["text","image"] },
  "glm-5":                       { context: 128000,  output: 128000, input: ["text","image"] },
  // MiniMax 系列（官方文档：context = 204800）
  "MiniMax-M2.5":                { context: 204800,  output: 64000,  input: ["text"] },
  "MiniMax-M2.5-highspeed":      { context: 204800,  output: 64000,  input: ["text"] },
  "MiniMax-M2.1":                { context: 204800,  output: 64000,  input: ["text"] },
  "MiniMax-M2.1-highspeed":      { context: 204800,  output: 64000,  input: ["text"] },
};

// 兜底值（未知模型 + 查询失败时使用）
const FALLBACK = { context: 128000, output: 32000, input: ["text"] };

// ── 缓存读写 ──────────────────────────────────────────────────────────────────

function loadCache() {
  if (!existsSync(CACHE_PATH)) return {};
  try {
    return JSON.parse(readFileSync(CACHE_PATH, "utf-8"));
  } catch {
    return {};
  }
}

function saveCache(cache) {
  writeFileSync(CACHE_PATH, JSON.stringify(cache, null, 2), "utf-8");
}

// ── 联网查询 ──────────────────────────────────────────────────────────────────

/**
 * 通过 PowerShell 脚本查询指定模型参数（PowerShell 天然走 Windows 系统代理）。
 * 返回 { context: number, output: number, source: string } 或 null。
 */
async function queryModelLimits(modelId, apiKey) {
  const ps1 = join(SKILL_DIR, "scripts", "query_model.ps1");
  try {
    const raw = execSync(
      `powershell -ExecutionPolicy Bypass -File "${ps1}" -ModelId "${modelId}" -ApiKey "${apiKey}" -QueryModel "${QUERY_MODEL}"`,
      { shell: SHELL, encoding: "utf-8", timeout: 40000 }
    );

    const match = raw.trim().match(/\{[\s\S]*\}/);
    if (!match) {
      console.warn(`  ⚠️  无法解析查询结果：${raw.trim().slice(0, 100)}`);
      return null;
    }

    const parsed = JSON.parse(match[0]);
    const context = parseInt(parsed.context);
    const output  = parseInt(parsed.output);

    if (!Number.isFinite(context) || !Number.isFinite(output)) {
      console.warn(`  ⚠️  解析值无效：context=${parsed.context}, output=${parsed.output}`);
      return null;
    }

    return { context, output, source: parsed.source ?? "llm-query" };
  } catch (err) {
    // 查询失败：打印提示，引导主人手动补缓存
    console.warn(`  ⚠️  联网查询失败（${err.message?.slice(0, 80)}）`);
    console.warn(`  💡 可在系统终端手动查询后写入缓存：`);
    console.warn(`     node "${join(SKILL_DIR, "scripts", "add_to_cache.mjs")}" "${modelId}" <context> <output>`);
    return null;
  }
}

// ── 获取模型参数（内置 → 缓存 → 联网 → 兜底）────────────────────────────────

async function resolveModelParams(modelId, apiKey, cache) {
  // 1. 内置 defaults 优先
  if (MODEL_DEFAULTS[modelId]) {
    return { ...MODEL_DEFAULTS[modelId], _source: "builtin" };
  }

  // 2. 本地缓存
  if (cache[modelId]) {
    return { ...cache[modelId], _source: "cache" };
  }

  // 3. litellm 远端数据库
  console.log(`  🔍 未知模型 "${modelId}"，查询 litellm 数据库...`);
  const litellmResult = await queryLitellm(modelId);
  if (litellmResult) {
    const input = litellmResult.supportsVision ? ["text", "image"] : ["text"];
    const entry = {
      context:      litellmResult.context,
      output:       litellmResult.output,
      input,
      _querySource: `litellm(${litellmResult.matchType}:${litellmResult.matchedKey})`,
      _queriedAt:   new Date().toISOString(),
    };
    cache[modelId] = entry;
    saveCache(cache);
    console.log(
      `  ✅ litellm 命中（${litellmResult.matchType}，key="${litellmResult.matchedKey}"）：` +
      `context=${litellmResult.context}, output=${litellmResult.output}, vision=${litellmResult.supportsVision}`
    );
    console.log(`     已缓存至 ${CACHE_PATH}`);
    return { ...entry, _source: "litellm" };
  }

  // 4. LLM 联网查询（fallback）
  console.log(`  🌐 litellm 未收录，尝试 LLM 联网查询...`);
  const result = await queryModelLimits(modelId, apiKey);

  if (result) {
    const entry = {
      context:      result.context,
      output:       result.output,
      input:        ["text"],   // 联网查询保守设为 text-only
      _querySource: result.source,
      _queriedAt:   new Date().toISOString(),
    };
    cache[modelId] = entry;
    saveCache(cache);
    console.log(`  ✅ LLM 查询成功：context=${result.context}, output=${result.output}（${result.source}）`);
    console.log(`     已缓存至 ${CACHE_PATH}`);
    return { ...entry, _source: "network" };
  }

  // 5. 兜底
  console.warn(`  ⚠️  所有查询均失败，使用兜底值（context=128000, output=32000）`);
  return { ...FALLBACK, _source: "fallback" };
}

// ── 构建 config entry ─────────────────────────────────────────────────────────

/**
 * 构建写回 config 的模型条目。
 *
 * 策略：
 *   - 新增模型：完整写入 limit + modalities
 *   - 已有模型：只更新 limit.context / limit.output，其余字段（modalities、用户自定义字段）完全保留
 */
function buildModelEntry(params, existing) {
  if (!existing) {
    // 全新模型，完整写入
    const inputM  = params.input ?? ["text"];
    const outputM = ["text"];
    return {
      limit:      { context: params.context, output: params.output },
      modalities: { input: inputM, output: outputM },
    };
  }

  // 已有模型：深拷贝原始条目，只覆盖 limit
  const entry = JSON.parse(JSON.stringify(existing));
  entry.limit = { ...(entry.limit ?? {}), context: params.context, output: params.output };
  return entry;
}

// ── 工具 ──────────────────────────────────────────────────────────────────────

function printTable(title, items) {
  if (!items.length) return;
  console.log(`\n${title}`);
  items.forEach((m) => console.log(`  ${m}`));
}

// ── 主流程 ────────────────────────────────────────────────────────────────────

async function main() {
  // 1. 读取合并后的完整 config（通过 opencode debug config 获取，包含所有配置文件的合并结果）
  //    仅用于读取 apiKey 和 model，不用于写回
  let mergedConfig;
  try {
    const raw = execSync("opencode debug config", { encoding: "utf-8", timeout: 10000 });
    mergedConfig = JSON.parse(raw);
  } catch (err) {
    console.warn(`  ⚠️  opencode debug config 失败，降级读取本地 config.json：${err.message?.slice(0, 80)}`);
    mergedConfig = JSON.parse(readFileSync(CONFIG_PATH, "utf-8"));
  }

  // 从合并后的 config 读取 apiKey，fallback 到环境变量
  const apiKey = mergedConfig.provider?.dodjoy?.options?.apiKey || process.env.OPENAI_API_KEY;
  if (!apiKey || apiKey === "YOUR_API_KEY") {
    console.error("错误：未找到有效的 API Key。");
    console.error("请确认 opencode config 中已设置 provider.dodjoy.options.apiKey，");
    console.error("或设置环境变量：export OPENAI_API_KEY=<你的 dodjoy key>");
    process.exit(1);
  }

  // 2. 单独读取本地 config.json（写回目标，不含 opencode 内部默认值）
  let localConfig;
  try {
    localConfig = JSON.parse(readFileSync(CONFIG_PATH, "utf-8"));
  } catch (err) {
    console.error(`读取本地 config.json 失败：${err.message}`);
    process.exit(1);
  }

  // 3. 加载本地缓存
  const cache = loadCache();

  // 3. 拉取远端模型列表（用 curl 走系统代理）
  console.log(`正在从 ${DODJOY_BASE_URL}/v1/models 拉取模型列表...`);
  let remoteModels;
  try {
    const raw = curlExec(`-s --max-time 30 "${DODJOY_BASE_URL}/v1/models" -H "Authorization: Bearer ${apiKey}"`);
    const parsed = JSON.parse(raw);
    remoteModels = parsed.data || parsed;
    if (!Array.isArray(remoteModels)) {
      console.error(`API 返回格式异常，期望数组或 {data: [...]}，实际收到：${raw.slice(0, 200)}`);
      process.exit(1);
    }
  } catch (err) {
    console.error(`拉取模型列表失败：${err.message}`);
    process.exit(1);
  }
  const remoteIds = [...new Set(remoteModels.map((m) => m.id))];
  console.log(`拉取到 ${remoteIds.length} 个模型。`);

  // 4. 从本地硬盘 config.json 取 dodjoy 配置节点（读取用，确保拿到最新写入的内容）
  const providerCfg = localConfig.provider?.dodjoy;
  if (!providerCfg) {
    console.error("错误：config.json 中未找到 provider.dodjoy 节点。");
    process.exit(1);
  }
  const existingModels = providerCfg.models ?? {};
  const existingIds = new Set(Object.keys(existingModels));

  // 从合并后的 config.model 中取默认模型（格式 "dodjoy/xxx" → 取 xxx 部分）
  const configModel = mergedConfig.model ?? "";
  QUERY_MODEL = configModel.includes("/") ? configModel.split("/").slice(1).join("/") : configModel;
  // 如果没配置或解析失败，fallback 到远端第一个模型
  if (!QUERY_MODEL) QUERY_MODEL = remoteIds[0];
  console.log(`联网查询使用模型：${QUERY_MODEL}`);
  const remoteIdSet = new Set(remoteIds);

  // 5. 计算差异
  const added   = remoteIds.filter((id) => !existingIds.has(id));
  const removed = [...existingIds].filter((id) => !remoteIdSet.has(id));
  const kept    = remoteIds.filter((id) => existingIds.has(id));

  printTable("新增模型：", added);
  printTable("已移除模型（下线）：", removed);
  printTable("保留模型：", kept);

  // 6. 构建新 models 对象，新模型触发参数查询
  if (added.length > 0) {
    console.log("\n正在解析新模型参数...");
  }
  const newModels = {};
  const sourceStats = { builtin: 0, cache: 0, litellm: 0, network: 0, fallback: 0 };
  for (const id of remoteIds) {
    const params = await resolveModelParams(id, apiKey, cache);
    newModels[id] = buildModelEntry(params, existingModels[id]);
    sourceStats[params._source] = (sourceStats[params._source] ?? 0) + 1;
  }

  // 输出参数来源统计
  const statsLine = Object.entries(sourceStats)
    .filter(([, v]) => v > 0)
    .map(([k, v]) => `${k}:${v}`)
    .join(", ");
  console.log(`\n参数来源统计：${statsLine}`);

  // 7. 写回 config：只修改本地 config.json 的 provider.dodjoy.models，其他字段一律不动
  if (!localConfig.provider) localConfig.provider = {};
  if (!localConfig.provider.dodjoy) localConfig.provider.dodjoy = {};
  localConfig.provider.dodjoy.models = newModels;
  writeFileSync(CONFIG_PATH, JSON.stringify(localConfig, null, 4), "utf-8");
  console.log(`\n✅ config.json 已更新（+${added.length} / -${removed.length}）。`);
  console.log(`   路径：${CONFIG_PATH}`);
}

main().catch((err) => {
  console.error("未预期的错误：", err);
  process.exit(1);
});
