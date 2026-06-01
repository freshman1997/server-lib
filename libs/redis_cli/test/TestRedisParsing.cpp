#include "buffer/byte_buffer.h"
#include "buffer/byte_buffer_reader.h"
#include "cmd/default_cmd.h"
#include "cmd/pipeline_cmd.h"
#include "cmd/subcribe_cmd.h"
#include "value/int_value.h"
#include "value/string_value.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#undef assert
#define assert(expr)                                                                                                   \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "assertion failed: " << #expr << " at " << __FILE__ << ":" << __LINE__ << std::endl;          \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (false)

namespace
{
    void fill_reader(yuan::buffer::ByteBufferReader &reader, const std::string &payload)
    {
        yuan::buffer::ByteBuffer buffer{std::string_view(payload)};
        reader.add_buffer(buffer);
    }

    void assert_pack_eq(const yuan::redis::DefaultCmd &cmd, const std::string &expected)
    {
        const auto actual = cmd.pack();
        if (actual != expected) {
            std::cerr << "Expected:\n" << expected << "\nActual:\n" << actual << std::endl;
        }
        assert(actual == expected);
    }
}

int main()
{
    using namespace yuan::redis;

    {
        DefaultCmd cmd;
        cmd.set_args("get", {StringValue::from_string("missing")});

        yuan::buffer::ByteBufferReader null_reader;
        fill_reader(null_reader, "$-1\r\n");
        assert(cmd.unpack(null_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_null);
        assert(cmd.get_result()->to_string() == "null");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("exec", {});

        yuan::buffer::ByteBufferReader null_array_reader;
        fill_reader(null_array_reader, "*-1\r\n");
        assert(cmd.unpack(null_array_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_null);
    }

    {
        DefaultCmd cmd;
        cmd.set_args("get", {StringValue::from_string("invalid-length")});

        yuan::buffer::ByteBufferReader invalid_reader;
        fill_reader(invalid_reader, "$5abc\r\nhello\r\n");
        assert(cmd.unpack(invalid_reader) == UnpackCode::format_error);
    }

    {
        DefaultCmd cmd;
        cmd.set_args("incr", {StringValue::from_string("invalid-int")});

        yuan::buffer::ByteBufferReader valid_reader;
        fill_reader(valid_reader, ":1\r\n");
        assert(cmd.unpack(valid_reader) == 0);
        assert(cmd.get_result() != nullptr);

        yuan::buffer::ByteBufferReader invalid_reader;
        fill_reader(invalid_reader, ":1abc\r\n");
        assert(cmd.unpack(invalid_reader) == UnpackCode::format_error);
        assert(cmd.get_result() == nullptr);
    }

    {
        DefaultCmd cmd;
        cmd.set_args("bigint", {});

        yuan::buffer::ByteBufferReader bigint_reader;
        fill_reader(bigint_reader, "(123456789012345678901234567890\r\n");
        assert(cmd.unpack(bigint_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_string);
        assert(cmd.get_result()->to_string() == "123456789012345678901234567890");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("bigint", {});

        yuan::buffer::ByteBufferReader invalid_bigint_reader;
        fill_reader(invalid_bigint_reader, "(12abc\r\n");
        assert(cmd.unpack(invalid_bigint_reader) == UnpackCode::format_error);
    }

    {
        DefaultCmd cmd;
        cmd.set_args("get", {StringValue::from_string("partial")});

        const std::string partial_payload = "$5\r\nhel";
        yuan::buffer::ByteBufferReader partial_reader;
        fill_reader(partial_reader, partial_payload);
        assert(cmd.unpack(partial_reader) == UnpackCode::need_more_bytes);
        assert(partial_reader.get_remain_bytes() == partial_payload.size());
        fill_reader(partial_reader, "lo\r\n");
        assert(cmd.unpack(partial_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_string);
        assert(cmd.get_result()->to_string() == "hello");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("get", {StringValue::from_string("multi-partial")});

        const std::string partial_second = "$5\r\nwor";
        yuan::buffer::ByteBufferReader multi_reader;
        fill_reader(multi_reader, "$5\r\nhello\r\n" + partial_second);
        assert(cmd.unpack(multi_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_string);
        assert(cmd.get_result()->to_string() == "hello");
        assert(multi_reader.get_remain_bytes() == partial_second.size());

        multi_reader.discard_read_bytes();
        fill_reader(multi_reader, "ld\r\n");
        assert(cmd.unpack(multi_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_string);
        assert(cmd.get_result()->to_string() == "world");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("get", {StringValue::from_string("partial-crlf")});

        yuan::buffer::ByteBufferReader partial_reader;
        fill_reader(partial_reader, "$5\r\nhello\r");
        assert(cmd.unpack(partial_reader) == UnpackCode::need_more_bytes);
        fill_reader(partial_reader, "\n");
        assert(cmd.unpack(partial_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_string);
        assert(cmd.get_result()->to_string() == "hello");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("hgetall", {StringValue::from_string("hash")});

        yuan::buffer::ByteBufferReader map_reader;
        fill_reader(map_reader, "%2\r\n$1\r\na\r\n:1\r\n$1\r\nb\r\n:2\r\n");
        assert(cmd.unpack(map_reader) == 0);
        assert(cmd.get_result() != nullptr);
        assert(cmd.get_result()->get_type() == resp_map);
    }

    {
        PipelineCmd pipeline_cmd;
        auto set_cmd = std::make_shared<DefaultCmd>();
        set_cmd->set_args("set", {
            StringValue::from_string("pkey"),
            StringValue::from_string("pvalue")
        });
        auto incr_cmd = std::make_shared<DefaultCmd>();
        incr_cmd->set_args("incr", {StringValue::from_string("pcounter")});
        pipeline_cmd.add_command(set_cmd);
        pipeline_cmd.add_command(incr_cmd);

        assert(
            pipeline_cmd.pack() ==
            "*3\r\n"
            "$3\r\nset\r\n"
            "$4\r\npkey\r\n"
            "$6\r\npvalue\r\n"
            "*2\r\n"
            "$4\r\nincr\r\n"
            "$8\r\npcounter\r\n");

        yuan::buffer::ByteBufferReader pipeline_reader;
        fill_reader(pipeline_reader, "+OK\r\n:2\r\n");
        assert(pipeline_cmd.unpack(pipeline_reader) == 0);
        assert(pipeline_cmd.get_result() != nullptr);
        assert(pipeline_cmd.get_result()->get_type() == resp_array);
        const auto &pipeline_values = pipeline_cmd.get_result()->as<ArrayValue>()->get_values();
        assert(pipeline_values.size() == 2);
        assert(pipeline_values[0]->to_string() == "OK");
        assert(pipeline_values[1]->to_string() == "2");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("linsert", {
            StringValue::from_string("queue"),
            StringValue::from_string("BEFORE"),
            StringValue::from_string("pivot"),
            StringValue::from_string("value")
        });
        assert_pack_eq(
            cmd,
            "*5\r\n"
            "$7\r\nlinsert\r\n"
            "$5\r\nqueue\r\n"
            "$6\r\nBEFORE\r\n"
            "$5\r\npivot\r\n"
            "$5\r\nvalue\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("EVAL", {
            StringValue::from_string("return ARGV[1]"),
            StringValue::from_string("0"),
            StringValue::from_string("hello")
        });
        assert_pack_eq(
            cmd,
            "*4\r\n"
            "$4\r\nEVAL\r\n"
            "$14\r\nreturn ARGV[1]\r\n"
            "$1\r\n0\r\n"
            "$5\r\nhello\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("zrangebyscore", {
            StringValue::from_string("scores"),
            StringValue::from_string("(1.5"),
            StringValue::from_string("9.5"),
            StringValue::from_string("LIMIT"),
            std::make_shared<IntValue>(2),
            std::make_shared<IntValue>(5),
            StringValue::from_string("WITHSCORES")
        });
        assert_pack_eq(
            cmd,
            "*8\r\n"
            "$13\r\nzrangebyscore\r\n"
            "$6\r\nscores\r\n"
            "$4\r\n(1.5\r\n"
            "$3\r\n9.5\r\n"
            "$5\r\nLIMIT\r\n"
            "$1\r\n2\r\n"
            "$1\r\n5\r\n"
            "$10\r\nWITHSCORES\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("bitfield", {
            StringValue::from_string("flags"),
            StringValue::from_string("OVERFLOW"),
            StringValue::from_string("SAT"),
            StringValue::from_string("INCRBY"),
            StringValue::from_string("i64"),
            std::make_shared<IntValue>(7),
            std::make_shared<IntValue>(3)
        });
        assert_pack_eq(
            cmd,
            "*8\r\n"
            "$8\r\nbitfield\r\n"
            "$5\r\nflags\r\n"
            "$8\r\nOVERFLOW\r\n"
            "$3\r\nSAT\r\n"
            "$6\r\nINCRBY\r\n"
            "$3\r\ni64\r\n"
            "$1\r\n7\r\n"
            "$1\r\n3\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("del", {
            StringValue::from_string("k1"),
            StringValue::from_string("k2"),
            StringValue::from_string("k3")
        });
        assert_pack_eq(
            cmd,
            "*4\r\n"
            "$3\r\ndel\r\n"
            "$2\r\nk1\r\n"
            "$2\r\nk2\r\n"
            "$2\r\nk3\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("scan", {
            std::make_shared<IntValue>(0),
            StringValue::from_string("MATCH"),
            StringValue::from_string("user:*"),
            StringValue::from_string("COUNT"),
            std::make_shared<IntValue>(200)
        });
        assert_pack_eq(
            cmd,
            "*6\r\n"
            "$4\r\nscan\r\n"
            "$1\r\n0\r\n"
            "$5\r\nMATCH\r\n"
            "$6\r\nuser:*\r\n"
            "$5\r\nCOUNT\r\n"
            "$3\r\n200\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("pexpire", {
            StringValue::from_string("session:42"),
            std::make_shared<IntValue>(1500)
        });
        assert_pack_eq(
            cmd,
            "*3\r\n"
            "$7\r\npexpire\r\n"
            "$10\r\nsession:42\r\n"
            "$4\r\n1500\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("set", {
            StringValue::from_string("token"),
            StringValue::from_string("abc"),
            StringValue::from_string("EX"),
            std::make_shared<IntValue>(30)
        });
        assert_pack_eq(
            cmd,
            "*5\r\n"
            "$3\r\nset\r\n"
            "$5\r\ntoken\r\n"
            "$3\r\nabc\r\n"
            "$2\r\nEX\r\n"
            "$2\r\n30\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("command", {
            StringValue::from_string("getkeys"),
            StringValue::from_string("EVAL"),
            StringValue::from_string("return 1"),
            std::make_shared<IntValue>(2),
            StringValue::from_string("k1"),
            StringValue::from_string("k2"),
            StringValue::from_string("arg1")
        });
        assert_pack_eq(
            cmd,
            "*8\r\n"
            "$7\r\ncommand\r\n"
            "$7\r\ngetkeys\r\n"
            "$4\r\nEVAL\r\n"
            "$8\r\nreturn 1\r\n"
            "$1\r\n2\r\n"
            "$2\r\nk1\r\n"
            "$2\r\nk2\r\n"
            "$4\r\narg1\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("hscan", {
            StringValue::from_string("meta"),
            std::make_shared<IntValue>(0),
            StringValue::from_string("MATCH"),
            StringValue::from_string("user:*"),
            StringValue::from_string("COUNT"),
            std::make_shared<IntValue>(50)
        });
        assert_pack_eq(
            cmd,
            "*7\r\n"
            "$5\r\nhscan\r\n"
            "$4\r\nmeta\r\n"
            "$1\r\n0\r\n"
            "$5\r\nMATCH\r\n"
            "$6\r\nuser:*\r\n"
            "$5\r\nCOUNT\r\n"
            "$2\r\n50\r\n");
    }

    {
        DefaultCmd cmd;
        cmd.set_args("zpopmin", {
            StringValue::from_string("leaders")
        });
        assert_pack_eq(
            cmd,
            "*2\r\n"
            "$7\r\nzpopmin\r\n"
            "$7\r\nleaders\r\n");
    }

    SubcribeCmd subscribe_cmd;
    subscribe_cmd.set_args("subscribe", {});
    subscribe_cmd.set_channels({"chan1"});

    yuan::buffer::ByteBufferReader subscribe_reader;
    fill_reader(subscribe_reader, "*3\r\n$9\r\nsubscribe\r\n$5\r\nchan1\r\n:1\r\n");
    assert(subscribe_cmd.unpack(subscribe_reader) == 0);
    assert(subscribe_cmd.is_subcribe());
    assert(subscribe_cmd.get_result() != nullptr);

    std::vector<std::string> received_messages;
    subscribe_cmd.set_msg_callback([&received_messages](const std::vector<SubMessage> &messages) {
        for (const auto &message : messages) {
            received_messages.push_back(message.channel->to_string() + ":" + message.message->to_string());
        }
    });

    yuan::buffer::ByteBufferReader message_reader;
    fill_reader(message_reader, "*3\r\n$7\r\nmessage\r\n$5\r\nchan1\r\n$5\r\nhello\r\n");
    assert(subscribe_cmd.unpack(message_reader) == 0);
    assert(subscribe_cmd.get_result() == nullptr);
    assert(subscribe_cmd.has_pending_messages());
    subscribe_cmd.exec_callback();
    assert(received_messages.size() == 1);
    assert(received_messages[0] == "chan1:hello");
    assert(!subscribe_cmd.has_pending_messages());

    yuan::buffer::ByteBufferReader double_message_reader;
    fill_reader(double_message_reader,
        "*3\r\n$7\r\nmessage\r\n$5\r\nchan1\r\n$5\r\nfirst\r\n"
        "*3\r\n$7\r\nmessage\r\n$5\r\nchan1\r\n$6\r\nsecond\r\n");
    assert(subscribe_cmd.unpack(double_message_reader) == 0);
    assert(subscribe_cmd.has_pending_messages());
    subscribe_cmd.exec_callback();
    assert(received_messages.size() == 3);
    assert(received_messages[1] == "chan1:first");
    assert(received_messages[2] == "chan1:second");
    assert(!subscribe_cmd.has_pending_messages());

    SubcribeCmd psubscribe_cmd;
    psubscribe_cmd.set_args("psubscribe", {});
    psubscribe_cmd.add_patterns({"test*"});

    yuan::buffer::ByteBufferReader psubscribe_reader;
    fill_reader(psubscribe_reader, "*3\r\n$10\r\npsubscribe\r\n$5\r\ntest*\r\n:1\r\n");
    assert(psubscribe_cmd.unpack(psubscribe_reader) == 0);
    assert(psubscribe_cmd.is_subcribe());

    std::vector<std::string> received_pmessages;
    psubscribe_cmd.set_pmsg_callback([&received_pmessages](const std::vector<PSubMessage> &messages) {
        for (const auto &message : messages) {
            received_pmessages.push_back(
                message.pattern->to_string() + ":" +
                message.channel->to_string() + ":" +
                message.message->to_string());
        }
    });

    yuan::buffer::ByteBufferReader pmessage_reader;
    fill_reader(pmessage_reader, "*4\r\n$8\r\npmessage\r\n$5\r\ntest*\r\n$5\r\ntest1\r\n$5\r\nworld\r\n");
    assert(psubscribe_cmd.unpack(pmessage_reader) == 0);
    assert(psubscribe_cmd.has_pending_messages());
    psubscribe_cmd.exec_callback();
    assert(received_pmessages.size() == 1);
    assert(received_pmessages[0] == "test*:test1:world");

    yuan::buffer::ByteBufferReader mixed_reader;
    fill_reader(mixed_reader,
        "*4\r\n$8\r\npmessage\r\n$5\r\ntest*\r\n$5\r\ntest2\r\n$3\r\none\r\n"
        "*3\r\n$7\r\nmessage\r\n$5\r\ntest2\r\n$3\r\ntwo\r\n");
    assert(psubscribe_cmd.unpack(mixed_reader) == 0);
    psubscribe_cmd.exec_callback();
    assert(received_pmessages.size() == 2);
    assert(received_pmessages[1] == "test*:test2:one");

    SubcribeCmd mixed_subscription_cmd;
    yuan::buffer::ByteBufferReader mixed_subscription_reader;
    fill_reader(mixed_subscription_reader,
        "*3\r\n$9\r\nsubscribe\r\n$5\r\nchan1\r\n:1\r\n"
        "*3\r\n$10\r\npsubscribe\r\n$5\r\ntest*\r\n:2\r\n");
    assert(mixed_subscription_cmd.unpack(mixed_subscription_reader) == 0);
    assert(mixed_subscription_cmd.is_subcribe());
    mixed_subscription_cmd.unsubcribe(std::vector<std::string>{});
    assert(mixed_subscription_cmd.is_subcribe());
    mixed_subscription_cmd.punsubcribe(std::vector<std::string>{});
    assert(!mixed_subscription_cmd.is_subcribe());

    SubcribeCmd limited_queue_cmd;
    limited_queue_cmd.set_max_pending_messages(1);
    yuan::buffer::ByteBufferReader limited_subscribe_reader;
    fill_reader(limited_subscribe_reader, "*3\r\n$9\r\nsubscribe\r\n$4\r\ntrim\r\n:1\r\n");
    assert(limited_queue_cmd.unpack(limited_subscribe_reader) == 0);

    yuan::buffer::ByteBufferReader first_limited_message_reader;
    fill_reader(first_limited_message_reader, "*3\r\n$7\r\nmessage\r\n$4\r\ntrim\r\n$5\r\nfirst\r\n");
    assert(limited_queue_cmd.unpack(first_limited_message_reader) == 0);
    assert(limited_queue_cmd.has_pending_messages());

    yuan::buffer::ByteBufferReader second_limited_message_reader;
    fill_reader(second_limited_message_reader, "*3\r\n$7\r\nmessage\r\n$4\r\ntrim\r\n$6\r\nsecond\r\n");
    assert(limited_queue_cmd.unpack(second_limited_message_reader) == 0);
    assert(limited_queue_cmd.dropped_message_batches() == 1);

    std::vector<std::string> limited_messages;
    limited_queue_cmd.set_msg_callback([&limited_messages](const std::vector<SubMessage> &messages) {
        for (const auto &message : messages) {
            limited_messages.push_back(message.message->to_string());
        }
    });
    limited_queue_cmd.exec_callback();
    assert(limited_messages.size() == 1);
    assert(limited_messages[0] == "second");
    assert(!limited_queue_cmd.has_pending_messages());

    SubcribeCmd no_callback_cmd;
    yuan::buffer::ByteBufferReader no_callback_subscribe_reader;
    fill_reader(no_callback_subscribe_reader, "*3\r\n$9\r\nsubscribe\r\n$8\r\nnocbchan\r\n:1\r\n");
    assert(no_callback_cmd.unpack(no_callback_subscribe_reader) == 0);
    yuan::buffer::ByteBufferReader no_callback_message_reader;
    fill_reader(no_callback_message_reader, "*3\r\n$7\r\nmessage\r\n$8\r\nnocbchan\r\n$5\r\nhello\r\n");
    assert(no_callback_cmd.unpack(no_callback_message_reader) == 0);
    assert(no_callback_cmd.has_pending_messages());
    no_callback_cmd.exec_callback();
    assert(!no_callback_cmd.has_pending_messages());

    return 0;
}
