#include "net/bit_torrent/structure/bencoding.h"
#include <cctype>
#include <sstream>
#include <utility>

namespace net::bit_torrent 
{
    std::string ListData::to_string()
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

        return {p - begin + 1, new IntegerData(std::atoi(str.c_str()))};
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
        const char *from = p;
        
        for (int i = 0; i < len && p <= end; ++i, ++p);

        if (p - from < len) {
            return {-1, nullptr};
        }

        return {p - begin, new StringData(from, p)};
    }

    static std::pair<int, BaseData *> parse_list(const char *begin, const char *end)
    {
        const char * p = begin;
        ListData *list = new ListData;
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

        return {p - begin, list};
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

        return {p - begin, dict};
    }

    static std::pair<int, BaseData *> do_parse(const char *begin, const char *end)
    {
        const char *p = begin;
        char ch = *p;
        std::pair<int, BaseData *> res{-1, nullptr};
        if (std::isdigit(ch)) {
            res = parse_string(p, end);
        } else if (ch == 'i') {
            res = parse_integer(p + 1, end);
        } else if (ch == 'l') {
            res = parse_list(p + 1, end);
        } else if (ch == 'd') {
            res = parse_dictionary(p + 1, end);
        }

        return res;
    }
}