#ifndef __NET_WEBSOCKET_COMMON_CLOSE_CODE_H__
#define __NET_WEBSOCKET_COMMON_CLOSE_CODE_H__

namespace net::websocket 
{
    // | 1012-1016 | 保留字段，未定义具体含义 |
    // | 1017-1023 | 未分配 |
    // | 1024-49151 | 为应用程序保留 |
    // | 49152-65535 | 为扩展保留 |
    // 扩展原因码（后 8 位）
    // 扩展原因码是由 WebSocket 应用程序或框架自定义的，用于提供更详细的关闭连接原因。标准原因码的后 8 位为 0，而扩展原因码的后 8 位不为 0。
    enum class WebSocketCloseCode
    {
        normal_close_               = 1000, // 正常关闭
        going_away_                 = 1001, // 终端离开
        protocol_error_             = 1002, // 协议错误
        unsupport_data_             = 1003, // 不支持的数据类型
        reserve_                    = 1004, // 保留字段，未定义具体含义
        no_status_code_             = 1005, // 没有状态码
        abnormal_close_             = 1006, // 连接异常关闭
        invalid_palyload_           = 1007, // 数据帧格式错误
        policy_violation_           = 1008, // 政策违规
        message_too_big_            = 1009, // 消息过大
        mising_extension_           = 1010, // 必需扩展缺失
        internal_server_error_      = 1011, // 服务端错误
    };
}

#endif