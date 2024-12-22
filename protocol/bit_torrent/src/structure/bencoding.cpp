#include <sstream>
#include "structure/bencoding.h"

namespace yuan::net::bit_torrent 
{
    std::string Listdata::to_string()
    {
        std::stringstream ss;
        ss << "[";
        
        int i = 0;
        for (const auto &it : datas_) {
            ss << it->to_string();
            if (i < datas_.size() - 1) {
                ss << ", ";
            }
            ++i;
        }

        ss << "]";

        return ss.str();
    }
    
    std::string DicttionaryData::to_string()
    {
        std::stringstream ss;
        ss << "{ \n";
        
        for (const auto &it : datas_) {
            ss << '"' << it.first << "\": " << it.second->to_string() << ", \n";
        }

        ss << '}';

        return ss.str();
    }
    
    BaseData * BencodingDataConverter::parse(const std::string &raw)
    {
        return parse(raw.c_str(), raw.c_str() + raw.size());
    }

    static std::pair<int, BaseData *> do_parse(const char *begin, const char *end);

    BaseData * BencodingDataConverter::parse(const char *begin, const char *end)
    {
        return do_parse(begin, end).second;
    }

    std::vector<BaseData *> BencodingDataConverter::parse_datas(const char *begin, const char *end)
    {
        std::vector<BaseData *> res;

        while (true) {
            const auto &block = do_parse(begin, end);
            if (block.first < 0) {
                break;
            }

            res.push_back(block.second);
            begin += block.first;
        }

        return res;
    }


    static std::pair<int, BaseData *> parse_integer(const char *begin, const char *end)
    {
        std::string str;
        const char * p = begin;
        for (; p <= end; ++p) {
            char ch = *p;
            if (ch == 'e') {
                break;
            }
            str.push_back(ch);
        }

        if (str.empty()) {
            return {-1, nullptr};
        }

        return {p - begin + 2, new IntegerData(std::atoi(str.c_str()))};
    }

    static std::pair<int, BaseData *> parse_string(const char *begin, const char *end)
    {
        std::string str;
        const char * p = begin;
        for (; p <= end; ++p) {
            char ch = *p;
            if (ch == ':') {
                ++p;
                break;
            }
            str.push_back(ch);
        }

        if (str.empty()) {
            return {-1, nullptr};
        }

        std::size_t len = std::atoi(str.c_str());
        str.clear();
        if (end - p < len) {
            return {-1, nullptr};
        }

        return {p - begin + len, new StringData(p, p + len)};
    }

    static std::pair<int, BaseData *> parse_list(const char *begin, const char *end)
    {
        const char * p = begin;
        Listdata *list = new Listdata;
        for (; p <= end; ) {
            char ch = *p;
            if (ch == 'e') {
                break;
            }

            const auto &res = do_parse(p, end);
            if (res.first < 0) {
                delete list;
                return {-1, nullptr};
            }
            
            p += res.first;
            list->push(res.second);
        }

        return {p - begin + 2, list};
    }

    static std::pair<int, BaseData *> parse_dictionary(const char *begin, const char *end)
    {
        const char * p = begin;
        DicttionaryData *dict = new DicttionaryData;
        for (; p <= end; ) {
            char ch = *p;
            if (ch == 'e') {
                break;
            }

            const auto &key = do_parse(p, end);
            if (key.first < 0 || key.second->type_ != DataType::string_) {
                delete dict;
                return {-1, nullptr};
            }
            
            p += key.first;

            const auto &val = do_parse(p, end);
            if (val.first < 0) {
                delete dict;
                return {-1, nullptr};
            }

            p += val.first;

            StringData *k = dynamic_cast<StringData *>(key.second);
            dict->add(k->get_data(), val.second);

            delete key.second;
        }

        return {p - begin + 2, dict};
    }

    static std::pair<int, BaseData *> do_parse(const char *begin, const char *end)
    {
        std::pair<int, BaseData *> res{-1, nullptr};

        char ch = *begin;
        if (std::isdigit(ch)) {
            res = parse_string(begin, end);
        } else if (ch == 'i') {
            res = parse_integer(begin + 1, end);
        } else if (ch == 'l') {
            res = parse_list(begin + 1, end);
        } else if (ch == 'd') {
            res = parse_dictionary(begin + 1, end);
        }

        return res;
    }
}