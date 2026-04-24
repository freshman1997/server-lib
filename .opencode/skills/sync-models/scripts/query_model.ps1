# query_model.ps1
# 用法：powershell -File query_model.ps1 -ModelId <id> -ApiKey <key> -QueryModel <model>
# 通过 dodjoy LLM API 查询指定模型的 context/output 参数（走 Windows 系统代理）
# 输出：标准 JSON 字符串

param(
    [Parameter(Mandatory)][string]$ModelId,
    [Parameter(Mandatory)][string]$ApiKey,
    [Parameter(Mandatory)][string]$QueryModel
)

$DODJOY_BASE_URL = "https://airouter.dodjoy.com"

$Prompt = "I need accurate technical specifications for the AI model `"$ModelId`".`nPlease return ONLY a JSON object: {`"context`":<context window tokens, integer>,`"output`":<max output tokens, integer>,`"source`":`"<official docs / estimated>`"}`nNo markdown, no explanation, ONLY the JSON."

$Body = @{
    model       = $QueryModel
    messages    = @(@{ role = "user"; content = $Prompt })
    temperature = 0
    max_tokens  = 200
} | ConvertTo-Json -Depth 5 -Compress

try {
    $Response = Invoke-RestMethod `
        -Uri "$DODJOY_BASE_URL/v1/chat/completions" `
        -Method POST `
        -Headers @{ Authorization = "Bearer $ApiKey"; "Content-Type" = "application/json" } `
        -Body $Body `
        -TimeoutSec 30 `
        -UseBasicParsing

    $Text = $Response.choices[0].message.content.Trim()
    # 提取 JSON 部分（防止模型前后加了多余文字）
    if ($Text -match '\{[\s\S]*\}') {
        Write-Output $Matches[0]
    } else {
        Write-Error "PARSE_FAIL: $Text"
        exit 1
    }
} catch {
    Write-Error "REQUEST_FAIL: $_"
    exit 2
}
