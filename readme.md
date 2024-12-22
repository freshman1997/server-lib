## C++ 网络库封装
* 对 select、poll、epoll、kqueue 多路复用包装，上层异步接收通知
* 事件循环处理读写事件
* 封装 tcp 连接、udp 连接实例并提供上层回调（可接入 kcp）
* 时间轮定时器
* 可简单使用openssl对接SSL
* TODO 多线程实现
* TODO 简单日志打印

## 目前基于上面的封装实现协议
* http 协议 <br/>
    1. 简单报文解析（根据状态机）和打包报文发送，目前支持文件上传，表单提交
    2. request，response 封装
    3. 提供简单的分片流媒体播放，可播放mp4等
    4. 提供http客户端和http服务器

<br/>

* ftp 协议 <br/>
    1. ftp 控制链接和数据链接的基本框架搭建
    2. 普通数据传输
    3. 提供客户端和服务器
    3. TODO 命令实现

* websocket 协议 <br/>
    1. 握手实现
    2. 报文解析、分片接收和发送
    3. 心跳、关闭连接
    4. 提供客户端和服务器


TODO 协议 bit_torrent, dns  <br/><br/><br/>

# 编译
* clone 下来后，执行 ``` git submodule update  --init --progress ``` 拉子模块，完成到 ``` third_party/openssl-3.4.0 ``` 切换到 origin/openssl-3.4 分支 ``` git checkout origin/openssl-3.4 ```
* 需要先编译 ``` third_party/openssl-3.4.0 ``` 下面的 openssl 3.4.0 <br/>
    1. windows 下使用vs编译工具，管理员模式打开编译命令行，``` perl Configure VC-WIN64A no-shared no-ASM ``` 执行看到success后 ``` nmake ```
    2. 使用mingw编译，需要安装 ```msys2```，安装了必要的gcc，g++，make工具链后，进到``` third_party/openssl-3.4.0 ```目录下，``` ./build.sh ```没报错即编译完成
    3. linux下编译，执行 ```./build.sh``` 编译即可
* 回到根目录 <br/>
    1. Windows 下 新建个build 文件夹，然后执行 ``` cd build ``` ``` cmake .. ``` 即可生成vs的工程文件
    2. linux 直接执行 ``` ./build.sh ``` 即可完成编译


