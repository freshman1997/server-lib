#!/usr/bin/env bash
# query_model.sh
# 用法：bash query_model.sh <model_id> <api_key> <query_model>
# 通过 dodjoy LLM API 查询指定模型的 context/output 参数
# 输出：JSON 字符串，格式 {"context":N,"output":N,"source":"..."}

MODEL_ID="$1"
API_KEY="$2"
QUERY_MODEL="$3"
DODJOY_BASE_URL="https://airouter.dodjoy.com"

PROMPT="I need accurate technical specifications for the AI model \"${MODEL_ID}\".
Please return ONLY a JSON object: {\"context\":<context window tokens, integer>,\"output\":<max output tokens, integer>,\"source\":\"<official docs / estimated>\"}
No markdown, no explanation, ONLY the JSON."

BODY=$(printf '{"model":"%s","messages":[{"role":"user","content":"%s"}],"temperature":0,"max_tokens":200}' \
  "$QUERY_MODEL" \
  "$(echo "$PROMPT" | python3 -c 'import json,sys; print(json.dumps(sys.stdin.read())[1:-1])')")

curl -s --max-time 30 \
  -X POST "${DODJOY_BASE_URL}/v1/chat/completions" \
  -H "Authorization: Bearer ${API_KEY}" \
  -H "Content-Type: application/json" \
  -d "{\"model\":\"${QUERY_MODEL}\",\"messages\":[{\"role\":\"user\",\"content\":$(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "$PROMPT")}],\"temperature\":0,\"max_tokens\":200}"
