#ifndef __NET_DNS_PACKET_H__
#define __NET_DNS_PACKET_H__
#include <cstdint>
#include <string>
#include <vector>
#include "buffer/linked_buffer.h"

namespace yuan::net::dns 
{
    class DnsPacket
    {
    public:
    
    private:
        uint16_t sessionId_;
        uint16_t flag_;

        uint16_t question_amount_;                      // 问题数
        uint16_t answerRRs_amount_;                     // 资源记录数
        uint16_t authorityRRs_amount_;                  // 授权资源记录数
        uint16_t additionalRRs_amount_;                 // 额外资源记录数

        std::vector<std::string> query_questions_;      // 查询问题
        std::vector<std::string> answers_;              // 回答
        std::vector<std::string> authorities_;          // 授权
        std::vector<std::string> extra_infos_;          // 额外信息

        LinkedBuffer buffer_;
    };
}

#endif