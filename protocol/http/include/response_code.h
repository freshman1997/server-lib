#ifndef __REPONSE_CODE_H__
#define __REPONSE_CODE_H__

namespace yuan::net::http
{
    enum ResponseCode : short
    {
        invalid = -1,
        
        /* 信息响应 */
        continue_ = 100,                            // 服务器已收到请求的初始部分，客户端应该继续发送其余部分
        switch_protocol = 101,                      // 服务器已理解并接受了客户端请求，将切换到不同的协议
        processing = 102,                           // 此代码表示服务器已收到并正在处理该请求，但当前没有响应可用
        early_hints = 103,                          // 此状态代码主要用于与 Link 链接头一起使用，以允许用户代理在服务器准备响应阶段时开始预加载 preloading 资源

        /* 成功响应 */
        ok_ = 200,                                  // 请求成功，返回所请求的数据
        created_ = 201,                             // 请求已经被实现，服务器创建了新的资源
        accepted = 202,                             // 请求已经接收到，但还未响应，没有结果
        non_authoritative_information = 203,        // 服务器已成功处理了请求，但返回的实体头部元信息不是在原始服务器上有效的确定集合，而是来自本地或者第三方的拷贝
        no_content = 204,                           // 服务器成功处理了请求，但没有返回任何内容
        reset_content = 205,                        // 告诉用户代理重置发送此请求的文档
        partial_content = 206,                      // 当从客户端发送Range范围标头以只请求资源的一部分时，将使用此响应代码
        multi_status = 207,                         // 对于多个状态代码都可能合适的情况，传输有关多个资源的信息

        /* 重定向消息 */
        multiple_choice = 300,                      // 请求拥有多个可能的响应
        moved_permanently = 301,                    // 请求的资源已被永久移动到新的UR
        found = 302,                                // 资源的URI临时变更，需要使用新的URI进行访问
        see_other = 303,                            // 服务器发送此响应，以指示客户端通过一个 GET 请求在另一个 URI 中获取所请求的资源
        not_modified = 304,                         // 资源在上次请求之后没有发生变化

        /* 客户端错误响应 */
        bad_request = 400,                          // 请求无效，服务器无法理解
        unauthorized = 401,                         // 未经授权，需要身份验证
        payment_required = 402,                     // 此响应代码保留供将来使用。创建此代码的最初目的是将其用于数字支付系统，但是此状态代码很少使用，并且不存在标准约定。
        forbidden = 403,                            // 服务器拒绝请求
        not_found = 404,                            // 请求的资源不存在
        method_not_allowed = 405,                   // 服务器知道请求方法，但目标资源不支持该方法
        not_acceptable = 406,                       // 当 web 服务器在执行 服务端驱动型内容协商机制 后，没有发现任何符合用户代理给定标准的内容时，就会发送此响应
        proxy_authentication_required = 407,        // 类似于 401 Unauthorized 但是认证需要由代理完成
        request_timeout = 408,                      // 此响应由一些服务器在空闲连接上发送，即使客户端之前没有任何请求
        conflict = 409,                             // 当请求与服务器的当前状态冲突时，将发送此响应
        gone = 410,                                 // 当请求的内容已从服务器中永久删除且没有转发地址时，将发送此响应
        length_required = 411,                      // 服务端拒绝该请求因为 Content-Length 头部字段未定义但是服务端需要它
        precondition_failed = 412,                  // 客户端在其头文件中指出了服务器不满足的先决条件
        payload_too_large = 413,                    // 请求实体大于服务器定义的限制。服务器可能会关闭连接，或在标头字段后返回重试 Retry-After
        uri_too_long = 414,                         // 客户端请求的 URI 比服务器愿意接收的长度长
        unsupported_media_type = 415,               // 服务器不支持请求数据的媒体格式，因此服务器拒绝请求
        range_not_satisfiable = 416,                // 无法满足请求中 Range 标头字段指定的范围。该范围可能超出了目标 URI 数据的大小
        expectation_failed = 417,                   // 此响应代码表示服务器无法满足 Expect 请求标头字段所指示的期望
        i_m_a_teapot = 418,                         // 服务端拒绝用茶壶煮咖啡
        misdirected_request = 421,                  // 请求被定向到无法生成响应的服务器。这可以由未配置为针对请求 URI 中包含的方案和权限组合生成响应的服务器发送
        unprocessable_entity = 422,                 // 请求格式正确，但由于语义错误而无法遵循
        locked = 423,                               // 正在访问的资源已锁定。
        failed_dependency = 424,                    // 由于前一个请求失败，请求失败。
        too_early = 425,                            // 表示服务器不愿意冒险处理可能被重播的请求
        upgrade_required = 426,                     // 服务器拒绝使用当前协议执行请求，但在客户端升级到其他协议后可能愿意这样做
        precondition_required = 428,                // 源服务器要求请求是有条件的
        too_many_requests = 429,                    // 用户在给定的时间内发送了太多请求
        request_header_fields_too_large = 430,      // 服务器不愿意处理请求，因为其头字段太大
        unavailable_for_legal_reasons = 451,        // 用户代理请求了无法合法提供的资源，例如政府审查的网页

        /* 服务端错误响应 */
        internal_server_error = 500,                // 服务器遇到了意外的错误
        not_implemented = 501,                      // 服务器不支持请求方法，因此无法处理。服务器需要支持的唯二方法（因此不能返回此代码）是 GET and HEAD.
        bad_gateway = 502,                          // 服务器作为网关或代理，从上游服务器接收到无效的响应
        service_unavailable = 503,                  // 服务器当前无法处理请求，可能是暂时过载或维护中
        gateway_timeout = 504,                      // 当服务器充当网关且无法及时获得响应时，会给出此错误响应
        http_version_not_supported = 505,           // 服务器不支持请求中使用的 HTTP 版本
        variant_also_negotiates = 506,              // 服务器存在内部配置错误：所选的变体资源被配置为参与透明内容协商本身，因此不是协商过程中的适当终点。
        insufficient_storage = 507,                 // 无法在资源上执行该方法，因为服务器无法存储成功完成请求所需的表示
        loop_detected = 508,                        // 服务器在处理请求时检测到无限循环
        not_extended = 510,                         // 服务器需要对请求进行进一步扩展才能完成请求
        network_authentication_required = 511,      // 指示客户端需要进行身份验证才能获得网络访问权限。
    };
}

#endif
