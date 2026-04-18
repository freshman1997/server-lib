#ifndef NET_FTP_COMMON_SESSION_H
#define NET_FTP_COMMON_SESSION_H

#include "net/handler/connection_handler.h"
#include "net/connection/connection.h"
#include "net/socket/inet_address.h"
#include "net/async/async_listener_host.h"
#include "net/runtime/network_runtime.h"
#include "common/def.h"
#include "file_manager.h"
#include "handler/file_stream_event.h"
#include "timer/timer_task.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace yuan::net::ftp
{
    class FtpFileStream;
    class FtpApp;

    enum class FtpSessionValueType : int {
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
            int32_t ival;
            double dval;
            std::string *sval;
            void *ptr;
        } item{ .ptr = nullptr };

        FtpSessionValueType type = FtpSessionValueType::invalid;

        FtpSessionValue() = default;
        ~FtpSessionValue() = default;

        FtpSessionValue(const FtpSessionValue &other)
            : type(other.type)
        {
            switch (type) {
            case FtpSessionValueType::int32_val:
                item.ival = other.item.ival;
                break;
            case FtpSessionValueType::double_val:
                item.dval = other.item.dval;
                break;
            case FtpSessionValueType::string_val:
                item.sval = other.item.sval ? new std::string(*other.item.sval) : nullptr;
                break;
            case FtpSessionValueType::ptr_val:
                item.ptr = other.item.ptr;
                break;
            default:
                item.ptr = nullptr;
                break;
            }
        }

        FtpSessionValue &operator=(const FtpSessionValue &other)
        {
            if (this != &other) {
                destroy_string();
                type = other.type;
                switch (type) {
                case FtpSessionValueType::int32_val:
                    item.ival = other.item.ival;
                    break;
                case FtpSessionValueType::double_val:
                    item.dval = other.item.dval;
                    break;
                case FtpSessionValueType::string_val:
                    item.sval = other.item.sval ? new std::string(*other.item.sval) : nullptr;
                    break;
                case FtpSessionValueType::ptr_val:
                    item.ptr = other.item.ptr;
                    break;
                default:
                    item.ptr = nullptr;
                    break;
                }
            }
            return *this;
        }

        FtpSessionValue(FtpSessionValue &&other) noexcept : type(other.type)
        {
            item.ptr = other.item.ptr;
            other.type = FtpSessionValueType::invalid;
            other.item.ptr = nullptr;
        }

        FtpSessionValue &operator=(FtpSessionValue &&other) noexcept
        {
            if (this != &other) {
                destroy_string();
                type = other.type;
                item.ptr = other.item.ptr;
                other.type = FtpSessionValueType::invalid;
                other.item.ptr = nullptr;
            }
            return *this;
        }

    private:
        void destroy_string()
        {
            if (type == FtpSessionValueType::string_val && item.sval) {
                delete item.sval;
                item.sval = nullptr;
            }
        }
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
            return user_.logined_;
        }

    public:
        FtpSession *instance_;
        FtpFileStream *file_stream_;
        FtpApp *app_;
        Connection *conn_;
        User user_;
        FileManager file_manager_;
        std::string cwd_;
        std::string root_dir_;
        std::optional<InetAddress> passive_addr_;
        std::unordered_map<std::string, FtpSessionValue> values;
    };

    class FtpSession : public ConnectionHandler, public FtpFileStreamEvent
    {
        friend class FtpSessionContext;

    public:
        FtpSession(Connection *conn, FtpApp *app, WorkMode mode, bool keepUtilSent = false, bool async_mode = false);
        virtual ~FtpSession();
        virtual void on_connected(Connection *conn);
        virtual void on_error(Connection *conn);
        virtual void on_read(Connection *conn) = 0;
        virtual void on_write(Connection *conn);
        virtual void on_close(Connection *conn);
        virtual void on_timer(timer::Timer *timer);
        virtual void on_opened(FtpFileStreamSession *fs);
        virtual void on_connect_timeout(FtpFileStreamSession *fs);
        virtual void on_start(FtpFileStreamSession *fs);
        virtual void on_error(FtpFileStreamSession *fs);
        virtual void on_completed(FtpFileStreamSession *fs);
        virtual void on_closed(FtpFileStreamSession *fs);
        virtual void on_idle_timeout(FtpFileStreamSession *fs);

        void quit();
        Connection *get_connection()
        {
            return context_.conn_;
        }
        FtpApp *get_app()
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
        const std::string &get_cwd() const
        {
            return context_.cwd_;
        }
        const std::string &get_root_dir() const
        {
            return context_.root_dir_;
        }
        void set_root_dir(const std::string &dir)
        {
            context_.root_dir_ = dir;
        }
        void set_passive_addr(const InetAddress &addr);
        std::optional<InetAddress> get_passive_addr() const
        {
            return context_.passive_addr_;
        }
        void clear_passive_addr();
        void check_file_stream(FtpFileStreamSession *fs);
        void set_keep_util_sent_flag(bool flag)
        {
            keep_util_sent_ = flag;
        }
        bool get_keep_util_sent_flag() const
        {
            return keep_util_sent_;
        }
        void on_error(int errcode);
        void on_file_stream_close(FtpFileStream *ffs);

        bool is_async_mode() const
        {
            return async_mode_;
        }
        net::AsyncListenerHost *data_listener() const
        {
            return data_listener_.get();
        }
        std::unique_ptr<net::AsyncListenerHost> take_data_listener()
        {
            return std::move(data_listener_);
        }
        bool has_pending_transfer() const
        {
            return pending_file_info_ != nullptr;
        }
        FtpFileInfo *pending_file_info() const
        {
            return pending_file_info_;
        }
        FtpFileInfo *take_pending_file_info()
        {
            auto *info = pending_file_info_;
            pending_file_info_ = nullptr;
            return info;
        }
        void clear_pending_transfer()
        {
            pending_file_info_ = nullptr;
        }
        void detach_async();

        template <typename T>
        void set_item_value(const std::string &key, T &&val)
        {
            auto it = context_.values.find(key);
            if (it != context_.values.end() && it->second.type == FtpSessionValueType::string_val && it->second.item.sval) {
                delete it->second.item.sval;
            }
            FtpSessionValue itemVal;
            using DecayedT = std::decay_t<T>;
            if
                constexpr(std::is_same_v<int32_t, DecayedT>)
                {
                    itemVal.type = FtpSessionValueType::int32_val;
                    itemVal.item.ival = val;
                }
            else if
                constexpr(std::is_same_v<void *, DecayedT>)
                {
                    itemVal.type = FtpSessionValueType::ptr_val;
                    itemVal.item.ptr = val;
                }
            else if
                constexpr(std::is_same_v<double, DecayedT>)
                {
                    itemVal.type = FtpSessionValueType::double_val;
                    itemVal.item.dval = val;
                }
            else if
                constexpr(std::is_same_v<std::string, DecayedT>)
                {
                    itemVal.type = FtpSessionValueType::string_val;
                    itemVal.item.sval = new std::string(val);
                }
            else if
                constexpr(std::is_convertible_v<DecayedT, const char *> && !std::is_same_v<DecayedT, void *>)
                {
                    itemVal.type = FtpSessionValueType::string_val;
                    itemVal.item.sval = new std::string(val);
                }
            else {
                static_assert(std::is_same_v<T, void>, "invalid type for set_item_value");
            }
            context_.values[key] = std::move(itemVal);
        }

        template <typename T>
        T get_item_value(const std::string &key)
        {
            auto it = context_.values.find(key);
            if
                constexpr(std::is_same_v<int32_t, T>)
                {
                    return it == context_.values.end() || it->second.type != FtpSessionValueType::int32_val ? 0 : it->second.item.ival;
                }
            else if
                constexpr(std::is_same_v<void *, T>)
                {
                    return it == context_.values.end() || it->second.type != FtpSessionValueType::ptr_val ? nullptr : it->second.item.ptr;
                }
            else if
                constexpr(std::is_same_v<double, T>)
                {
                    return it == context_.values.end() || it->second.type != FtpSessionValueType::double_val ? 0 : it->second.item.dval;
                }
            else if
                constexpr(std::is_same_v<std::string *, T>)
                {
                    return it == context_.values.end() || it->second.type != FtpSessionValueType::string_val ? nullptr : it->second.item.sval;
                }
            else {
                static_assert(std::is_same_v<T, void>, "invalid type for get_item_value");
            }
            return {};
        }

        void remove_item(const std::string &key)
        {
            auto it = context_.values.find(key);
            if (it != context_.values.end()) {
                if (it->second.type == FtpSessionValueType::string_val && it->second.item.sval) {
                    delete it->second.item.sval;
                }
                context_.values.erase(it);
            }
        }

        FileManager *get_file_manager()
        {
            return &context_.file_manager_;
        }
        bool set_work_file(FtpFileInfo *info);

    protected:
        WorkMode work_mode_;
        bool keep_util_sent_;
        bool close_;
        bool async_mode_;
        FtpSessionContext context_;
        FtpFileStream *pending_file_stream_cleanup_ = nullptr;
        std::unique_ptr<net::AsyncListenerHost> data_listener_;
        FtpFileInfo *pending_file_info_ = nullptr;
    };
}

#endif
