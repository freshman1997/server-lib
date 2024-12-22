#ifndef __NET_BIT_TORRENT_STRUCTURE_BENCODING_H__
#define __NET_BIT_TORRENT_STRUCTURE_BENCODING_H__
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace yuan::net::bit_torrent 
{
    enum class DataType
    {
        invalid_ = -1,
        integer_,
        string_,
        list_,
        dictionary_
    };

    class BaseData
    {
    public:
        virtual ~BaseData() {}

        virtual std::string to_string() = 0;

    public:
        DataType type_ = DataType::invalid_;
    };

    class IntegerData : public BaseData
    {
    public:
        IntegerData(int32_t data) : data_(data) 
        {
            type_ = DataType::integer_;
        }

        int32_t get_data() const 
        {
            return data_;
        }

        std::string to_string()
        {
            return std::to_string(data_);
        }

    private:
        int32_t data_;
    };

    class StringData : public BaseData
    {
    public:
        StringData(const std::string &data)
        {
            data_ = std::move(data);
            type_ = DataType::string_;
        }
        
        StringData(const char *begin, const char *end)
        {
            data_ = std::move(std::string(begin, end));
            type_ = DataType::string_;
        }

        const std::string & get_data() const 
        {
            return data_;
        }

        void set_Data(const std::string &data)
        {
            data_ = std::move(data);
        }

        void set_Data(const char *begin, const char *end)
        {
            data_ = std::move(std::string(begin, end));
        }

        std::string to_string()
        {
            return data_;
        }

    private:
        std::string data_;
    };

    class Listdata : public BaseData
    {
    public:
        Listdata()
        {
            type_ = DataType::list_;
        }

        ~Listdata()
        {
            for (const auto &it : datas_) {
                delete it;
            }
        }

        void push(BaseData *data)
        {
            datas_.push_back(data);
        }

        BaseData * get_data(uint32_t idx) 
        {
            return idx >= datas_.size() ? nullptr : datas_[idx];
        }

        std::vector<BaseData *> get_data() const 
        {
            return datas_;
        }

        std::string to_string();

    private:
        std::vector<BaseData *> datas_;
    };

    class DicttionaryData : public BaseData
    {
    public:
        DicttionaryData()
        {
            type_ = DataType::dictionary_;
        }

        ~DicttionaryData()
        {
            for (const auto &it : datas_) {
                delete it.second;
            }
        }

        void add(const std::string &key, BaseData *val)
        {
            datas_[key] = val;
        }

        bool has(const std::string &k) 
        {
            return datas_.count(k);
        }

        BaseData * get_val(const std::string &k)
        {
            auto it = datas_.find(k);
            return it == datas_.end() ? nullptr : it->second;
        }

        std::string to_string();

    private:
        std::unordered_map<std::string, BaseData *> datas_;
    };

    class BencodingDataConverter
    {
    public:
        // 单个文件
        static BaseData * parse(const std::string &raw);
        static BaseData * parse(const char *begin, const char *end);

        // 多个文件
        static std::vector<BaseData *> parse_datas(const char *begin, const char *end);
    };
}

#endif