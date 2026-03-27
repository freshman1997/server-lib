# FTP 架构重构方案

## 问题根源分析

### 当前架构的核心问题

根据代码分析和工具反馈，当前 FTP 实现存在以下严重架构问题：

#### 1. **控制连接和数据连接混淆**

**问题描述：**
- `FtpSession::on_close()` 在接收到 `Connection*` 参数时，无法区分是控制连接还是数据连接的关闭
- 当前使用 `close_` 标志来判断是否应该删除 session，但这会导致**控制连接意外关闭时无法清理 session**

**证据（session.cpp:77-86）：**
```cpp
void FtpSession::on_close(Connection *conn)
{
    (void)conn;
    if (!close_ || keep_util_sent_) {
        return;  // Don't destroy session on data connection close
    }
    delete this;
}
```

**问题场景：**
1. 客户端执行 `upload` 命令
2. PASV 建立数据连接，创建 `FtpFileStreamSession`
3. 数据传输完成后，数据连接关闭
4. `FtpFileStreamSession::on_close()` → `quit()` → `session_->on_closed(this)`
5. **错误地触发 `FtpSession::on_close()`**，导致控制 session 被删除

#### 2. **循环引用和内存泄漏**

**循环引用链：**
```
FtpSessionManager → FtpSession → Connection → FtpSession
                       ↓
                 FtpFileStream → FtpFileStreamSession
```

**问题点：**
- `FtpSessionManager::clear()` 只清空 map，不删除对象（内存泄漏）
- `Connection` 和 `FtpSession` 相互持有原始指针，无明确所有权
- 多处 `delete this` 危险模式，容易导致悬垂指针

#### 3. **事件循环退出逻辑错误**

**服务器端问题（ftp_server.cpp:150-169）：**
```cpp
void FtpServer::quit()
{
    impl_->closing_ = true;
    // ... 清理所有 session
    impl_->ev_loop_->quit();  // 退出事件循环
}
```

**问题：** 事件循环退出后，整个服务器停止，但可能还有活跃的传输连接。

**客户端问题（ftp_client.cpp:41-49）：**
```cpp
void FtpClient::quit()
{
    if (session_) {
        session_->quit();  // 可能调用 delete this
    }
    if (ev_loop_) {
        ev_loop_->quit();  // 删除后再调用 quit() 是危险的
    }
}
```

**问题：** `session_->quit()` 可能已经删除了 session，但后续代码仍然访问。

#### 4. **Session 生命周期不清晰**

**当前实现：**
- `FtpSession` 既管理控制连接，又通过 `file_stream_` 管理数据连接
- `FtpFileStreamSession` 继承自 `ConnectionHandler`，在关闭时通知 `FtpSession`
- 但 `FtpSession::on_close()` 被调用时，不知道是控制连接还是数据连接关闭

**结果：**
- 数据连接关闭时，误认为控制连接关闭，导致整个 session 被删除
- 用户上传文件后，控制连接被意外关闭，导致双端退出

## 架构重构方案

### 核心原则

1. **明确区分控制连接和数据连接**
2. **建立清晰的对象所有权关系**
3. **使用智能指针替代原始指针**
4. **消除 `delete this` 危险模式**

### 重构架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                      FtpServer                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │  FtpSessionManager (ownership of all sessions)       ││
│  │  ┌───────────────────────────────────────────────────┐││
│  │  │  Connection* → std::shared_ptr<ServerFtpSession>│││
│  │  └───────────────────────────────────────────────────┘││
│  └─────────────────────────────────────────────────────────┘│
│  ┌─────────────────────────────────────────────────────────┐│
│  │  EventLoop (singleton for the server)                ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
                            ↓
                    on_connected(new Connection)
                            ↓
┌─────────────────────────────────────────────────────────────┐
│                 ServerFtpSession                          │
│  ┌─────────────────────────────────────────────────────────┐│
│  │  ControlConnection (unique ownership)                  ││
│  └─────────────────────────────────────────────────────────┘│
│  ┌─────────────────────────────────────────────────────────┐│
│  │  std::shared_ptr<ServerFtpFileStream>                 ││
│  │  (lazy created when PASV is issued)                   ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
                            ↓
                  handle_commands_from_control
                            ↓
                 PASV command received
                            ↓
┌─────────────────────────────────────────────────────────────┐
│              ServerFtpFileStream                           │
│  ┌─────────────────────────────────────────────────────────┐│
│  │  Acceptor (listens on random port)                   ││
│  └─────────────────────────────────────────────────────────┘│
│  ┌─────────────────────────────────────────────────────────┐│
│  │  std::unordered_map<Connection*,                       ││
│  │      std::shared_ptr<FtpFileStreamSession>>           ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
                            ↓
              new data connection accepted
                            ↓
┌─────────────────────────────────────────────────────────────┐
│           FtpFileStreamSession                             │
│  ┌─────────────────────────────────────────────────────────┐│
│  │  DataConnection (unique ownership)                     ││
│  └─────────────────────────────────────────────────────────┘│
│  ┌─────────────────────────────────────────────────────────┐│
│  │  weak_ptr<ServerFtpSession> (避免循环引用)            ││
│  └─────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────┘
```

### 关键改进点

#### 1. **移除 `FtpSession::on_close(Connection* conn)`**

**原因：** 无法区分控制连接和数据连接

**改为：**
```cpp
// FtpSession 只作为控制连接的 handler
class ServerFtpSession : public ConnectionHandler {
public:
    void on_connected(Connection* conn) override;  // 控制连接建立
    void on_read(Connection* conn) override;     // 控制命令处理
    void on_error(Connection* conn) override;    // 控制连接错误
    void on_close(Connection* conn) override;    // 只处理控制连接关闭

private:
    std::shared_ptr<ServerFtpFileStream> file_stream_;
};
```

**FtpFileStreamSession 单独管理：**
```cpp
class FtpFileStreamSession : public ConnectionHandler {
public:
    void on_close(Connection* conn) override {
        // 数据连接关闭，只销毁数据 session，不影响控制 session
        weak_ptr<ServerFtpSession> session_weak = session_;
        if (auto session = session_weak.lock()) {
            session->on_data_connection_closed(this);
        }
    }

private:
    std::weak_ptr<ServerFtpSession> session_;  // 避免循环引用
};
```

#### 2. **使用智能指针管理所有权**

**FtpSessionManager 改为拥有 session：**
```cpp
class FtpSessionManager {
public:
    void add_session(Connection* conn, std::shared_ptr<FtpSession> session) {
        sessions_[conn] = session;
    }

    void remove_session(Connection* conn) {
        sessions_.erase(conn);  // shared_ptr 自动减少引用计数
    }

    void clear() {
        sessions_.clear();  // 自动删除所有 session
    }

private:
    std::unordered_map<Connection*, std::shared_ptr<FtpSession>> sessions_;
};
```

**FtpServer::quit() 改为智能指针：**
```cpp
void FtpServer::quit() {
    impl_->closing_ = true;

    // 自动删除所有 session（通过 shared_ptr）
    impl_->session_manager_.clear();

    // 最后退出事件循环
    if (impl_->ev_loop_) {
        impl_->ev_loop_->quit();
    }
}
```

#### 3. **移除 `delete this` 模式**

**改为显式删除：**
```cpp
class ServerFtpSession {
public:
    void quit() {
        // 通知 session manager 移除自己
        if (auto* app = get_app()) {
            app->on_session_closed(this);
        }

        // 关闭控制连接
        if (context_.conn_) {
            context_.conn_->close();
        }
    }
};
```

**FtpSessionManager 真正拥有 session：**
```cpp
void FtpServer::on_session_closed(FtpSession* session) {
    if (!session || impl_->closing_) {
        return;
    }

    // 从 manager 中移除，shared_ptr 自动删除
    impl_->session_manager_.remove_session(session->get_connection());
}
```

#### 4. **控制连接和数据连接完全分离**

**ServerFtpSession 只处理控制连接：**
```cpp
class ServerFtpSession : public ConnectionHandler {
public:
    void on_read(Connection* conn) override {
        // 解析 FTP 命令
        auto cmd = parse_command(conn->get_input_buff());

        // 处理命令
        switch (cmd.type()) {
            case CommandType::PASV:
                // 创建数据流监听器
                if (!file_stream_) {
                    file_stream_ = std::make_shared<ServerFtpFileStream>(this);
                    file_stream_->start(pasv_address);
                }
                send_pasv_response(file_stream_->get_listen_address());
                break;

            case CommandType::STOR:
                // 准备接收文件
                if (file_stream_) {
                    file_stream_->set_work_file(file_info, client_ip);
                }
                break;

            case CommandType::QUIT:
                // 关闭控制连接
                quit();
                break;
        }
    }

    void on_close(Connection* conn) override {
        // 控制连接关闭，整个 session 结束
        std::cout << "Control connection closed, session terminated\n";
        // 通知 manager 移除自己
        get_app()->on_session_closed(this);
    }

private:
    std::shared_ptr<ServerFtpFileStream> file_stream_;
};
```

**ServerFtpFileStream 只处理数据连接：**
```cpp
class ServerFtpFileStream : public ConnectionHandler {
public:
    void on_connected(Connection* conn) override {
        // 新的数据连接建立
        auto data_session = std::make_shared<FtpFileStreamSession>(session_weak_);
        data_sessions_[conn] = data_session;
        conn->set_connection_handler(data_session.get());
        data_session->on_connected(conn);
    }

    void remove_session(FtpFileStreamSession* fs) {
        // 数据连接关闭，从 map 中移除
        for (auto it = data_sessions_.begin(); it != data_sessions_.end(); ++it) {
            if (it->second.get() == fs) {
                data_sessions_.erase(it);
                break;
            }
        }

        // 注意：这里不删除自己，因为可能还有其他数据连接

        // 如果没有更多数据连接，通知控制 session
        if (data_sessions_.empty()) {
            if (auto session = session_weak_.lock()) {
                session->on_data_connections_empty();
            }
        }
    }

private:
    std::weak_ptr<ServerFtpSession> session_weak_;
    std::unordered_map<Connection*, std::shared_ptr<FtpFileStreamSession>> data_sessions_;
};
```

## 具体修复步骤

### 第一步：修改 FtpSession 和 FtpSessionManager

1. **修改 session_manager.h**
```cpp
#ifndef __NET_FTP_SERVER_SESSION_MANAGER_H__
#define __NET_FTP_SERVER_SESSION_MANAGER_H__

#include <unordered_map>
#include <memory>

namespace yuan::net { class Connection; }
namespace yuan::net::ftp { class FtpSession; }

namespace yuan::net::ftp
{
    class FtpSessionManager
    {
    public:
        void add_session(Connection *conn, std::shared_ptr<FtpSession> session)
        {
            sessions_[conn] = session;
        }

        std::shared_ptr<FtpSession> get_session(Connection *conn)
        {
            auto it = sessions_.find(conn);
            return it != sessions_.end() ? it->second : nullptr;
        }

        void remove_session(Connection *conn)
        {
            sessions_.erase(conn);  // shared_ptr 自动管理生命周期
        }

        const std::unordered_map<Connection *, std::shared_ptr<FtpSession>> & get_sessions()
        {
            return sessions_;
        }

        void clear()
        {
            sessions_.clear();  // 自动删除所有 session
        }

    private:
        std::unordered_map<Connection *, std::shared_ptr<FtpSession>> sessions_;
    };
}

#endif
```

2. **修改 session.h，移除危险标志**
```cpp
// 移除 close_ 和 keep_util_sent_ 标志
// 改为更清晰的方法调用

class FtpSession : public ConnectionHandler, public FtpFileStreamEvent
{
public:
    // 控制连接回调
    virtual void on_connected(Connection *conn) override;
    virtual void on_error(Connection *conn) override;
    virtual void on_read(Connection *conn) = 0;
    virtual void on_write(Connection *conn) override;

    // 控制连接关闭时调用（只处理控制连接）
    virtual void on_close(Connection *conn) override;

    // 数据连接相关回调（新增）
    virtual void on_data_connection_opened(FtpFileStreamSession *fs);
    virtual void on_data_connection_error(FtpFileStreamSession *fs);
    virtual void on_data_transfer_completed(FtpFileStreamSession *fs);
    virtual void on_data_connection_closed(FtpFileStreamSession *fs);
    virtual void on_data_connections_empty();  // 所有数据连接都关闭

protected:
    // 移除 close_ 和 keep_util_sent_
    // 改为使用 weak_ptr 来避免循环引用
    std::weak_ptr<FtpApp> app_weak_;  // 改为 weak_ptr
    FtpSessionContext context_;
};
```

### 第二步：修改 FtpFileStream 和 FtpFileStreamSession

1. **使用 weak_ptr 避免循环引用**
```cpp
class FtpFileStreamSession : public ConnectionHandler
{
public:
    FtpFileStreamSession(std::weak_ptr<FtpSession> session)
        : session_weak_(session)
    {
        // ...
    }

    void on_close(Connection *conn) override
    {
        // 数据连接关闭
        if (auto session = session_weak_.lock()) {
            session->on_data_connection_closed(this);
        }
    }

private:
    std::weak_ptr<FtpSession> session_weak_;  // weak_ptr 避免循环引用
    std::unique_ptr<Connection> conn_;        // 独占数据连接
    FileSteamState state_;
    FtpFileInfo *current_file_info_;
};
```

2. **FtpFileStream 使用 shared_ptr 管理 sessions**
```cpp
class FtpFileStream : public ConnectionHandler
{
public:
    FtpFileStream(std::weak_ptr<FtpSession> session)
        : session_weak_(session)
    {
    }

    void on_connected(Connection *conn) override
    {
        auto data_session = std::make_shared<FtpFileStreamSession>(session_weak_);
        data_session->on_connected(conn);
        data_sessions_[conn] = data_session;
    }

    void remove_session(FtpFileStreamSession *fs)
    {
        for (auto it = data_sessions_.begin(); it != data_sessions_.end(); ++it) {
            if (it->second.get() == fs) {
                data_sessions_.erase(it);
                break;
            }
        }

        if (data_sessions_.empty()) {
            if (auto session = session_weak_.lock()) {
                session->on_data_connections_empty();
            }
        }
    }

private:
    std::weak_ptr<FtpSession> session_weak_;
    std::unordered_map<Connection*, std::shared_ptr<FtpFileStreamSession>> data_sessions_;
};
```

### 第三步：修改 FtpServer 的 session 创建逻辑

**ftp_server.cpp**
```cpp
void FtpServer::on_connected(Connection *conn)
{
    auto session = impl_->session_manager_.get_session(conn);
    if (session) {
        std::cout << "session already exists for this connection!\n";
        conn->close();
        return;
    }

    // 使用 shared_ptr 创建 session
    auto newSession = std::make_shared<ServerFtpSession>(conn, this);
    impl_->session_manager_.add_session(conn, newSession);
    newSession->on_connected(conn);
}
```

**quit() 方法**
```cpp
void FtpServer::quit()
{
    if (impl_->closing_) {
        return;
    }

    impl_->closing_ = true;

    // 自动清理所有 session（shared_ptr 自动删除）
    impl_->session_manager_.clear();

    impl_->timer_manager_ = nullptr;

    if (impl_->ev_loop_) {
        impl_->ev_loop_->quit();
        impl_->ev_loop_ = nullptr;
    }
}
```

### 第四步：修改数据传输完成后的行为

**FtpFileStreamSession 完成传输后**
```cpp
void FtpFileStreamSession::on_read(Connection *conn)
{
    // ... 处理数据 ...

    if (current_file_info_->is_completed()) {
        state_ = FileSteamState::idle;
        current_file_info_->ready_ = false;

        // 通知控制 session 传输完成
        if (auto session = session_weak_.lock()) {
            session->on_data_transfer_completed(this);
        }

        current_file_info_ = nullptr;
        conn_->close();  // 关闭数据连接，不影响控制连接
    }
}
```

**ServerFtpSession 处理传输完成**
```cpp
void ServerFtpSession::on_data_transfer_completed(FtpFileStreamSession *fs)
{
    (void)fs;
    // 发送传输完成响应
    send_command("226 Transfer complete.\r\n");

    // 注意：这里不删除自己，控制连接继续保持
    std::cout << "Data transfer completed, control connection remains\n";
}

void ServerFtpSession::on_data_connections_empty()
{
    // 所有数据连接都关闭了，但控制连接继续保持
    file_stream_.reset();  // 释放 FtpFileStream 对象
    std::cout << "All data connections closed, control connection still active\n";
}
```

## 测试验证

### 测试用例

1. **基本上传测试**
```
- 连接到服务器
- 登录
- 执行 PASV
- 上传文件
- 验证：文件上传成功，控制连接仍然保持
```

2. **多次上传测试**
```
- 登录后连续执行多次上传
- 验证：每次上传后控制连接都保持
```

3. **网络中断测试**
```
- 上传过程中断开网络
- 验证：连接关闭后 session 被正确清理，无内存泄漏
```

4. **并发传输测试**
```
- 使用多个客户端同时上传
- 验证：互不影响，无竞态条件
```

## 预期效果

### 修复前
```
客户端: login -> upload E:/utils/script.sh run.sh
服务器: [上传成功]
结果: 双端都退出 ❌
```

### 修复后
```
客户端: login -> upload E:/utils/script.sh run.sh
服务器: [上传成功] [发送 226 Transfer complete]
结果: 控制连接保持，可以继续操作 ✅
客户端: upload E:/utils/another.txt another.txt
服务器: [上传成功] [发送 226 Transfer complete]
结果: 继续保持，可以执行更多操作 ✅
```

## 总结

这个重构方案的核心思想：

1. **完全分离控制连接和数据连接**：使用不同的回调方法处理
2. **使用智能指针管理所有权**：消除循环引用和内存泄漏
3. **移除 `delete this` 模式**：使用显式的生命周期管理
4. **清晰的职责分离**：每个类只管理自己的资源

这样修改后，数据连接的关闭不会影响控制连接，用户可以在上传文件后继续使用 FTP 服务。
