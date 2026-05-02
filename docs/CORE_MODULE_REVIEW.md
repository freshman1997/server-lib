# Core 模块设计与风险扫描

日期：2026-05-02

## 范围

本次主要扫描 `core/base`、`core/core`、`core/app`，重点放在网络运行时、连接生命周期、协程 awaitable、事件循环、timer、模块组织。协议层未展开，只把它们作为 core API 的使用方来判断风险。

整体判断：`core` 已经具备可跑的 reactor + coroutine 雏形，但现在几个关键抽象仍混在一起，尤其是 `ConnectionHandler` 同时承担业务回调和协程唤醒，`ConnectionRef` 同时表达拥有和不拥有连接。这会让代码在单层 demo 里能工作，但一旦协议栈、代理、隧道、TLS、关闭流程叠起来，就容易出现悬挂指针、事件丢失、handler 被覆盖、关闭时丢数据等问题。

个人项目还未稳定，建议趁现在做一次偏破坏式的 API 收束。

## 最高优先级问题

### 1. `ConnectionRef` 的语义不够清晰，裸指针 await 路径存在生命周期风险

位置：

- [connection_ref.h](/home/yuan/codes/test/webserver/core/core/include/net/connection/connection_ref.h:16)
- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:22)
- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:315)
- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:971)
- [network_runtime.h](/home/yuan/codes/test/webserver/core/core/include/net/runtime/network_runtime.h:158)

现状：

- `ConnectionRef(Connection*)` 只保存裸指针。
- `ConnectionRef(std::shared_ptr<Connection>)` 保存 owner，再缓存裸指针。
- awaiter、runtime view 同时暴露 `Connection*` 和 `shared_ptr<Connection>` 重载。

风险：

- `ConnectionRef` 看起来像安全引用，但裸指针构造并不延长生命周期。
- awaiter 会跨挂起点保存 `ConnectionRef`，裸指针路径下，连接可能已被 `EventLoop::connections_` 移除并析构。
- `ConnectionRef::operator->` 和 `operator*` 不做空检查，调用者容易把它当成总是有效的 handle。
- `owner()` 每次复制 `shared_ptr`，但裸指针模式下没有任何 owner 可拿，API 语义不对称。

建议方向：

1. 把“长期跨挂起点使用的连接”统一改成 owning handle。
   - awaitable 构造只接受 `std::shared_ptr<Connection>` 或 `ConnectionHandle`。
   - `Connection*` 只允许用于立即同步调用，不允许进入 awaiter 成员。
2. 删除或降级 `ConnectionRef` 的裸指针构造。
   - 如果暂时保留，命名成 `ConnectionView`，只用于不跨事件循环、不跨协程挂起的同步路径。
3. 推荐引入两个类型，而不是一个类型塞两种语义：
   - `ConnectionHandle`：内部持有 `std::shared_ptr<Connection>`，可跨 await。
   - `ConnectionView`：内部是 `Connection*`，不可保存到 awaiter / timer callback。
4. 更进一步可以做 `WeakConnectionHandle`。
   - timer、延迟 callback 只捕获 `std::weak_ptr<Connection>`。
   - 恢复时 `lock()`，失败就返回 `connection_closed` 或 `invalid_state`。

我更推荐当前阶段直接做方案 1 + 2：awaiter 和 runtime 注册路径全部走 `shared_ptr`，裸指针 API 先保留为 deprecated wrapper，内部立刻 `shared_from_this()` 转成 owner。如果调用者传入的对象并非 shared 管理，尽早暴露问题比偶发 UAF 更好。

### 2. awaitable 通过替换 `ConnectionHandler` 实现等待，不具备组合安全

位置：

- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:77)
- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:356)
- [connection_event_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/connection_event_awaitable.h:51)
- [datagram_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/datagram_io_awaitable.h:64)

现状：

- `async_read/write/flush/close/receive_from/wait_event` 都创建 proxy handler。
- proxy 保存原 handler，然后 `connection->set_connection_handler(proxy_)`。
- 完成后再尝试 restore。

风险：

- 同一个连接上两个 awaiter 并发等待时会互相覆盖。
- 协议层 handler、TLS handler、监控 handler、await proxy 都争抢同一个 handler slot。
- restore 时只能检查当前 owner 是否还是 proxy，无法表达“已有其他层接管”的合法状态。
- handler replacement 把业务协议回调和 coroutine wakeup 绑死，后续协议栈越复杂越难维护。

建议方向：

- `ConnectionHandler` 只保留为业务事件入口。
- 在 `Connection` 上新增独立的 waiters/observers：
  - `read_waiters`
  - `write_waiters`
  - `close_waiters`
  - `error_waiters`
  - `input_shutdown_waiters`
- 事件发生时先更新连接状态，再唤醒 waiters，再分发给业务 handler。
- waiters 建议是一次性 token，析构时自动取消，避免 awaiter 超时后回调还持有 `this`。

这一步会比修补 `ConnectionRef` 更关键，因为它能同时解决 handler 覆盖、事件丢失、timeout callback 捕获悬空 awaiter 等一组问题。

### 3. timer callback 捕获 awaiter 的 `this`，取消语义不够硬

位置：

- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:104)
- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:373)
- [stream_io_awaitable.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/stream_io_awaitable.h:1047)

风险：

- awaiter 对象活在 coroutine frame 里，timeout timer lambda 捕获裸 `this`。
- `await_resume()` 和析构里会 cancel timer，但这要求 timer manager 不会在 cancel 后再触发旧 task。
- 如果 tick、cancel、resume 的顺序稍有交错，就可能访问已经恢复或销毁的 awaiter。

建议方向：

- timeout 状态放进 `std::shared_ptr<OperationState>`，timer 捕获 weak state。
- awaiter 析构只标记 `cancelled = true`。
- timer 触发时 `if (auto s = weak.lock(); !s->cancelled && !s->completed) resume`。

### 4. epoll generation 设计没有闭环，stale fd 防护基本失效

位置：

- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:131)
- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:242)
- [epoll_poller.cpp](/home/yuan/codes/test/webserver/core/core/src/net/poller/epoll_poller.cpp:19)
- [epoll_poller.cpp](/home/yuan/codes/test/webserver/core/core/src/net/poller/epoll_poller.cpp:160)

现状：

- `EventLoop` 有 `channel_generations_`，关闭时递增。
- `EpollPoller` 自己也有 `ChannelEntry::generation`。
- 但 `EpollPoller` 里的 generation 没有和 `EventLoop` 的 generation 同步，也没有在 add/remove 时递增。
- `EventLoop` 只在 `event.generation != 0` 时校验，所以 generation 为 0 的事件直接放行。

风险：

- fd 快速关闭再复用时，旧 epoll 事件可能命中新连接。
- 当前 generation 字段看起来在防这个问题，但实际没有真正生效，容易给维护者错误安全感。

建议方向：

- generation 应该归属一个地方。更简单的做法：把 generation 放进 `Channel`。
- `EventLoop::update_channel` 传给 poller 的就是 channel 当前 generation。
- 每次 close/remove 后让 channel generation 失效，fd 复用必须创建新 channel/token。
- `event.generation == 0` 不应作为绕过校验的常态。

### 5. TCP graceful close 可能丢未刷出的输出

位置：

- [tcp_connection.cpp](/home/yuan/codes/test/webserver/core/core/src/net/connection/tcp_connection.cpp:323)
- [tcp_connection.cpp](/home/yuan/codes/test/webserver/core/core/src/net/connection/tcp_connection.cpp:620)

现状：

- `close()` 在有 pending output 时把状态设为 `closing` 并等待写事件。
- `on_write_event()` 进入后，如果状态是 `closing`，调用 handler 的 `on_write()` 后直接 `do_close()`，没有先 `flush()`。

风险：

- 调用 `write()` 后马上 `close()` 的场景，可能还没真正把 `output_buffer_` 写完就关闭。
- 上层协议看到 close 回调，但对端可能收到半包或收不到最后响应。

建议方向：

- `closing` 状态下写事件仍应先 `flush()`。
- 只有 `output_buffer_` 为空时才能 `do_close()`。
- `on_write` 回调是否应该在 flush 前触发需要明确。通常更推荐：flush 完成后通知 write-drained。

### 6. `TcpConnection::get_local_address()` 返回了 remote address

位置：

- [tcp_connection.cpp](/home/yuan/codes/test/webserver/core/core/src/net/connection/tcp_connection.cpp:146)

风险：

- 本地地址、端口统计和日志会错误。
- 代理、端口转发、审计、安全策略如果依赖 local address 会误判。

建议方向：

- `Socket` 里保存 remote/local 两个地址，或者 `TcpConnection` 在 fd 建立后调用 `getsockname()` 填充 local address。

## 中优先级问题

### 7. `Task<T>` API 容易误导调用者

位置：

- [task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:115)
- [task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:123)
- [task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:129)
- [task.h](/home/yuan/codes/test/webserver/core/core/include/coroutine/task.h:263)

问题：

- `Task<T>::execute()` 只 resume 一次，不是 run-to-completion。
- `execute()` 不检查 exception。
- `operator T()` 会让 coroutine 结果获取变得隐式，容易在未完成时取到默认值。
- `Task<void>::detach()` 对异常没有统一日志/上报。

建议：

- 移除 `operator T()`。
- `execute()` 改名为 `resume_once()`，或者实现真正的 sync wait。
- detached task 加 runtime exception sink。

### 8. `EventLoop::loop()` 同时承担一次 poll 和长期 run，语义不稳

位置：

- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:65)
- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:117)

问题：

- `loop()` 会在 `resume_coroutine_requested_` 为 true 时退出。
- `request_coroutine_resume()` 不是简单 wakeup，而是改变 loop 的退出条件。
- 这让 `NetworkRuntime::run()`、`sync_wait`、长期服务 run loop 容易混用语义。

建议：

- 分成两个 API：
  - `run_forever()`
  - `run_until_idle_or_resume()`
- `post_coroutine()` 只 wakeup，不应该默认让长期 loop 退出。

### 9. `EventLoop` 持有连接 owner，但生命周期边界不够显式

位置：

- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:51)
- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:177)
- [event_loop.cpp](/home/yuan/codes/test/webserver/core/core/src/event/event_loop.cpp:231)

问题：

- `EventLoop::connections_` 是 stream connection 的实际 owner 之一。
- `ConnectionRef` 裸指针路径和这个 owner map 的增删耦合，但 API 没有表达这种依赖。
- `UdpConnection` 生命周期则由 `UdpInstance` 管，和 TCP 不同。

建议：

- 明确 `ConnectionRegistry` 概念，统一 TCP/UDP 的拥有关系。
- `EventLoop` 负责事件调度，registry 负责连接 owner 和 fd/peer 映射。

## 代码组织建议

### 1. 拆开 core 的几个子域

现在 `core/core` 同时包含 buffer、timer、coroutine、event、net、ssl、acceptor、connector。建议逐步形成这些边界：

- `core/base`：时间、字符串、编码、无网络依赖。
- `core/buffer`：ByteBuffer/BufferChain，独立测试。
- `core/timer`：TimerManager/TimerTask，不依赖 net。
- `core/event`：Poller/EventLoop/Channel，只知道 fd 和 SelectHandler。
- `core/net`：Connection/Acceptor/Connector/Socket。
- `core/coroutine`：Task、RuntimeView、awaitable，但不要直接替换业务 handler。
- `core/app`：Application/Bootstrap/ServiceRegistry。

如果不想改目录，至少在 CMake target 上拆。比如 `CoreBuffer`、`CoreTimer`、`CoreEvent`、`CoreNet`、`CoreCoroutine`。这样依赖方向会被编译器帮你守住。

### 2. `secuity` 拼写建议现在就改

目录是 `net/secuity`，建议改成 `net/security`。个人项目未稳定时最好现在改，后面协议层 include 多了会越来越烦。

### 3. handler 命名需要区分“业务处理器”和“事件观察者”

建议保留：

- `ConnectionHandler`：业务协议 handler，单 owner。

新增：

- `ConnectionObserver`：可多播观察，不影响主 handler。
- `ConnectionWaiter` 或 `IoWaiter`：一次性 coroutine waiter。
- `ConnectionLifecycle`：close/error/shutdown 状态订阅。

这样 awaitable 不再假装自己是业务 handler。

## 推荐重构路线

### Phase 1：先把生命周期补硬

- awaitable 构造统一持有 `std::shared_ptr<Connection>`。
- `RuntimeView::{read,write,flush,close,receive_from}` 的 `Connection*` 重载标记 deprecated，内部立刻转 `shared_from_this()`。
- timeout callback 改成 weak operation state，不捕获 awaiter `this`。
- 补测试：连接关闭后 timeout、timeout 后连接再触发事件、write 后 close。

### Phase 2：移除 handler swapping

- 在 `Connection` 增加一次性 waiter 注册接口。
- `TcpConnection::on_read_event/on_write_event/do_close` 触发 waiters。
- awaitable 从“替换 handler”改成“注册 waiter token”。
- 业务 handler 不再因为 coroutine await 被替换。

### Phase 3：整理 EventLoop/Poller token

- generation 放到 `Channel` 或 `PollToken`。
- poller 不维护独立 generation。
- `EventLoop` 对所有事件强制校验 token。
- close/remove/update 的顺序加测试覆盖 fd reuse。

### Phase 4：收 API 和模块边界

- 删除 `ConnectionRef` 或改名拆成 `ConnectionHandle/ConnectionView`。
- 去掉 `Task<T>::operator T()`，重命名 `execute()`。
- `core/core` 内部按 target 或目录拆层。
- 修正 `net/security` 拼写。

## 建议的 `ConnectionRef` 替代设计

推荐最终形态：

```cpp
class ConnectionHandle {
public:
    explicit ConnectionHandle(std::shared_ptr<Connection> conn) noexcept;
    Connection* get() const noexcept;
    Connection& operator*() const noexcept;
    Connection* operator->() const noexcept;
    std::shared_ptr<Connection> shared() const noexcept;
    explicit operator bool() const noexcept;

private:
    std::shared_ptr<Connection> conn_;
};

class ConnectionView {
public:
    explicit ConnectionView(Connection& conn) noexcept;
    Connection* get() const noexcept;
    Connection& operator*() const noexcept;
    Connection* operator->() const noexcept;

private:
    Connection* conn_;
};
```

使用规则：

- `ConnectionHandle` 可以进 awaiter、timer、lambda、跨线程队列。
- `ConnectionView` 只能用于当前调用栈，不能保存为成员。
- 所有 coroutine I/O API 接受 `ConnectionHandle` 或 `std::shared_ptr<Connection>`。
- 如果确实想支持裸指针，API 名字要暴露风险，比如 `unsafe_read(Connection*)`，不要伪装成普通 `read(Connection*)`。

如果想更稳一点，再加：

```cpp
class WeakConnectionHandle {
public:
    explicit WeakConnectionHandle(const std::shared_ptr<Connection>& conn) noexcept;
    std::shared_ptr<Connection> lock() const noexcept;

private:
    std::weak_ptr<Connection> conn_;
};
```

timer 和延迟恢复逻辑优先捕获 `WeakConnectionHandle`，避免延长连接寿命导致关闭不及时。

## 测试缺口

建议补这些 core 级测试：

- `async_read` 挂起后连接关闭，必须恢复并返回 closed。
- `async_read` timeout 后，再触发 read/close，不允许访问已完成 awaiter。
- `async_write` 小包 loopback 立即完成，不应挂起。
- `write + close` 必须保证对端收到完整 payload。
- fd close 后快速复用，不应把旧事件投递到新 channel。
- 两个 awaiter 同时等待同一连接时应明确失败，或按设计都能正确完成。
- detached coroutine 抛异常时必须有日志或 error hook。

## 结论

`core` 现在最该改的不是小的代码风格，而是三个基础语义：

1. 连接生命周期必须显式：跨 await 必须有 owner，裸指针只能是同步 view。
2. coroutine I/O completion 不能靠替换业务 handler。
3. EventLoop/Poller 的事件 token 要真正闭环，避免 fd reuse 类问题。

这三点做完后，协议层会轻很多，后面 SSH/HTTP/MQTT/WebSocket 这类长连接协议也更容易叠功能。
