# Proxy Tool Guide

Source files live in `test/proxy/` and the executable is built to `cmake-build-mingw/test/proxy/proxy_tool.exe`.

This folder now contains a small SOCKS5 toolkit:

- `proxy_tool serve [port]`
- `proxy_tool udp-echo [port]`
- `proxy_tool udp-probe [proxy_host] [proxy_port] [target_host] [target_port] [payload]`

## Build

Build the tool from the `cmake-build-mingw` directory:

```powershell
cmake --build . --target proxy_tool -j 4
```

## VS Code

If you open the repository in VS Code, use these flows:

- `Terminal -> Run Task -> build proxy_tool` to compile the proxy helper
- `Run and Debug -> Run Proxy Tool` to start the SOCKS5 server on `1080`
- `Run and Debug -> Run UDP Echo` to start the local UDP echo server
- `Terminal -> Run Task -> udp probe` to validate UDP relay after both are running

Recommended order:

1. Build `proxy_tool`
2. Start `Run Proxy Tool`
3. Start `Run UDP Echo`
4. Run `udp probe`

## Start the SOCKS5 proxy

```powershell
.\test\proxy\proxy_tool.exe serve 1080
```

Defaults:

- `CONNECT` enabled
- `UDP ASSOCIATE` enabled
- authentication disabled

## Verify TCP proxying

Use curl through the SOCKS5 proxy:

```powershell
curl.exe --socks5-hostname 127.0.0.1:1080 -I https://example.com
```

## Verify UDP proxying locally

Start a local UDP echo server:

```powershell
.\test\proxy\proxy_tool.exe udp-echo 19090
```

Then send a UDP packet through the SOCKS5 relay:

```powershell
.\test\proxy\proxy_tool.exe udp-probe 127.0.0.1 1080 127.0.0.1 19090 PING-UDP
```

If everything is working, the probe prints `UDP_OK`.

## Windows system proxy

Set the proxy in:

`Settings` -> `Network & Internet` -> `Proxy`

Use:

- `Address`: `127.0.0.1`
- `Port`: `1080`

Notes:

- Windows system proxy settings are usually HTTP/HTTPS-oriented.
- For SOCKS5-specific apps, configure the app directly if it supports SOCKS5.

## Browser setup

### Firefox

Firefox supports SOCKS5 directly:

- Open `Settings`
- Search for `Proxy`
- Open `Network Settings`
- Choose `Manual proxy configuration`
- Set `SOCKS Host` to `127.0.0.1`
- Set `Port` to `1080`
- Select `SOCKS v5`
- Enable remote DNS if available

### Chrome / Edge

Chrome and Edge usually follow the system proxy on Windows.

If you want a direct launch flag, use:

```powershell
chrome.exe --proxy-server="socks5://127.0.0.1:1080"
```

For Edge, replace `chrome.exe` with `msedge.exe`.

## Recommended quick check

1. Start `proxy_tool serve 1080`
2. Run the curl TCP check
3. Run `proxy_tool udp-echo 19090`
4. Run `proxy_tool udp-probe 127.0.0.1 1080 127.0.0.1 19090 PING-UDP`

## VS Code notes

- Keep the repository root as the workspace folder so relative paths in tasks resolve correctly.
- If VS Code asks for a build directory, point CMake to `cmake-build-mingw`.
- `curl.exe` is used directly in the terminal because the PowerShell `curl` alias is not the real curl binary.
- If port `1080` is busy, change the launch argument in VS Code or use a different port.
