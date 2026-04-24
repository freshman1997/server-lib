#!/usr/bin/env node
/**
 * add_to_cache.mjs
 * 手动将模型参数写入本地缓存，供 sync_models.mjs 下次同步时读取。
 *
 * 用法：
 *   node add_to_cache.mjs <model_id> <context> <output> [input_modalities]
 *
 * 示例：
 *   node add_to_cache.mjs gpt-5.4-codex 400000 128000
 *   node add_to_cache.mjs some-vision-model 200000 64000 text,image
 */

import { readFileSync, writeFileSync, existsSync } from "fs";
import { join, dirname } from "path";
import { fileURLToPath } from "url";

const __dir     = dirname(fileURLToPath(import.meta.url));
const CACHE_PATH = join(__dir, "..", "model_cache.json");

const [,, modelId, contextStr, outputStr, inputStr] = process.argv;

if (!modelId || !contextStr || !outputStr) {
  console.error("用法：node add_to_cache.mjs <model_id> <context> <output> [input_modalities]");
  console.error("示例：node add_to_cache.mjs gpt-5.4-codex 400000 128000");
  console.error("      node add_to_cache.mjs vision-model 200000 64000 text,image");
  process.exit(1);
}

const context = parseInt(contextStr);
const output  = parseInt(outputStr);
if (!Number.isFinite(context) || !Number.isFinite(output)) {
  console.error(`参数错误：context 和 output 必须是整数。`);
  process.exit(1);
}

const input = inputStr ? inputStr.split(",").map(s => s.trim()) : ["text"];

const cache = existsSync(CACHE_PATH)
  ? JSON.parse(readFileSync(CACHE_PATH, "utf-8"))
  : {};

cache[modelId] = {
  context,
  output,
  input,
  _querySource: "manual",
  _queriedAt: new Date().toISOString(),
};

writeFileSync(CACHE_PATH, JSON.stringify(cache, null, 2), "utf-8");
console.log(`✅ 已写入缓存：${modelId} → context=${context}, output=${output}, input=[${input}]`);
console.log(`   缓存路径：${CACHE_PATH}`);
