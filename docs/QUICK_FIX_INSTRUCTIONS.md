# FTP 双端退出问题 - 快速修复指南

## 问题现象
执行 `upload E:/utils/script.sh run.sh` 后，双端都退出。

## 根本原因
服务器端的 `FtpSession::on_close(Connection *conn)` 无法区分是控制连接关闭还是数据连接关闭，导致：
1. 数据传输完成后，数据连接关闭
2. 触发 `on_close()` 被调用
3. 错误地认为控制连接关闭
4. 删除了整个 session
5. 控制连接也被关闭
6. 客户端检测到控制连接关闭，也退出

## 快速修复方案

### 方案一：修改 `on_close` 逻辑（推荐）

**修改文件：** `protocol/ftp/src/common/session.cpp`

**找到 `on_close` 方法（大约在第 76-86 行）：**

```cpp
void FtpSession::on_close(Connection *conn)
{
    (void)conn;
    // Only delete session if it was explicitly quit, not after data transfer completion
    // The data transfer connection closing should NOT destroy the control session
    if (!close_ || keep_util_sent_) {
        return;  // Don't destroy session on data connection close
    }
    delete this;
}
```

**改为：**

```cpp
void FtpSession::on_close(Connection *conn)
{
    (void)conn;

    // 关键修复：检查是否是控制连接
    // 只有当 conn 是控制连接时，才删除 session
    if (conn == context_.conn_) {
        // 这是控制连接关闭，需要删除 session
        std::cout << "Control connection closed, terminating session\n";

        // 通知应用移除自己
        if (context_.app_) {
            context_.app_->on_session_closed(this);
        }

        // 清理文件流（如果存在）
        context_.file_stream_ = nullptr;

        // 不再使用 delete this
        // 改为让应用层管理生命周期
    } else {
        // 这是数据连接关闭，不影响控制 session
        std::cout << "Data connection closed (ignoring)\n";
    }
}
```

### 方案二：使用 shared_ptr 管理生命周期（完整方案）

需要修改多个文件，详见 `FTP_ARCHITECTURE_REFACTOR.md`。

## 验证修复

### 测试步骤

1. 编译修改后的代码
```bash
cd build
cmake --build . --target ftp_server_with_client ftp_interactive_improved
```

2. 启动服务器
```bash
./ftp_server_with_client
```

3. 启动客户端
```bash
./ftp_interactive_improved
```

4. 执行上传操作
```
ftp> connect 127.0.0.1 12123
ftp-connected> login tester secret
ftp> upload E:/utils/script.sh run.sh
```

### 预期结果

**修复前：**
```
Uploading E:/utils/script.sh to run.sh...
ftp session closed!  <- session 被删除
Control connection closed, terminating session
[客户端和服务器都退出]
```

**修复后：**
```
Uploading E:/utils/script.sh to run.sh...
Upload complete!
Data connection closed (ignoring)  <- 数据连接关闭，但 session 还在
ftp>  <- 提示符还在，可以继续操作
```

## 附加说明

### 为什么需要 `conn == context_.conn_` 的判断？

- `context_.conn_` 存储的是控制连接的指针
- 数据连接是由 `FtpFileStream` 和 `FtpFileStreamSession` 管理的
- 当数据连接关闭时，`conn` 不等于 `context_.conn_`
- 通过这个判断，我们可以区分两种情况

### 这个修复的局限性

1. **内存管理不清晰**：仍然使用原始指针，容易产生内存泄漏
2. **循环引用**：`FtpSession` ↔ `Connection` ↔ `FtpSession` 仍然存在
3. **delete this 危险**：部分代码仍然使用 `delete this`

### 完整解决方案

要彻底解决架构问题，请参考 `FTP_ARCHITECTURE_REFACTOR.md` 中的完整重构方案。

## 紧急回滚

如果修复后出现问题，可以使用以下命令回滚：
```bash
git checkout protocol/ftp/src/common/session.cpp
```

## 修改文件清单

本次快速修复仅修改：
- `protocol/ftp/src/common/session.cpp` - 修改 `on_close` 方法

## 测试建议

1. **基本功能测试**：
   - 登录
   - 上传单个文件
   - 上传多个文件
   - 下载文件

2. **异常情况测试**：
   - 上传过程中断网
   - 服务器异常关闭
   - 客户端异常关闭

3. **并发测试**：
   - 多个客户端同时上传
   - 同一个客户端连续上传多个文件

## 联系方式

如果问题仍然存在，请检查：
1. 是否正确识别了 `on_close` 的调用者
2. `context_.conn_` 是否在正确的时机被设置
3. `context_.app_` 是否有效

可以添加调试输出：
```cpp
void FtpSession::on_close(Connection *conn)
{
    std::cout << "on_close called, conn=" << conn
              << ", control_conn=" << context_.conn_
              << ", is_control=" << (conn == context_.conn_) << "\n";

    if (conn == context_.conn_) {
        // ...
    } else {
        std::cout << "Ignoring data connection close\n";
    }
}
```
