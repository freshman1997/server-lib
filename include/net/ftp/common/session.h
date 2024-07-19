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
        } item{.ptr = nullptr};

        FtpSessionValueType type = FtpSessionValueType::invalid;
    };

    class FtpSession;
    
    class FtpSessionContext
    {
    public:
        FtpSessionContext();
        ~FtpSessionContext();

    public:
        void close();

    public:
        bool login_success()
        {
            return !user_.logined_;
        }

        bool start_file_stream_transe();

    public:
        FtpSession *instance_;
        FtpFileStream *file_stream_;
        FtpApp *app_;
        Connection *conn_;
        User user_;
        FileManager file_manager_;
        std::string cwd_;
        std::unordered_map<std::string, FtpSessionValue> values;
    };

    class FtpSession : public ConnectionHandler, public FtpFileStreamEvent
    {
        friend class FtpSessionContext;
    public:
        FtpSession(Connection *conn, FtpApp *app, WorkMode mode, bool keepUtilSent = false);
        virtual ~FtpSession();

    public:
        virtual void on_connected(Connection *conn);

        virtual void on_error(Connection *conn);

        virtual void on_read(Connection *conn) = 0;

        virtual void on_write(Connection *conn);

        virtual void on_close(Connection *conn);

    public: // timer task
        virtual void on_timer(timer::Timer *timer);

    public: // file stream event
        virtual void on_opened(FtpFileStreamSession *fs);

        virtual void on_connect_timeout(FtpFileStreamSession *fs);

        virtual void on_start(FtpFileStreamSession *fs);

        virtual void on_error(FtpFileStreamSession *fs);

        virtual void on_completed(FtpFileStreamSession *fs);

        virtual void on_closed(FtpFileStreamSession *fs);

        virtual void on_idle_timeout(FtpFileStreamSession *fs);
    
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

        bool login();

        void set_username(const std::string &username);

        void set_password(const std::string &passwd);

        bool start_file_stream(const InetAddress &addr, StreamMode mode);

        bool login_success()
        {
            return context_.login_success();
        }

        bool send_command(const std::string &cmd);

        void change_cwd(const std::string &filepath);

        void check_file_stream(FtpFileStreamSession *fs);

        void set_keep_util_sent_flag(bool flag)
        {
            keep_util_sent_ = flag;
        }

        bool get_keep_util_sent_flag(bool flag)
        {
            return keep_util_sent_;
        }

        void on_error(int errcode);

    public:
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

        template<typename T>
        T get_item_value(const std::string &key)
        {
            auto it = context_.values.find(key);
            if constexpr (std::is_same<int32_t, T>::value) {
                if (it == context_.values.end()) {
                    return 0;
                }
                return it->second.type == FtpSessionValueType::int32_val ? it->second.item.ival : 0;
            } else if constexpr (std::is_same<void *, T>::value) {
                if (it == context_.values.end()) {
                    return nullptr;
                }
                return it->second.type == FtpSessionValueType::ptr_val ? it->second.item.ptr : nullptr;
            } else if constexpr (std::is_same<double, T>::value) {
                if (it == context_.values.end()) {
                    return 0;
                }
                return it->second.type == FtpSessionValueType::double_val ? it->second.item.dval : 0;
            } else if constexpr (std::is_same<std::string *, T>::value) {
                if (it == context_.values.end()) {
                    return nullptr;
                }
                return it->second.type == FtpSessionValueType::string_val ? it->second.item.sval : nullptr;
            } else {
                static_assert("invalid type found");
            }

            return {};
        }

        void remove_item(const std::string &key)
        {
            context_.values.erase(key);
        }

        FileManager * get_file_manager()
        {
            return &context_.file_manager_;
        }

        bool set_work_file(FtpFileInfo *info);

    protected:
        WorkMode work_mode_;
        bool keep_util_sent_;
        bool close_;
        FtpSessionContext context_;
    };
}

#endif