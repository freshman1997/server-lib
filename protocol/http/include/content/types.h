#ifndef __NET_HTTP_CONTENT_TYPES_H__
#define __NET_HTTP_CONTENT_TYPES_H__

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "nlohmann/json.hpp"
#include "content_type.h"

namespace yuan::net::http 
{
    // ============================================================
    // 内容基类（轻量级）
    // ============================================================
    struct ContentData
    {
        virtual ~ContentData() = default;
    };

    // ============================================================
    // 纯文本内容
    // ============================================================
    struct TextContent : ContentData
    {
        std::string data;  // 改为 string 存储，避免悬空指针
    };

    // ============================================================
    // JSON 内容
    // ============================================================
    struct JsonContent : ContentData
    {
        nlohmann::json value;
    };

    // ============================================================
    // 表单数据类型枚举
    // ============================================================
    enum class FormDataType { string_, file_ };

    // ============================================================
    // 表单项基类
    // ============================================================
    struct FormDataItem
    {
        FormDataType type;
        
        explicit FormDataItem(FormDataType t) : type(t) {}
        virtual ~FormDataItem() = default;

        // 禁止拷贝/移动（通过指针管理）
        FormDataItem(const FormDataItem&) = delete;
        FormDataItem& operator=(const FormDataItem&) = delete;
    };

    // ============================================================
    // 字符串表单项（普通 form 字段）
    // ============================================================
    struct FormDataStringItem : FormDataItem
    {
        std::string value;

        explicit FormDataStringItem(std::string val)
            : FormDataItem(FormDataType::string_), value(std::move(val)) {}
    };

    // ============================================================
    // 文件表单项（合并原 FileItem + StreamItem）
    //
    // 当 config::form_data_upload_save=true 时:
    //   数据保存到 tmp_file，begin/end 为空
    // 当 config::form_data_upload_save=false 时:
    //   数据保留在内存(buffer)中，begin/end 指向原始数据
    // ============================================================
    struct FormDataFileItem : FormDataItem
    {
        std::string origin_name;       // 原始文件名
        std::string content_type;      // MIME type (如 "image/png")
        std::string tmp_file;          // 临时文件路径（如果持久化到磁盘）

        // 内存模式下的数据指针（指向原始 body buffer）
        const char *data_begin = nullptr;
        const char *data_end   = nullptr;

        // 判断是否为内存模式（未落盘）
        bool is_in_memory() const noexcept { return data_begin != nullptr; }

        // 获取数据大小
        size_t size() const noexcept 
        {
            if (is_in_memory() && data_end >= data_begin) 
                return static_cast<size_t>(data_end - data_begin);
            return 0;  // 文件模式用 filesystem 查询
        }

        FormDataFileItem(std::string origin, std::string ctype,
                          const char *begin = nullptr, const char *end = nullptr,
                          std::string tmpPath = {})
            : FormDataItem(FormDataType::file_)
            , origin_name(std::move(origin))
            , content_type(std::move(ctype))
            , tmp_file(std::move(tmpPath))
            , data_begin(begin)
            , data_end(end)
        {}

        // 析构时清理临时文件
        ~FormDataFileItem() override
        {
            if (!tmp_file.empty()) {
                std::error_code ec;
                std::filesystem::remove(tmp_file, ec);
                // 静默失败即可
            }
        }
    };

    // ============================================================
    // Multipart Form Data 容器
    // name -> FormDataItem
    // ============================================================
    struct FormDataContent : ContentData
    {
        // 按 name 存储所有字段
        std::unordered_map<std::string, std::shared_ptr<FormDataItem>> fields;

        // 便捷访问：获取字符串字段值（不存在返回空串）
        std::string get_string(const std::string &name) const
        {
            auto it = fields.find(name);
            if (it != fields.end() && it->second->type == FormDataType::string_)
                return static_cast<FormDataStringItem*>(it->second.get())->value;
            return {};
        }

        // 便捷访问：获取文件项
        FormDataFileItem *get_file(const std::string &name) const
        {
            auto it = fields.find(name);
            if (it != fields.end() && it->second->type == FormDataType::file_)
                return static_cast<FormDataFileItem*>(it->second.get());
            return nullptr;
        }

        // 便捷访问：是否有某字段
        bool has(const std::string &name) const
        {
            return fields.find(name) != fields.end();
        }
    };

    // ============================================================
    // Range (用于 byte-range 请求，保持兼容性但简化)
    // ============================================================
    struct RangeChunk
    {
        std::string data;             // 直接存数据，避免悬空指针
        uint32_t from = 0;
        uint32_t to   = 0;
        uint32_t length = 0;
    };

    struct RangeDataContent : ContentData
    {
        std::vector<RangeChunk> chunks;
    };

    // ============================================================
    // Chunked 传输编码解析后的元数据
    // ============================================================
    struct ChunkedContent : ContentData
    {
        std::string tmp_file;          // 落盘文件路径（空表示纯内存）
        std::size_t total_bytes = 0;   // 解析完成后的总字节数
        std::string trailer_checksum;  // 可选的 trailer checksum
    };

    // ============================================================
    // 统一内容包装器
    // ============================================================
    struct Content
    {
        ContentType type = ContentType::not_support;
        std::unique_ptr<ContentData> data;  // unique_ptr 替代裸指针，RAII管理

        Content() = default;
        Content(ContentType t, ContentData *d) : type(t), data(d) {}
        
        // 移动语义
        Content(Content&&) noexcept = default;
        Content& operator=(Content&&) noexcept = default;

        // 禁止拷贝
        Content(const Content&) = delete;
        Content& operator=(const Content&) = delete;

        // 类型安全的获取
        template<typename T>
        T *as()
        {
            return dynamic_cast<T*>(data.get());
        }

        template<typename T>
        const T *as() const
        {
            return dynamic_cast<const T*>(data.get());
        }

        bool is_valid() const noexcept { return data != nullptr && type != ContentType::not_support; }
    };
}

#endif
