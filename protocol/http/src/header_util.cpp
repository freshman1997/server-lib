#include "header_util.h"
#include <unordered_map>

namespace yuan::net::http::helper
{
    std::unordered_map<std::string, ContentDispositionType> dispistion_type_mapping_ = {
        {"inline", ContentDispositionType::inline_},
        {"attachment", ContentDispositionType::attachment_},
        {"form-data", ContentDispositionType::form_data_},
    };

    const char * dispistion_type_names[] = {
        "inline",
        "attachment",
        "form-data"
    };

    const std::string filename_ = "filename";
    const std::string name_ = "name";

    bool str_cmp(const char *begin, const char *end, const char *str)
    {
        const char *p = begin;
        const char *p1 = str;
        for (;p != end && *p1; ++p, ++p1) {
            if (std::tolower(*p) != *p1) {
                return false;
            }
        }

        return p == end && !(*p1);
    }

    std::string read_identifier(const char *p, const char *end)
    {
        std::string id;
        bool quoted = false;
        for (; p != end; ++p) {
            const char ch = *p;
            if (ch == ' ') {
                continue;
            }

            if (ch == '\"') {
                if (quoted) break;
                quoted = true;
                continue;
            }

            if (ch == '=' || ch == ':' || ch == ';' || ch == '\r') {
                break;
            }

            id.push_back(std::tolower(ch));
        }

        return id;
    }

    uint32_t skip_new_line(const char *data)
    {
        const char ch = *data;
        if (ch == '\r') {
            return 2;
        }

        if (ch == '\n') {
            return 1;
        }

        return 0;
    }

    void read_next(const char *begin, const char *end, char ending, std::string &str)
    {
        while (begin <= end) {
            char ch = *begin;
            if (ch == ending) {
                break;
            }
            str.push_back(ch);
        }
    }

    ContentDispositionType get_content_disposition_type(const std::string &name)
    {
        const auto it = dispistion_type_mapping_.find(name);
        return it == dispistion_type_mapping_.end() ? ContentDispositionType::unknow_ : it->second;
    }

    std::string content_disposition_to_string(ContentDispositionType type)
    {
        if (type == ContentDispositionType::unknow_) {
            return {};
        }

        return dispistion_type_names[static_cast<std::size_t>(type)];
    }

    std::vector<std::pair<std::size_t, std::size_t>> parse_range(const std::string &range)
    {
        std::size_t i = 0;
        if (!str_cmp(range.c_str(), range.c_str() + 6, "bytes=")) {
            return {};
        }

        i = 6;

        std::vector<std::pair<std::size_t, std::size_t>> res;
        std::string from, to;
        bool next = false;
        for (; i < range.size(); ++i) {
            char ch = range[i];
            if (ch == ' ') {
                continue;
            }

            if (ch == ',') {
                if (from.empty() || to.empty()) {
                    return {};
                }

                std::size_t beg = std::atol(from.c_str());
                std::size_t end = std::atol(to.c_str());
                if (beg > end) {
                    return {};
                }

                res.emplace_back(beg, end);

                from.clear();
                to.clear();
                next = false;
                continue;
            }

            if (ch == '-') {
                next = true;
                continue;
            }

            if (next) {
                to.push_back(ch);
            } else {
                from.push_back(ch);
            }
        }
        
        if (!from.empty()) {
            res.emplace_back(std::atol(from.c_str()), std::atol(to.c_str()));
        }

        return res;
    }
};