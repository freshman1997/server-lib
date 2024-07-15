#ifndef __NET_FTP_COMMON_SESSION_H__
#define __NET_FTP_COMMON_SESSION_H__

#include "../../base/handler/connection_handler.h"
#include "../../base/connection/connection.h"
#include "file_manager.h"
#include "../../base/event/event_loop.h"
#include "../handler/file_stream_event.h"
#include "../../../timer/timer_task.h"

#include <string>
#include <type_traits>
#include <unordered_map>
#include <cstdint>
#include <queue>

namespace net::ftp 
{
    class FtpFileStream;
    class FtpApp;

    enum class FtpSessionValueType : int
    {
        invalid = -1,
        int32_val,
        double_val,
        string_val,
        ptr_val,
    };

    struct FtpSessionValue
    {
        union 
        {
            int32_t         ival;
            double          dval;
            std::string    *sval;
            void           *ptr;
        } item;

        FtpSessionValueType type = FtpSessionValueType::invalid;
    };


    enum class FtpSessionWorkMode : char
    {
        client = 0,
        server
    };

    class FtpSession;
    
    class FtpSessionContext
    {
        friend class FtpSession;
    public:
        FtpSessionContext();
        ~FtpSessionContext();

    public:
        void close();

    public:
        bool login_success()
        {
            return !password_.empty() && verified_;
        }

        bool start_file_stream_transe();

    private:
        bool verified_;
        FtpSession *instance_;
        FtpFileStream *file_stream_;
        FtpApp *app_;
        Connection *conn_;
        std::string username_;
        std::string password_;
        FileManager file_manager_;
        std::unordered_map<std::string, FtpSessionValue> values;
        std::queue<std::string> cmd_queue_;
    };

    class FtpSession : public ConnectionHandler, public FtpFileStreamEvent
    {
        friend class FtpSessionContext;
    public:
        FtpSession(Connection *conn, FtpApp *entry, FtpSessionWorkMode workMode = FtpSessionWorkMode::server, bool keepUtilSent = false);
        ~FtpSession();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn);

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public: // timer task
        virtual void on_timer(timer::Timer *timer);

    public: // file stream event
        virtual void on_opened(FtpFileStream *fs);

        virtual void on_connect_timeout(FtpFileStream *fs);

        virtual void on_start(FtpFileStream *fs);

        virtual void on_error(FtpFileStream *fs);

        virtual void on_completed(FtpFileStream *fs);

        virtual void on_closed(FtpFileStream *fs);

        virtual void on_idle_timeout(FtpFileStream *fs);
    
    public:
        void quit();

        Connection * get_connection()
        {
            return context_.conn_;
        }

        FtpApp * get_app()
        {
            return context_.app_;
        }

        bool on_login(const std::string &username, std::string &passwd);

        bool start_file_stream(const InetAddress &addr, StreamMode mode);

        bool login_success()
        {
            return context_.login_success();
        }

        void add_command(const std::string &cmd);

        void check_file_stream(FtpFileStream *fs);

        template<typename T>
        void set_item_value(const std::string &key, T && val)
        {
            FtpSessionValue itemVal;
            if constexpr (std::is_same<int32_t, T>::value) {
                itemVal.type = FtpSessionValueType::int32_val;
                itemVal.item.ival = val;
            } else if constexpr (std::is_same<void *, T>::value) {
                itemVal.type = FtpSessionValueType::ptr_val;
                itemVal.item.ptr = val;
            } else if constexpr (std::is_same<double, T>::value) {
                itemVal.type = FtpSessionValueType::double_val;
                itemVal.item.dval = val;
            } else if constexpr (std::is_same<std::string, T>::value) {
                itemVal.type = FtpSessionValueType::string_val;
                itemVal.item.sval = new T(val);
            } else {
                static_assert("invalid type found");
            }

            context_.values[key] = itemVal;
        }

        void remove_item(const std::string &key)
        {
            context_.values.erase(key);
        }

        int32_t get_i_value(const std::string &key)
        {
            auto it = context_.values.find(key);
            if (it == context_.values.end()) {
                return 0;
            }
            return it->second.type == FtpSessionValueType::int32_val ? it->second.item.ival : 0;
        }

        double get_d_value(const std::string &key)
        {
            auto it = context_.values.find(key);
            if (it == context_.values.end()) {
                return 0;
            }
            return it->second.type == FtpSessionValueType::double_val ? it->second.item.dval : 0;
        }

        void * get_ptr_value(const std::string &key)
        {
            auto it = context_.values.find(key);
            if (it == context_.values.end()) {
                return nullptr;
            }
            return it->second.type == FtpSessionValueType::ptr_val ? it->second.item.ptr : 0;
        }

        const std::string * get_str_value(const std::string &key)
        {
            auto it = context_.values.find(key);
            if (it == context_.values.end()) {
                return nullptr;
            }
            return it->second.type == FtpSessionValueType::string_val ? it->second.item.sval : 0;
        }

    private:
        bool keep_util_sent_;
        bool close_;
        FtpSessionWorkMode work_mode_;
        FtpSessionContext context_;
    };
}

#endif