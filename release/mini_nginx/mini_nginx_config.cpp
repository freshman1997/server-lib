#include "mini_nginx_common.h"

namespace mini_nginx
{
    bool parse_header_map(const nlohmann::json &obj, std::vector<std::pair<std::string, std::string>> &headers)
    {
        if (!obj.is_object()) {
            std::cerr << "response headers must be an object\n";
            return false;
        }

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().is_string()) {
                std::cerr << "response header values must be strings\n";
                return false;
            }

            const auto name = it.key();
            if (name.empty() || name.find_first_of("\r\n:") != std::string::npos) {
                std::cerr << "response header name is invalid\n";
                return false;
            }

            headers.emplace_back(name, it.value().get<std::string>());
        }

        return true;
    }

    bool parse_proxy_redirects(const nlohmann::json &value, std::vector<yuan::net::http::ProxyRedirectRule> &redirects, const char *field_name)
    {
        auto append_one = [&](const nlohmann::json &item) {
            if (!item.is_object()) {
                std::cerr << field_name << " items must be objects\n";
                return false;
            }

            if (!item.contains("from") || !item["from"].is_string() ||
                !item.contains("to") || !item["to"].is_string()) {
                std::cerr << field_name << " items require from/to strings\n";
                return false;
            }

            yuan::net::http::ProxyRedirectRule rule;
            rule.from = item["from"].get<std::string>();
            rule.to = item["to"].get<std::string>();
            if (rule.from.empty() || rule.from.find_first_of("\r\n") != std::string::npos ||
                rule.to.find_first_of("\r\n") != std::string::npos) {
                std::cerr << field_name << " contains invalid values\n";
                return false;
            }

            redirects.push_back(std::move(rule));

            return true;
        };

        if (value.is_array()) {
            for (const auto &item : value) {
                if (!append_one(item)) {
                    return false;
                }
            }
            return true;
        }
        if (!value.is_object()) {
            std::cerr << field_name << " must be an object or array\n";
            return false;
        }
        if (value.contains("from") || value.contains("to")) {
            return append_one(value);
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!it.value().is_string()) {
                std::cerr << field_name << " map values must be strings\n";
                return false;
            }
            yuan::net::http::ProxyRedirectRule rule;
            rule.from = it.key();
            rule.to = it.value().get<std::string>();
            if (rule.from.empty() || rule.from.find_first_of("\r\n") != std::string::npos ||
                rule.to.find_first_of("\r\n") != std::string::npos) {
                std::cerr << field_name << " contains invalid values\n";
                return false;
            }
            redirects.push_back(std::move(rule));
        }
        return true;
    }

    bool parse_string_array(const nlohmann::json &arr, std::vector<std::string> &values, const char *field_name)
    {
        if (!arr.is_array()) {
            std::cerr << field_name << " must be an array\n";
            return false;
        }

        for (const auto &item : arr) {
            if (!item.is_string()) {
                std::cerr << field_name << " values must be strings\n";
                return false;
            }
            values.push_back(item.get<std::string>());
        }

        return true;
    }

    bool parse_string_or_array(const nlohmann::json &value, std::vector<std::string> &values, const char *field_name)
    {
        if (value.is_string()) {
            values.push_back(value.get<std::string>());
            return true;
        }
        return parse_string_array(value, values, field_name);
    }

    bool parse_http_version_value(const nlohmann::json &value, yuan::net::http::HttpVersion &version, const char *field_name)
    {
        std::string text;
        if (value.is_number_integer()) {
            text = std::to_string(value.get<int>());
        } else if (value.is_string()) {
            text = to_lower_ascii(trim_ascii(value.get<std::string>()));
        } else {
            std::cerr << field_name << " must be a string or integer\n";
            return false;
        }

        if (text == "1" || text == "1.0" || text == "http/1.0") {
            version = yuan::net::http::HttpVersion::v_1_0;
            return true;
        }

        if (text == "1.1" || text == "http/1.1") {
            version = yuan::net::http::HttpVersion::v_1_1;
            return true;
        }

        if (text == "2" || text == "2.0" || text == "h2" || text == "http/2" || text == "http/2.0") {
            version = yuan::net::http::HttpVersion::v_2_0;
            return true;
        }

        std::cerr << field_name << " must be one of 1.0, 1.1, or 2\n";

        return false;
    }

    bool parse_gzip_proxied(const nlohmann::json &value, std::unordered_set<std::string> &out, const char *field_name)
    {
        std::vector<std::string> values;
        if (!parse_string_or_array(value, values, field_name)) {
            return false;
        }

        static const std::unordered_set<std::string> allowed{
            "off", "any", "expired", "no-cache", "no-store", "private", "auth",
            "no_last_modified", "no_etag"};

        for (auto item : values) {
            item = to_lower_ascii(trim_ascii(item));
            if (item.empty()) {
                std::cerr << field_name << " values must not be empty\n";
                return false;
            }
            if (allowed.find(item) == allowed.end()) {
                std::cerr << field_name << " contains unsupported value: " << item << '\n';
                return false;
            }
            out.insert(std::move(item));
        }

        return true;
    }

    bool parse_method_set(const nlohmann::json &arr, std::unordered_set<std::string> &methods, const char *field_name)
    {
        if (!arr.is_array()) {
            std::cerr << field_name << " must be an array\n";
            return false;
        }

        for (const auto &item : arr) {
            if (!item.is_string()) {
                std::cerr << field_name << " values must be strings\n";
                return false;
            }
            const auto method = to_upper_ascii(item.get<std::string>());
            if (method.empty()) {
                std::cerr << field_name << " values must not be empty\n";
                return false;
            }
            methods.insert(method);
        }

        return true;
    }

    int tls_version_rank(const std::string &value)
    {
        auto normalized = to_lower_ascii(trim_ascii(value));
        normalized.erase(std::remove_if(normalized.begin(), normalized.end(), [](unsigned char ch) {
                             return std::isspace(ch) != 0 || ch == '_' || ch == '-';
                         }),
                         normalized.end());
        if (normalized == "tlsv1" || normalized == "tls1" || normalized == "1.0" || normalized == "1") {
            return 10;
        }
        if (normalized == "tlsv1.1" || normalized == "tls1.1" || normalized == "1.1") {
            return 11;
        }
        if (normalized == "tlsv1.2" || normalized == "tls1.2" || normalized == "1.2") {
            return 12;
        }
        if (normalized == "tlsv1.3" || normalized == "tls1.3" || normalized == "1.3") {
            return 13;
        }
        return -1;
    }

    std::string tls_version_name_from_rank(int rank)
    {
        switch (rank) {
        case 10:
            return "TLSv1";
        case 11:
            return "TLSv1.1";
        case 12:
            return "TLSv1.2";
        case 13:
            return "TLSv1.3";
        default:
            return {};
        }
    }

    bool parse_ssl_protocols(const nlohmann::json &value,
                             yuan::net::http::HttpServerConfig &server_config,
                             const char *field_name)
    {
        if (!value.is_array() || value.empty()) {
            std::cerr << field_name << " must be a non-empty array of TLS versions\n";
            return false;
        }
        int min_rank = std::numeric_limits<int>::max();
        int max_rank = std::numeric_limits<int>::min();
        for (const auto &item : value) {
            if (!item.is_string()) {
                std::cerr << field_name << " values must be strings\n";
                return false;
            }
            const int rank = tls_version_rank(item.get<std::string>());
            if (rank < 0) {
                std::cerr << field_name << " contains unsupported TLS version\n";
                return false;
            }
            min_rank = std::min(min_rank, rank);
            max_rank = std::max(max_rank, rank);
        }
        server_config.ssl_min_version = tls_version_name_from_rank(min_rank);
        server_config.ssl_max_version = tls_version_name_from_rank(max_rank);
        return true;
    }

    bool parse_access_values(const nlohmann::json &value,
                             bool allow,
                             std::vector<yuan::net::http::AccessRule> &rules,
                             const char *field_name)
    {
        auto append_one = [&](const nlohmann::json &item) {
            if (!item.is_string()) {
                std::cerr << field_name << " values must be strings\n";
                return false;
            }
            yuan::net::http::AccessRule rule;
            rule.allow = allow;
            rule.value = item.get<std::string>();
            if (trim_ascii(rule.value).empty()) {
                std::cerr << field_name << " values must not be empty\n";
                return false;
            }
            rules.push_back(std::move(rule));
            return true;
        };

        if (value.is_string()) {
            return append_one(value);
        }
        if (!value.is_array()) {
            std::cerr << field_name << " must be a string or array\n";
            return false;
        }
        for (const auto &item : value) {
            if (!append_one(item)) {
                return false;
            }
        }
        return true;
    }

    bool parse_access_rules(const nlohmann::json &json,
                            std::vector<yuan::net::http::AccessRule> &rules,
                            const char *field_name)
    {
        if (json.contains("access")) {
            if (!json["access"].is_array()) {
                std::cerr << field_name << ".access must be an array\n";
                return false;
            }
            for (const auto &item : json["access"]) {
                if (!item.is_object()) {
                    std::cerr << field_name << ".access items must be objects\n";
                    return false;
                }
                if (item.contains("allow")) {
                    if (!parse_access_values(item["allow"], true, rules, "access.allow")) {
                        return false;
                    }
                    continue;
                }
                if (item.contains("deny")) {
                    if (!parse_access_values(item["deny"], false, rules, "access.deny")) {
                        return false;
                    }
                    continue;
                }
                if (item.contains("action") && item.contains("value") &&
                    item["action"].is_string()) {
                    const auto action = to_lower_ascii(item["action"].get<std::string>());
                    if (action != "allow" && action != "deny") {
                        std::cerr << field_name << ".access action must be allow or deny\n";
                        return false;
                    }
                    if (!parse_access_values(item["value"], action == "allow", rules, "access.value")) {
                        return false;
                    }
                    continue;
                }
                std::cerr << field_name << ".access item must contain allow or deny\n";
                return false;
            }
        }
        if (json.contains("allow") && !parse_access_values(json["allow"], true, rules, "allow")) {
            return false;
        }
        if (json.contains("deny") && !parse_access_values(json["deny"], false, rules, "deny")) {
            return false;
        }
        return true;
    }

    bool parse_location_match(const nlohmann::json &json, bool fallback_exact)
    {
        bool exact = fallback_exact;
        if (json.contains("exact") && json["exact"].is_boolean() && json["exact"].get<bool>()) {
            exact = true;
        }
        auto apply_match = [&](const char *key) {
            if (!json.contains(key) || !json[key].is_string()) {
                return;
            }
            const auto match = to_lower_ascii(trim_ascii(json[key].get<std::string>()));
            if (match == "exact" || match == "=") {
                exact = true;
            } else if (match == "prefix" || match == "prefix_strong" || match == "^~") {
                exact = false;
            }
        };
        apply_match("match");
        apply_match("location_match");
        return exact;
    }

    void apply_static_location_match(const nlohmann::json &json,
                                     yuan::net::http::StaticMountOptions &options,
                                     bool fallback_exact)
    {
        options.exact_match = fallback_exact;
        options.match_type = fallback_exact
                                 ? yuan::net::http::StaticMountOptions::MatchType::exact
                                 : yuan::net::http::StaticMountOptions::MatchType::prefix;
        if (json.contains("exact") && json["exact"].is_boolean() && json["exact"].get<bool>()) {
            options.exact_match = true;
            options.match_type = yuan::net::http::StaticMountOptions::MatchType::exact;
        }
        auto apply_match = [&](const char *key) {
            if (!json.contains(key) || !json[key].is_string()) {
                return;
            }
            const auto match = to_lower_ascii(trim_ascii(json[key].get<std::string>()));
            if (match == "exact" || match == "=") {
                options.exact_match = true;
                options.match_type = yuan::net::http::StaticMountOptions::MatchType::exact;
            } else if (match == "prefix") {
                options.exact_match = false;
                options.match_type = yuan::net::http::StaticMountOptions::MatchType::prefix;
            } else if (match == "prefix_strong" || match == "^~") {
                options.exact_match = false;
                options.match_type = yuan::net::http::StaticMountOptions::MatchType::prefix_strong;
            } else if (match == "regex" || match == "~") {
                options.exact_match = false;
                options.match_type = yuan::net::http::StaticMountOptions::MatchType::regex;
                options.regex_case_sensitive = true;
            } else if (match == "regex_i" || match == "~*") {
                options.exact_match = false;
                options.match_type = yuan::net::http::StaticMountOptions::MatchType::regex;
                options.regex_case_sensitive = false;
            }
        };
        apply_match("match");
        apply_match("location_match");
    }

    bool valid_return_code(int code)
    {
        switch (code) {
        case 200:
        case 201:
        case 202:
        case 204:
        case 206:
        case 207:
        case 301:
        case 302:
        case 303:
        case 304:
        case 400:
        case 401:
        case 403:
        case 404:
        case 405:
        case 409:
        case 410:
        case 412:
        case 413:
        case 415:
        case 422:
        case 423:
        case 424:
        case 429:
        case 500:
        case 501:
        case 502:
        case 503:
        case 504:
        case 505:
        case 507:
            return true;
        default:
            return false;
        }
    }

    bool is_redirect_code(int code)
    {
        return code == 301 || code == 302 || code == 303;
    }

    bool parse_return_directive(const nlohmann::json &value,
                                const std::string &path,
                                bool exact,
                                ReturnRule &rule,
                                const char *field_name)
    {
        if (path.empty() || path.front() != '/') {
            std::cerr << field_name << " path must start with '/'\n";
            return false;
        }

        rule = ReturnRule{};
        rule.path = path;
        rule.exact = exact;

        if (value.is_number_integer()) {
            rule.code = value.get<int>();
        } else if (value.is_string()) {
            rule.code = 200;
            rule.body = value.get<std::string>();
        } else if (value.is_object()) {
            if (!value.contains("code") || !value["code"].is_number_integer()) {
                std::cerr << field_name << ".code integer is required\n";
                return false;
            }
            rule.code = value["code"].get<int>();
            auto copy_string = [&](const char *key, std::string &target) {
                if (value.contains(key) && value[key].is_string()) {
                    target = value[key].get<std::string>();
                }
            };
            copy_string("body", rule.body);
            copy_string("text", rule.body);
            copy_string("url", rule.location);
            copy_string("to", rule.location);
            copy_string("location", rule.location);
            copy_string("content_type", rule.content_type);
        } else {
            std::cerr << field_name << " must be an integer, string, or object\n";
            return false;
        }

        if (!valid_return_code(rule.code)) {
            std::cerr << field_name << ".code is not supported\n";
            return false;
        }
        if (rule.content_type.find_first_of("\r\n") != std::string::npos) {
            std::cerr << field_name << ".content_type is invalid\n";
            return false;
        }
        if (is_redirect_code(rule.code)) {
            if (rule.location.empty() || rule.location.find_first_of("\r\n") != std::string::npos) {
                std::cerr << field_name << " redirect requires url/to/location\n";
                return false;
            }
        }
        return true;
    }

    bool parse_return_rules_from_object(const nlohmann::json &json,
                                        const std::string &path,
                                        bool exact,
                                        std::vector<ReturnRule> &rules,
                                        const char *field_name)
    {
        if (!json.contains("return")) {
            return true;
        }
        ReturnRule rule;
        if (!parse_return_directive(json["return"], path, exact, rule, field_name)) {
            return false;
        }
        rules.push_back(std::move(rule));
        return true;
    }

    bool parse_rewrite_match_mode(const nlohmann::json &json,
                                  RewriteMatchMode fallback,
                                  RewriteMatchMode &mode,
                                  bool &case_sensitive)
    {
        mode = fallback;
        if (json.contains("exact") && json["exact"].is_boolean() && json["exact"].get<bool>()) {
            mode = RewriteMatchMode::exact;
        }
        if (json.contains("prefix") && json["prefix"].is_boolean() && json["prefix"].get<bool>()) {
            mode = RewriteMatchMode::prefix;
        }
        auto apply_match = [&](const char *key) {
            if (!json.contains(key) || !json[key].is_string()) {
                return true;
            }
            const auto match = to_lower_ascii(trim_ascii(json[key].get<std::string>()));
            if (match == "exact" || match == "=") {
                mode = RewriteMatchMode::exact;
            } else if (match == "prefix" || match == "prefix_strong" || match == "^~") {
                mode = RewriteMatchMode::prefix;
            } else if (match == "regex" || match == "~") {
                mode = RewriteMatchMode::regex;
                case_sensitive = true;
            } else if (match == "regex_i" || match == "~*") {
                mode = RewriteMatchMode::regex;
                case_sensitive = false;
            } else {
                std::cerr << "rewrite match must be prefix, exact, regex, ~, or ~*\n";
                return false;
            }
            return true;
        };
        return apply_match("match") && apply_match("location_match");
    }

    bool parse_rewrite_rule(const nlohmann::json &item,
                            const std::string &default_from,
                            bool default_exact,
                            RewriteRule &rule,
                            const char *field_name)
    {
        if (!item.is_object()) {
            std::cerr << field_name << " item must be object\n";
            return false;
        }

        rule = RewriteRule{};
        RewriteMatchMode fallback = default_exact ? RewriteMatchMode::exact : RewriteMatchMode::prefix;
        if (item.contains("regex") && item["regex"].is_string()) {
            rule.from = item["regex"].get<std::string>();
            fallback = RewriteMatchMode::regex;
        } else if (item.contains("pattern") && item["pattern"].is_string()) {
            rule.from = item["pattern"].get<std::string>();
            fallback = RewriteMatchMode::regex;
        } else if (item.contains("from") && item["from"].is_string()) {
            rule.from = item["from"].get<std::string>();
        } else if (item.contains("path") && item["path"].is_string()) {
            rule.from = item["path"].get<std::string>();
        } else if (item.contains("location") && item["location"].is_string()) {
            rule.from = item["location"].get<std::string>();
        } else {
            rule.from = default_from;
        }

        auto copy_replacement = [&](const char *key) {
            if (item.contains(key) && item[key].is_string()) {
                rule.to = item[key].get<std::string>();
                return true;
            }
            return false;
        };
        if (!copy_replacement("to") &&
            !copy_replacement("replacement") &&
            !copy_replacement("target") &&
            !copy_replacement("uri")) {
            std::cerr << field_name << " requires to/replacement/target/uri\n";
            return false;
        }

        if (rule.from.empty()) {
            std::cerr << field_name << " from/pattern must not be empty\n";
            return false;
        }
        if (rule.to.empty() || rule.to.find_first_of("\r\n") != std::string::npos) {
            std::cerr << field_name << " replacement is invalid\n";
            return false;
        }

        rule.case_sensitive = json_bool(item, "case_sensitive", rule.case_sensitive);
        if (!parse_rewrite_match_mode(item, fallback, rule.match, rule.case_sensitive)) {
            return false;
        }
        rule.preserve_query = json_bool(item, "preserve_query", rule.preserve_query);
        rule.preserve_path = json_bool(item, "preserve_path", rule.preserve_path);

        if (item.contains("code") && item["code"].is_number_integer()) {
            rule.code = item["code"].get<int>();
        }
        if (item.contains("redirect")) {
            if (item["redirect"].is_boolean() && item["redirect"].get<bool>() && rule.code == 0) {
                rule.code = 302;
            } else if (item["redirect"].is_number_integer()) {
                rule.code = item["redirect"].get<int>();
            }
        }
        if (json_bool(item, "permanent", false)) {
            rule.code = 301;
        }
        if (item.contains("flag") && item["flag"].is_string()) {
            const auto flag = to_lower_ascii(trim_ascii(item["flag"].get<std::string>()));
            if (flag == "redirect") {
                rule.code = 302;
            } else if (flag == "permanent") {
                rule.code = 301;
            } else if (flag == "last" || flag == "break") {
                rule.code = 0;
            } else {
                std::cerr << field_name << ".flag must be redirect, permanent, last, or break\n";
                return false;
            }
        }
        if (rule.code != 0 && !is_redirect_code(rule.code)) {
            std::cerr << field_name << ".code must be 301, 302, or 303 for redirects\n";
            return false;
        }
        if (rule.code == 0 && (rule.to.empty() || rule.to.front() != '/')) {
            std::cerr << field_name << " internal rewrite target must start with '/'\n";
            return false;
        }
        if (rule.match != RewriteMatchMode::regex && (rule.from.empty() || rule.from.front() != '/')) {
            std::cerr << field_name << " prefix/exact source must start with '/'\n";
            return false;
        }

        if (rule.match == RewriteMatchMode::regex) {
            try {
                auto flags = std::regex::ECMAScript;
                if (!rule.case_sensitive) {
                    flags |= std::regex::icase;
                }
                rule.compiled = std::regex(rule.from, flags);
            } catch (const std::regex_error &ex) {
                std::cerr << field_name << " regex is invalid: " << ex.what() << '\n';
                return false;
            }
        }
        return true;
    }

    bool parse_rewrite_array(const nlohmann::json &value,
                             const std::string &default_from,
                             bool default_exact,
                             std::vector<RewriteRule> &rules,
                             const char *field_name)
    {
        if (value.is_object()) {
            RewriteRule rule;
            if (!parse_rewrite_rule(value, default_from, default_exact, rule, field_name)) {
                return false;
            }
            rules.push_back(std::move(rule));
            return true;
        }
        if (!value.is_array()) {
            std::cerr << field_name << " must be an object or array\n";
            return false;
        }
        for (const auto &item : value) {
            RewriteRule rule;
            if (!parse_rewrite_rule(item, default_from, default_exact, rule, field_name)) {
                return false;
            }
            rules.push_back(std::move(rule));
        }
        return true;
    }

    bool parse_rewrite_rules_from_object(const nlohmann::json &json,
                                         const std::string &default_from,
                                         bool default_exact,
                                         std::vector<RewriteRule> &rules,
                                         const char *field_name)
    {
        if (json.contains("rewrites")) {
            if (!parse_rewrite_array(json["rewrites"], default_from, default_exact, rules, field_name)) {
                return false;
            }
        }
        if (json.contains("rewrite_rule")) {
            if (!parse_rewrite_array(json["rewrite_rule"], default_from, default_exact, rules, field_name)) {
                return false;
            }
        }
        if (json.contains("rewrite") && !json["rewrite"].is_string() && !json["rewrite"].is_boolean()) {
            if (!parse_rewrite_array(json["rewrite"], default_from, default_exact, rules, field_name)) {
                return false;
            }
        }
        return true;
    }

    bool parse_status_code_list(const std::string &text,
                                std::vector<int> &codes,
                                const char *field_name)
    {
        std::istringstream input(text);
        std::string token;
        while (input >> token) {
            try {
                const int code = std::stoi(token);
                if (!valid_return_code(code)) {
                    std::cerr << field_name << " contains unsupported status code\n";
                    return false;
                }
                codes.push_back(code);
            } catch (...) {
                std::cerr << field_name << " status code is invalid\n";
                return false;
            }
        }
        if (codes.empty()) {
            std::cerr << field_name << " requires at least one status code\n";
            return false;
        }
        return true;
    }

    bool parse_error_page_target(const nlohmann::json &value,
                                 yuan::net::http::StaticMountOptions::ErrorPage &page,
                                 const char *field_name)
    {
        page = {};
        if (value.is_string()) {
            std::string text = trim_ascii(value.get<std::string>());
            if (text.empty()) {
                std::cerr << field_name << " target must not be empty\n";
                return false;
            }
            if (text.front() == '=') {
                const auto space = text.find_first_of(" \t");
                if (space == std::string::npos) {
                    std::cerr << field_name << " =code target requires a URI\n";
                    return false;
                }
                try {
                    page.response_code = std::stoi(text.substr(1, space - 1));
                } catch (...) {
                    std::cerr << field_name << " =code is invalid\n";
                    return false;
                }
                text = trim_ascii(text.substr(space + 1));
            }
            page.uri = text;
        } else if (value.is_object()) {
            auto copy_uri = [&](const char *key) {
                if (value.contains(key) && value[key].is_string()) {
                    page.uri = value[key].get<std::string>();
                    return true;
                }
                return false;
            };
            if (!copy_uri("uri") && !copy_uri("path") && !copy_uri("file") && !copy_uri("to")) {
                std::cerr << field_name << " requires uri/path/file/to\n";
                return false;
            }
            if (value.contains("code") && value["code"].is_number_integer()) {
                page.response_code = value["code"].get<int>();
            }
            if (value.contains("response_code") && value["response_code"].is_number_integer()) {
                page.response_code = value["response_code"].get<int>();
            }
            if (value.contains("status") && value["status"].is_number_integer()) {
                page.response_code = value["status"].get<int>();
            }
        } else {
            std::cerr << field_name << " target must be a string or object\n";
            return false;
        }

        page.uri = trim_ascii(page.uri);
        if (page.uri.empty() || page.uri.front() != '/' || page.uri.find_first_of("\r\n") != std::string::npos) {
            std::cerr << field_name << " target URI must start with '/'\n";
            return false;
        }
        if (page.response_code != 0 && !valid_return_code(page.response_code)) {
            std::cerr << field_name << " response code is not supported\n";
            return false;
        }
        return true;
    }

    bool parse_error_page_object(const nlohmann::json &obj,
                                 std::unordered_map<int, yuan::net::http::StaticMountOptions::ErrorPage> &pages,
                                 const char *field_name)
    {
        if (!obj.is_object()) {
            std::cerr << field_name << " must be an object\n";
            return false;
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            std::vector<int> codes;
            if (!parse_status_code_list(it.key(), codes, field_name)) {
                return false;
            }
            yuan::net::http::StaticMountOptions::ErrorPage page;
            if (!parse_error_page_target(it.value(), page, field_name)) {
                return false;
            }
            for (const int code : codes) {
                pages[code] = page;
            }
        }
        return true;
    }

    bool parse_basic_auth_users(const nlohmann::json &value,
                                std::unordered_map<std::string, std::string> &users,
                                const char *field_name)
    {
        if (value.is_object()) {
            for (auto it = value.begin(); it != value.end(); ++it) {
                if (!it.value().is_string()) {
                    std::cerr << field_name << " user passwords must be strings\n";
                    return false;
                }
                if (it.key().empty()) {
                    std::cerr << field_name << " user names must not be empty\n";
                    return false;
                }
                users[it.key()] = it.value().get<std::string>();
            }
            return true;
        }
        if (!value.is_array()) {
            std::cerr << field_name << " must be an object or array\n";
            return false;
        }
        for (const auto &item : value) {
            if (!item.is_object() ||
                !item.contains("user") || !item["user"].is_string() ||
                !item.contains("password") || !item["password"].is_string()) {
                std::cerr << field_name << " array items require user/password strings\n";
                return false;
            }
            const auto user = item["user"].get<std::string>();
            if (user.empty()) {
                std::cerr << field_name << " user names must not be empty\n";
                return false;
            }
            users[user] = item["password"].get<std::string>();
        }
        return true;
    }

    bool parse_htpasswd_file(const std::string &path,
                             std::unordered_map<std::string, std::string> &users,
                             const char *field_name)
    {
        if (path.empty()) {
            std::cerr << field_name << " user file path must not be empty\n";
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            std::cerr << field_name << " cannot open user file: " << path << '\n';
            return false;
        }

        std::string line;
        std::size_t line_no = 0;
        while (std::getline(input, line)) {
            ++line_no;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            const auto trimmed = trim_ascii(line);
            if (trimmed.empty() || trimmed.front() == '#') {
                continue;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                std::cerr << field_name << " invalid user file line " << line_no << '\n';
                return false;
            }
            const std::string user = line.substr(0, colon);
            const std::string secret = line.substr(colon + 1);
            if (user.find_first_of("\r\n") != std::string::npos || secret.empty()) {
                std::cerr << field_name << " invalid user file line " << line_no << '\n';
                return false;
            }
            users[user] = secret;
        }
        return true;
    }

    bool parse_basic_auth_directive(const nlohmann::json &value,
                                    const std::string &path,
                                    bool exact,
                                    BasicAuthRule &rule,
                                    const char *field_name)
    {
        if (path.empty() || path.front() != '/') {
            std::cerr << field_name << " path must start with '/'\n";
            return false;
        }

        rule = BasicAuthRule{};
        rule.path = path;
        rule.exact = exact;

        if (value.is_boolean()) {
            rule.enabled = value.get<bool>();
        } else if (value.is_string()) {
            const auto text = trim_ascii(value.get<std::string>());
            if (to_lower_ascii(text) == "off") {
                rule.enabled = false;
            } else if (!text.empty()) {
                rule.realm = text;
            }
        } else if (value.is_object()) {
            if (value.contains("enabled") && value["enabled"].is_boolean()) {
                rule.enabled = value["enabled"].get<bool>();
            }
            if (value.contains("off") && value["off"].is_boolean() && value["off"].get<bool>()) {
                rule.enabled = false;
            }
            if (value.contains("realm") && value["realm"].is_string()) {
                rule.realm = value["realm"].get<std::string>();
            }
            if (value.contains("users") &&
                !parse_basic_auth_users(value["users"], rule.users, field_name)) {
                return false;
            }
            auto parse_user_file = [&](const char *key) {
                if (!value.contains(key)) {
                    return true;
                }
                if (!value[key].is_string()) {
                    std::cerr << field_name << "." << key << " must be a string\n";
                    return false;
                }
                return parse_htpasswd_file(value[key].get<std::string>(), rule.users, field_name);
            };
            if (!parse_user_file("auth_basic_user_file") ||
                !parse_user_file("basic_auth_user_file") ||
                !parse_user_file("user_file")) {
                return false;
            }
            if (value.contains("username") && value["username"].is_string() &&
                value.contains("password") && value["password"].is_string()) {
                rule.users[value["username"].get<std::string>()] = value["password"].get<std::string>();
            }
        } else {
            std::cerr << field_name << " must be bool, string, or object\n";
            return false;
        }

        if (rule.realm.empty() || rule.realm.find_first_of("\r\n\"") != std::string::npos) {
            std::cerr << field_name << " realm is invalid\n";
            return false;
        }
        if (rule.enabled && rule.users.empty()) {
            std::cerr << field_name << " requires at least one user\n";
            return false;
        }
        return true;
    }

    bool parse_basic_auth_rule_from_object(const nlohmann::json &json,
                                           const std::string &path,
                                           bool exact,
                                           std::vector<BasicAuthRule> &rules,
                                           const char *field_name)
    {
        const char *keys[] = {"auth_basic", "basic_auth", "auth"};
        for (const auto *key : keys) {
            if (!json.contains(key)) {
                continue;
            }
            nlohmann::json directive = json[key];
            if (directive.is_string() && to_lower_ascii(trim_ascii(directive.get<std::string>())) != "off") {
                nlohmann::json expanded = nlohmann::json::object();
                expanded["realm"] = directive;
                if (json.contains("auth_basic_users")) {
                    expanded["users"] = json["auth_basic_users"];
                } else if (json.contains("basic_auth_users")) {
                    expanded["users"] = json["basic_auth_users"];
                } else if (json.contains("users")) {
                    expanded["users"] = json["users"];
                }
                if (json.contains("auth_basic_user_file")) {
                    expanded["auth_basic_user_file"] = json["auth_basic_user_file"];
                } else if (json.contains("basic_auth_user_file")) {
                    expanded["basic_auth_user_file"] = json["basic_auth_user_file"];
                } else if (json.contains("user_file")) {
                    expanded["user_file"] = json["user_file"];
                }
                directive = std::move(expanded);
            }
            BasicAuthRule rule;
            if (!parse_basic_auth_directive(directive, path, exact, rule, field_name)) {
                return false;
            }
            rules.push_back(std::move(rule));
            return true;
        }
        return true;
    }

    bool parse_satisfy_directive(const nlohmann::json &json,
                                 SatisfyMode &mode,
                                 const char *field_name)
    {
        if (!json.contains("satisfy")) {
            return true;
        }
        if (!json["satisfy"].is_string()) {
            std::cerr << field_name << ".satisfy must be \"all\" or \"any\"\n";
            return false;
        }
        const auto value = to_lower_ascii(trim_ascii(json["satisfy"].get<std::string>()));
        if (value == "all") {
            mode = SatisfyMode::all;
            return true;
        }
        if (value == "any") {
            mode = SatisfyMode::any;
            return true;
        }
        std::cerr << field_name << ".satisfy must be \"all\" or \"any\"\n";
        return false;
    }

    bool parse_nginx_types_map(const nlohmann::json &obj,
                               std::unordered_map<std::string, std::string> &mime_types)
    {
        if (!obj.is_object()) {
            std::cerr << "static.types must be an object\n";
            return false;
        }

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const std::string mime = it.key();
            if (mime.empty() || mime.find_first_of("\r\n") != std::string::npos) {
                std::cerr << "static.types MIME name is invalid\n";
                return false;
            }

            auto add_ext = [&](const nlohmann::json &value) {
                if (!value.is_string()) {
                    std::cerr << "static.types extensions must be strings\n";
                    return false;
                }
                const std::string ext = normalize_extension(value.get<std::string>());
                if (ext.empty() || ext.find_first_of("\r\n") != std::string::npos) {
                    std::cerr << "static.types extension is invalid\n";
                    return false;
                }
                mime_types[ext] = mime;
                return true;
            };

            if (it.value().is_string()) {
                if (!add_ext(it.value())) {
                    return false;
                }
            } else if (it.value().is_array()) {
                for (const auto &ext : it.value()) {
                    if (!add_ext(ext)) {
                        return false;
                    }
                }
            } else {
                std::cerr << "static.types values must be strings or arrays\n";
                return false;
            }
        }
        return true;
    }

    bool parse_extension_mime_map(const nlohmann::json &obj,
                                  std::unordered_map<std::string, std::string> &mime_types)
    {
        if (!obj.is_object()) {
            std::cerr << "static.mime_types must be an object\n";
            return false;
        }

        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (!it.value().is_string()) {
                std::cerr << "static.mime_types values must be strings\n";
                return false;
            }
            const std::string ext = normalize_extension(it.key());
            const std::string mime = it.value().get<std::string>();
            if (ext.empty() || ext.find_first_of("\r\n") != std::string::npos ||
                mime.empty() || mime.find_first_of("\r\n") != std::string::npos) {
                std::cerr << "static.mime_types entry is invalid\n";
                return false;
            }
            mime_types[ext] = mime;
        }
        return true;
    }

    bool parse_expires_seconds(const nlohmann::json &value, int &seconds)
    {
        if (value.is_number_integer()) {
            seconds = value.get<int>();
            return seconds >= -1;
        }
        if (!value.is_string()) {
            return false;
        }

        std::string text = value.get<std::string>();
        if (text == "off") {
            seconds = -1;
            return true;
        }
        if (text.empty()) {
            return false;
        }

        int multiplier = 1;
        const char suffix = text.back();
        if (std::isalpha(static_cast<unsigned char>(suffix)) != 0) {
            text.pop_back();
            switch (suffix) {
            case 's':
                multiplier = 1;
                break;
            case 'm':
                multiplier = 60;
                break;
            case 'h':
                multiplier = 60 * 60;
                break;
            case 'd':
                multiplier = 24 * 60 * 60;
                break;
            case 'w':
                multiplier = 7 * 24 * 60 * 60;
                break;
            default:
                return false;
            }
        }
        if (text.empty()) {
            return false;
        }
        try {
            const int amount = std::stoi(text);
            if (amount < 0) {
                return false;
            }
            seconds = amount * multiplier;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_duration_ms(const nlohmann::json &value, int &milliseconds)
    {
        if (value.is_number_integer()) {
            milliseconds = value.get<int>();
            return milliseconds >= 0;
        }
        if (!value.is_string()) {
            return false;
        }

        std::string text = value.get<std::string>();
        if (text.empty()) {
            return false;
        }

        int multiplier = 1;
        if (text.size() >= 2 && text.substr(text.size() - 2) == "ms") {
            text.resize(text.size() - 2);
            multiplier = 1;
        } else if (std::isalpha(static_cast<unsigned char>(text.back())) != 0) {
            const char suffix = text.back();
            text.pop_back();
            switch (suffix) {
            case 's':
                multiplier = 1000;
                break;
            case 'm':
                multiplier = 60 * 1000;
                break;
            case 'h':
                multiplier = 60 * 60 * 1000;
                break;
            case 'd':
                multiplier = 24 * 60 * 60 * 1000;
                break;
            default:
                return false;
            }
        }
        if (text.empty()) {
            return false;
        }

        try {
            const long long amount = std::stoll(text);
            if (amount < 0 || amount > std::numeric_limits<int>::max() / multiplier) {
                return false;
            }
            milliseconds = static_cast<int>(amount * multiplier);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_byte_size(const nlohmann::json &value, std::size_t &bytes)
    {
        if (value.is_number_unsigned()) {
            bytes = value.get<std::size_t>();
            return true;
        }
        if (value.is_number_integer()) {
            const auto raw = value.get<long long>();
            if (raw < 0) {
                return false;
            }
            bytes = static_cast<std::size_t>(raw);
            return true;
        }
        if (!value.is_string()) {
            return false;
        }

        std::string text = value.get<std::string>();
        if (text.empty()) {
            return false;
        }
        std::uint64_t multiplier = 1;
        char suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
        if (suffix == 'b' && text.size() > 1) {
            text.pop_back();
            suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
        }
        if (suffix == 'k' || suffix == 'm' || suffix == 'g') {
            text.pop_back();
            if (suffix == 'k') {
                multiplier = 1024ull;
            } else if (suffix == 'm') {
                multiplier = 1024ull * 1024ull;
            } else {
                multiplier = 1024ull * 1024ull * 1024ull;
            }
        }
        if (text.empty()) {
            return false;
        }

        try {
            const auto amount = std::stoull(text);
            if (amount > std::numeric_limits<std::uint64_t>::max() / multiplier) {
                return false;
            }
            const auto raw = amount * multiplier;
            if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
                return false;
            }
            bytes = static_cast<std::size_t>(raw);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_proxy_pass_url(const std::string &proxy_pass,
                              std::string &host,
                              uint16_t &port,
                              std::string &path_prefix)
    {
        constexpr std::string_view http_prefix = "http://";
        if (proxy_pass.rfind(std::string(http_prefix), 0) != 0) {
            return false;
        }

        std::string rest = proxy_pass.substr(http_prefix.size());
        const auto slash_pos = rest.find('/');
        std::string authority = slash_pos == std::string::npos ? rest : rest.substr(0, slash_pos);
        path_prefix = slash_pos == std::string::npos ? "" : rest.substr(slash_pos);
        if (authority.empty()) {
            return false;
        }

        std::string port_text;
        if (!authority.empty() && authority.front() == '[') {
            const auto close = authority.find(']');
            if (close == std::string::npos) {
                return false;
            }
            host = authority.substr(1, close - 1);
            if (close + 1 < authority.size()) {
                if (authority[close + 1] != ':') {
                    return false;
                }
                port_text = authority.substr(close + 2);
            }
        } else {
            const auto colon = authority.rfind(':');
            if (colon == std::string::npos) {
                host = authority;
            } else {
                host = authority.substr(0, colon);
                port_text = authority.substr(colon + 1);
            }
        }

        if (host.empty()) {
            return false;
        }
        if (port_text.empty()) {
            port = 80;
            return true;
        }
        try {
            const auto parsed = std::stoul(port_text);
            if (parsed == 0 || parsed > 65535) {
                return false;
            }
            port = static_cast<uint16_t>(parsed);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_edge_features(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        cfg.response_headers.clear();
        cfg.redirects.clear();
        cfg.return_rules.clear();
        cfg.rewrite_rules.clear();
        cfg.basic_auth_rules.clear();
        cfg.allowed_methods.clear();

        if (json.contains("health")) {
            if (!json["health"].is_object()) {
                std::cerr << "config 'health' must be an object\n";
                return false;
            }
            const auto &health = json["health"];
            cfg.health_enabled = json_bool(health, "enabled", cfg.health_enabled);
            cfg.health_json = json_bool(health, "json", cfg.health_json);
            if (health.contains("path") && health["path"].is_string()) {
                cfg.health_path = health["path"].get<std::string>();
            }
            if (cfg.health_path.empty() || cfg.health_path.front() != '/') {
                std::cerr << "health.path must start with '/'\n";
                return false;
            }
        }

        if (json.contains("response_headers") &&
            !parse_header_map(json["response_headers"], cfg.response_headers)) {
            return false;
        }
        if (json.contains("headers")) {
            if (!json["headers"].is_object()) {
                std::cerr << "config 'headers' must be an object\n";
                return false;
            }
            if (json["headers"].contains("add")) {
                if (!parse_header_map(json["headers"]["add"], cfg.response_headers)) {
                    return false;
                }
            } else if (!parse_header_map(json["headers"], cfg.response_headers)) {
                return false;
            }
        }

        if (json.contains("allowed_methods") &&
            !parse_method_set(json["allowed_methods"], cfg.allowed_methods, "allowed_methods")) {
            return false;
        }
        if (!parse_satisfy_directive(json, cfg.satisfy_mode, "root")) {
            return false;
        }
        if (!parse_access_rules(json, cfg.access_rules, "root")) {
            return false;
        }
        if (!parse_basic_auth_rule_from_object(json, "/", false, cfg.basic_auth_rules, "auth_basic")) {
            return false;
        }
        if (json.contains("server") && json["server"].is_object() &&
            json["server"].contains("allowed_methods") &&
            !parse_method_set(json["server"]["allowed_methods"], cfg.allowed_methods, "allowed_methods")) {
            return false;
        }
        if (json.contains("server") && json["server"].is_object() &&
            !parse_satisfy_directive(json["server"], cfg.satisfy_mode, "server")) {
            return false;
        }
        if (json.contains("server") && json["server"].is_object() &&
            !parse_access_rules(json["server"], cfg.access_rules, "server")) {
            return false;
        }
        if (json.contains("server") && json["server"].is_object() &&
            !parse_basic_auth_rule_from_object(json["server"], "/", false, cfg.basic_auth_rules, "server.auth_basic")) {
            return false;
        }

        if (json.contains("redirects")) {
            if (!json["redirects"].is_array()) {
                std::cerr << "config 'redirects' must be an array\n";
                return false;
            }
            for (const auto &item : json["redirects"]) {
                if (!item.is_object()) {
                    std::cerr << "redirect item must be object\n";
                    return false;
                }
                if (!item.contains("from") || !item["from"].is_string() ||
                    !item.contains("to") || !item["to"].is_string()) {
                    std::cerr << "redirect.from and redirect.to are required strings\n";
                    return false;
                }
                RedirectRule rule;
                rule.from = item["from"].get<std::string>();
                rule.to = item["to"].get<std::string>();
                rule.code = json_int(item, "code", rule.code);
                rule.prefix = json_bool(item, "prefix", rule.prefix);
                rule.preserve_path = json_bool(item, "preserve_path", rule.preserve_path);
                if (rule.from.empty() || rule.from.front() != '/') {
                    std::cerr << "redirect.from must start with '/'\n";
                    return false;
                }
                if (rule.to.empty() || rule.to.find_first_of("\r\n") != std::string::npos) {
                    std::cerr << "redirect.to is invalid\n";
                    return false;
                }
                if (rule.code != 301 && rule.code != 302 && rule.code != 303) {
                    std::cerr << "redirect.code must be 301, 302, or 303\n";
                    return false;
                }
                cfg.redirects.push_back(std::move(rule));
            }
        }

        if (json.contains("returns")) {
            if (!json["returns"].is_array()) {
                std::cerr << "config 'returns' must be an array\n";
                return false;
            }
            for (const auto &item : json["returns"]) {
                if (!item.is_object()) {
                    std::cerr << "return item must be object\n";
                    return false;
                }
                std::string path;
                if (item.contains("path") && item["path"].is_string()) {
                    path = item["path"].get<std::string>();
                } else if (item.contains("location") && item["location"].is_string()) {
                    path = item["location"].get<std::string>();
                } else if (item.contains("from") && item["from"].is_string()) {
                    path = item["from"].get<std::string>();
                } else {
                    std::cerr << "return item requires path/location/from\n";
                    return false;
                }

                const bool exact = parse_location_match(item, json_bool(item, "prefix", false) ? false : true);
                nlohmann::json directive;
                if (item.contains("return")) {
                    directive = item["return"];
                } else {
                    directive = item;
                }
                ReturnRule rule;
                if (!parse_return_directive(directive, path, exact, rule, "returns")) {
                    return false;
                }
                cfg.return_rules.push_back(std::move(rule));
            }
        }

        if (json.contains("rewrites")) {
            if (!parse_rewrite_array(json["rewrites"], {}, false, cfg.rewrite_rules, "rewrites")) {
                return false;
            }
        }
        if (json.contains("rewrite_rule")) {
            if (!parse_rewrite_array(json["rewrite_rule"], {}, false, cfg.rewrite_rules, "rewrite_rule")) {
                return false;
            }
        }
        if (json.contains("rewrite") && !json["rewrite"].is_string() && !json["rewrite"].is_boolean()) {
            if (!parse_rewrite_array(json["rewrite"], {}, false, cfg.rewrite_rules, "rewrite")) {
                return false;
            }
        }

        return true;
    }

    bool parse_static_mounts(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        cfg.static_error_pages.clear();
        if (json.contains("error_page") &&
            !parse_error_page_object(json["error_page"], cfg.static_error_pages, "error_page")) {
            return false;
        }
        if (json.contains("error_pages") &&
            !parse_error_page_object(json["error_pages"], cfg.static_error_pages, "error_pages")) {
            return false;
        }

        if (!json.contains("static")) {
            return true;
        }
        if (!json["static"].is_array()) {
            std::cerr << "config 'static' must be an array\n";
            return false;
        }

        cfg.static_mounts.clear();
        for (const auto &item : json["static"]) {
            if (!item.is_object()) {
                std::cerr << "static item must be object\n";
                return false;
            }
            if (!item.contains("location") || !item["location"].is_string()) {
                std::cerr << "static.location(string) is required\n";
                return false;
            }
            const bool has_root = item.contains("root") && item["root"].is_string();
            const bool has_alias = item.contains("alias") && item["alias"].is_string();
            if (!has_root && !has_alias) {
                std::cerr << "static.root(string) or static.alias(string) is required\n";
                return false;
            }

            yuan::net::http::StaticMount mount;
            mount.options.error_pages = cfg.static_error_pages;
            mount.prefix = item["location"].get<std::string>();
            if (has_root) {
                mount.root = item["root"].get<std::string>();
            }
            if (has_alias) {
                mount.root = item["alias"].get<std::string>();
            }
            const bool static_regex_location = route_match_is_regex(item);
            if (mount.prefix.empty() || (mount.prefix.front() != '/' && !static_regex_location)) {
                std::cerr << (static_regex_location ? "static.location regex must not be empty\n"
                                                    : "static.location must start with '/'\n");
                return false;
            }
            if (mount.root.empty()) {
                std::cerr << "static.root must not be empty\n";
                return false;
            }

            apply_static_location_match(item, mount.options, false);
            if (!parse_return_rules_from_object(item, mount.prefix, mount.options.exact_match, cfg.return_rules, "static.return")) {
                return false;
            }
            if (!parse_rewrite_rules_from_object(item, mount.prefix, mount.options.exact_match, cfg.rewrite_rules, "static.rewrite")) {
                return false;
            }

            if (item.contains("auto_index") && item["auto_index"].is_boolean()) {
                mount.options.auto_index = item["auto_index"].get<bool>();
            }
            if (item.contains("autoindex") && item["autoindex"].is_boolean()) {
                mount.options.auto_index = item["autoindex"].get<bool>();
            }

            if (item.contains("enable_range") && item["enable_range"].is_boolean()) {
                mount.options.enable_range = item["enable_range"].get<bool>();
            }
            if (item.contains("gzip") && item["gzip"].is_boolean()) {
                mount.options.enable_gzip = item["gzip"].get<bool>();
            }
            if (item.contains("gzip_static") && item["gzip_static"].is_boolean()) {
                mount.options.enable_gzip_static = item["gzip_static"].get<bool>();
            }
            if (item.contains("gzip_vary") && item["gzip_vary"].is_boolean()) {
                mount.options.gzip_vary = item["gzip_vary"].get<bool>();
            }
            if (item.contains("gzip_min_length")) {
                std::size_t bytes = 0;
                if (!parse_byte_size(item["gzip_min_length"], bytes)) {
                    std::cerr << "static.gzip_min_length must be bytes or a size like 1k\n";
                    return false;
                }
                mount.options.gzip_min_length = bytes;
            }
            if (item.contains("gzip_comp_level")) {
                if (!item["gzip_comp_level"].is_number_integer()) {
                    std::cerr << "static.gzip_comp_level must be an integer in [1, 9]\n";
                    return false;
                }
                const int level = item["gzip_comp_level"].get<int>();
                if (level < 1 || level > 9) {
                    std::cerr << "static.gzip_comp_level must be in [1, 9]\n";
                    return false;
                }
                mount.options.gzip_comp_level = level;
            }
            if (item.contains("brotli_comp_level")) {
                if (!item["brotli_comp_level"].is_number_integer()) {
                    std::cerr << "static.brotli_comp_level must be an integer in [0, 11]\n";
                    return false;
                }
                const int level = item["brotli_comp_level"].get<int>();
                if (level < 0 || level > 11) {
                    std::cerr << "static.brotli_comp_level must be in [0, 11]\n";
                    return false;
                }
                mount.options.brotli_comp_level = level;
            }
            if (item.contains("gzip_http_version") &&
                !parse_http_version_value(item["gzip_http_version"], mount.options.gzip_http_version, "static.gzip_http_version")) {
                return false;
            }
            if (item.contains("gzip_disable")) {
                mount.options.gzip_disable.clear();
                if (!parse_string_or_array(item["gzip_disable"], mount.options.gzip_disable, "static.gzip_disable")) {
                    return false;
                }
                if (mount.options.gzip_disable.size() == 1 &&
                    to_lower_ascii(trim_ascii(mount.options.gzip_disable.front())) == "off") {
                    mount.options.gzip_disable.clear();
                }
                for (const auto &pattern : mount.options.gzip_disable) {
                    try {
                        (void)std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
                    } catch (const std::regex_error &ex) {
                        std::cerr << "static.gzip_disable contains invalid regex: " << ex.what() << '\n';
                        return false;
                    }
                }
            }
            if (item.contains("gzip_proxied")) {
                mount.options.gzip_proxied.clear();
                if (!parse_gzip_proxied(item["gzip_proxied"], mount.options.gzip_proxied, "static.gzip_proxied")) {
                    return false;
                }
            }
            if (item.contains("gzip_types")) {
                mount.options.gzip_types.clear();
                if (!parse_string_array(item["gzip_types"], mount.options.gzip_types, "gzip_types")) {
                    return false;
                }
            }
            if (item.contains("sendfile") && item["sendfile"].is_boolean()) {
                mount.options.enable_sendfile = item["sendfile"].get<bool>();
            }
            if (item.contains("default_type") && item["default_type"].is_string()) {
                mount.options.default_type = item["default_type"].get<std::string>();
            }
            if (item.contains("types") && !parse_nginx_types_map(item["types"], mount.options.mime_types)) {
                return false;
            }
            if (item.contains("mime_types") && !parse_extension_mime_map(item["mime_types"], mount.options.mime_types)) {
                return false;
            }
            if (item.contains("allowed_methods") &&
                !parse_method_set(item["allowed_methods"], mount.options.allowed_methods, "static.allowed_methods")) {
                return false;
            }
            if (item.contains("limit_except") &&
                !parse_method_set(item["limit_except"], mount.options.allowed_methods, "static.limit_except")) {
                return false;
            }
            if (!parse_access_rules(item, mount.options.access_rules, "static")) {
                return false;
            }
            if (!parse_basic_auth_rule_from_object(item, mount.prefix, mount.options.exact_match, cfg.basic_auth_rules, "static.auth_basic")) {
                return false;
            }
            if (item.contains("cache_control") && item["cache_control"].is_string()) {
                mount.options.cache_control = item["cache_control"].get<std::string>();
            }
            if (item.contains("expires")) {
                if (!parse_expires_seconds(item["expires"], mount.options.expires_seconds)) {
                    std::cerr << "static.expires must be integer seconds, 'off', or a duration like 1h/7d\n";
                    return false;
                }
            }
            if (item.contains("headers")) {
                if (!parse_header_map(item["headers"], mount.options.headers)) {
                    return false;
                }
            }
            if (item.contains("add_headers")) {
                if (!parse_header_map(item["add_headers"], mount.options.headers)) {
                    return false;
                }
            }
            if (item.contains("index") && item["index"].is_array()) {
                mount.options.index_files.clear();
                for (const auto &index_name : item["index"]) {
                    if (index_name.is_string()) {
                        mount.options.index_files.push_back(index_name.get<std::string>());
                    }
                }
                if (mount.options.index_files.empty()) {
                    mount.options.index_files = {"index.html", "index.htm"};
                }
            }

            if (item.contains("try_files") && item["try_files"].is_array()) {
                mount.options.try_files.clear();
                for (const auto &tf : item["try_files"]) {
                    if (tf.is_string()) {
                        mount.options.try_files.push_back(tf.get<std::string>());
                    }
                }
            }

            if (item.contains("error_page") &&
                !parse_error_page_object(item["error_page"], mount.options.error_pages, "static.error_page")) {
                return false;
            }
            if (item.contains("error_pages") &&
                !parse_error_page_object(item["error_pages"], mount.options.error_pages, "static.error_pages")) {
                return false;
            }

            cfg.static_mounts.push_back(std::move(mount));
        }
        return true;
    }

    void parse_listen_options(const nlohmann::json &server, MiniNginxConfig &cfg)
    {
        auto apply = [&](const nlohmann::json &options) {
            cfg.server_config.listen_options.reuse_addr =
                json_bool(options, "reuse_addr", cfg.server_config.listen_options.reuse_addr);
            cfg.server_config.listen_options.reuse_port =
                json_bool(options, "reuse_port", cfg.server_config.listen_options.reuse_port);
            cfg.server_config.listen_options.exclusive_addr =
                json_bool(options, "exclusive_addr", cfg.server_config.listen_options.exclusive_addr);
            cfg.server_config.listen_options.non_block =
                json_bool(options, "non_block", cfg.server_config.listen_options.non_block);
            cfg.server_config.listen_options.use_iocp =
                json_bool(options, "use_iocp", cfg.server_config.listen_options.use_iocp);
            cfg.server_config.listen_options.backlog =
                json_int(options, "backlog", cfg.server_config.listen_options.backlog);
            if (options.contains("iocp_worker_count") && options["iocp_worker_count"].is_number_unsigned()) {
                cfg.server_config.listen_options.iocp_worker_count =
                    options["iocp_worker_count"].get<std::size_t>();
            }
            if (options.contains("iocp_completion_batch_size") &&
                options["iocp_completion_batch_size"].is_number_unsigned()) {
                cfg.server_config.listen_options.iocp_completion_batch_size =
                    options["iocp_completion_batch_size"].get<std::size_t>();
            }
            if (options.contains("shard_count") && options["shard_count"].is_number_unsigned()) {
                cfg.server_config.listen_options.shard_count =
                    options["shard_count"].get<std::size_t>();
            }
            if (options.contains("scheduling_mode") && options["scheduling_mode"].is_string()) {
                cfg.server_config.listen_options.scheduling_mode =
                    parse_listen_scheduling_mode(options["scheduling_mode"].get<std::string>(),
                                                 cfg.server_config.listen_options.scheduling_mode);
            }
        };

        apply(server);
        if (server.contains("listen_options") && server["listen_options"].is_object()) {
            apply(server["listen_options"]);
        }
        if (cfg.server_config.listen_options.backlog < 1) {
            cfg.server_config.listen_options.backlog = 128;
        }
        if (cfg.server_config.listen_options.iocp_worker_count == 0) {
            cfg.server_config.listen_options.iocp_worker_count = 1;
        }
        if (cfg.server_config.listen_options.iocp_completion_batch_size == 0) {
            cfg.server_config.listen_options.iocp_completion_batch_size = 1;
        }
        if (cfg.server_config.listen_options.shard_count == 0) {
            cfg.server_config.listen_options.shard_count = 1;
        }
    }

    bool parse_server_config(const nlohmann::json &json, MiniNginxConfig &cfg)
    {
        if (json.contains("server")) {
            if (!json["server"].is_object()) {
                std::cerr << "config 'server' must be an object\n";
                return false;
            }

            const auto &server = json["server"];
            if (server.contains("listen") && server["listen"].is_number_integer()) {
                cfg.listen_port = server["listen"].get<int>();
            }
            if (server.contains("server_name") && server["server_name"].is_string()) {
                cfg.server_config.server_name = server["server_name"].get<std::string>();
            }
            if (server.contains("server_tokens") && server["server_tokens"].is_boolean()) {
                cfg.server_tokens = server["server_tokens"].get<bool>();
            }
            if (server.contains("server_header") && server["server_header"].is_string()) {
                cfg.server_header = server["server_header"].get<std::string>();
            }
            if (server.contains("thread_pool_size") && server["thread_pool_size"].is_number_integer()) {
                cfg.server_config.thread_pool_size = server["thread_pool_size"].get<int>();
            }
            if (server.contains("enable_keep_alive") && server["enable_keep_alive"].is_boolean()) {
                cfg.server_config.enable_keep_alive = server["enable_keep_alive"].get<bool>();
            }
            if (server.contains("enable_ssl") && server["enable_ssl"].is_boolean()) {
                cfg.server_config.enable_ssl = server["enable_ssl"].get<bool>();
            }
            if (server.contains("ssl_certificate") && server["ssl_certificate"].is_string()) {
                cfg.server_config.ssl_certificate = server["ssl_certificate"].get<std::string>();
            }
            if (server.contains("ssl_certificate_key") && server["ssl_certificate_key"].is_string()) {
                cfg.server_config.ssl_certificate_key = server["ssl_certificate_key"].get<std::string>();
            }
            if (server.contains("ssl_min_version") && server["ssl_min_version"].is_string()) {
                if (tls_version_rank(server["ssl_min_version"].get<std::string>()) < 0) {
                    std::cerr << "server.ssl_min_version is unsupported\n";
                    return false;
                }
                cfg.server_config.ssl_min_version = server["ssl_min_version"].get<std::string>();
            }
            if (server.contains("ssl_max_version") && server["ssl_max_version"].is_string()) {
                if (tls_version_rank(server["ssl_max_version"].get<std::string>()) < 0) {
                    std::cerr << "server.ssl_max_version is unsupported\n";
                    return false;
                }
                cfg.server_config.ssl_max_version = server["ssl_max_version"].get<std::string>();
            }
            if (server.contains("ssl_protocols") &&
                !parse_ssl_protocols(server["ssl_protocols"], cfg.server_config, "server.ssl_protocols")) {
                return false;
            }
            if (server.contains("ssl_ciphers") && server["ssl_ciphers"].is_string()) {
                cfg.server_config.ssl_ciphers = server["ssl_ciphers"].get<std::string>();
            }
            if (server.contains("ssl_ciphersuites") && server["ssl_ciphersuites"].is_string()) {
                cfg.server_config.ssl_ciphersuites = server["ssl_ciphersuites"].get<std::string>();
            }
            if (server.contains("ssl_prefer_server_ciphers") && server["ssl_prefer_server_ciphers"].is_boolean()) {
                cfg.server_config.ssl_prefer_server_ciphers = server["ssl_prefer_server_ciphers"].get<bool>();
            }
            if (server.contains("enable_cors") && server["enable_cors"].is_boolean()) {
                cfg.server_config.enable_cors = server["enable_cors"].get<bool>();
            }
            auto apply_body_size = [&](const char *key) {
                if (!server.contains(key)) {
                    return true;
                }
                std::size_t bytes = 0;
                if (!parse_byte_size(server[key], bytes)) {
                    std::cerr << "server." << key << " must be bytes or a size like 10m\n";
                    return false;
                }
                cfg.server_config.max_body_size = bytes;
                return true;
            };
            if (!apply_body_size("max_body_size") || !apply_body_size("client_max_body_size")) {
                return false;
            }
            auto apply_duration = [&](const char *key, int &target) {
                if (!server.contains(key)) {
                    return true;
                }
                int ms = 0;
                if (!parse_duration_ms(server[key], ms)) {
                    std::cerr << "server." << key << " must be milliseconds or a duration like 30s\n";
                    return false;
                }
                target = ms;
                return true;
            };
            if (!apply_duration("write_timeout_ms", cfg.server_config.write_timeout_ms) ||
                !apply_duration("send_timeout", cfg.server_config.write_timeout_ms)) {
                return false;
            }
            if (server.contains("keepalive_timeout") || server.contains("keep_alive_timeout")) {
                int keepalive_ms = 0;
                const char *key = server.contains("keepalive_timeout") ? "keepalive_timeout" : "keep_alive_timeout";
                if (!parse_duration_ms(server[key], keepalive_ms)) {
                    std::cerr << "server." << key << " must be milliseconds or a duration like 60s\n";
                    return false;
                }
                cfg.server_config.keep_alive_timeout_ms = keepalive_ms;
            }
            if (server.contains("enable_http2") && server["enable_http2"].is_boolean()) {
                cfg.server_config.enable_http2 = server["enable_http2"].get<bool>();
            }
            if (server.contains("http2_tls_only") && server["http2_tls_only"].is_boolean()) {
                cfg.server_config.http2_tls_only = server["http2_tls_only"].get<bool>();
            }
            if (server.contains("enable_h2c") && server["enable_h2c"].is_boolean()) {
                cfg.server_config.http2_tls_only = !server["enable_h2c"].get<bool>();
            }
            if (server.contains("max_connections") && server["max_connections"].is_number_integer()) {
                cfg.server_config.max_connections = server["max_connections"].get<int>();
            }
            if (server.contains("max_connections_per_ip") && server["max_connections_per_ip"].is_number_integer()) {
                cfg.server_config.max_connections_per_ip = server["max_connections_per_ip"].get<int>();
            }
            if (server.contains("max_inflight_requests_per_ip") && server["max_inflight_requests_per_ip"].is_number_integer()) {
                cfg.server_config.max_inflight_requests_per_ip = server["max_inflight_requests_per_ip"].get<int>();
            }
            if (server.contains("max_concurrent_requests_per_ip") && server["max_concurrent_requests_per_ip"].is_number_integer()) {
                cfg.server_config.max_inflight_requests_per_ip = server["max_concurrent_requests_per_ip"].get<int>();
            }
            if (server.contains("worker_processes") && server["worker_processes"].is_number_integer()) {
                cfg.worker_processes = server["worker_processes"].get<int>();
            }
            parse_listen_options(server, cfg);
        }

        if (json.contains("server_tokens") && json["server_tokens"].is_boolean()) {
            cfg.server_tokens = json["server_tokens"].get<bool>();
        }
        if (json.contains("server_header") && json["server_header"].is_string()) {
            cfg.server_header = json["server_header"].get<std::string>();
        }
        if (cfg.server_tokens &&
            (cfg.server_header.empty() || cfg.server_header.find_first_of("\r\n") != std::string::npos)) {
            std::cerr << "server_header is invalid\n";
            return false;
        }
        if (!cfg.server_config.ssl_min_version.empty() && !cfg.server_config.ssl_max_version.empty() &&
            tls_version_rank(cfg.server_config.ssl_min_version) > tls_version_rank(cfg.server_config.ssl_max_version)) {
            std::cerr << "server.ssl_min_version must be <= server.ssl_max_version\n";
            return false;
        }

        if (json.contains("access_log") && json["access_log"].is_object()) {
            const auto &access_log = json["access_log"];
            if (access_log.contains("enabled") && access_log["enabled"].is_boolean()) {
                cfg.access_log_enabled = access_log["enabled"].get<bool>();
            }
            if (access_log.contains("json") && access_log["json"].is_boolean()) {
                cfg.access_log_json = access_log["json"].get<bool>();
            }
            if (access_log.contains("path") && access_log["path"].is_string()) {
                cfg.access_log_path = access_log["path"].get<std::string>();
            }
            if (access_log.contains("format") && access_log["format"].is_string()) {
                cfg.access_log_format = access_log["format"].get<std::string>();
            }
        }
        if (json.contains("log_format") && json["log_format"].is_string()) {
            cfg.access_log_format = json["log_format"].get<std::string>();
        }

        if (json.contains("reload_check_interval_ms") && json["reload_check_interval_ms"].is_number_integer()) {
            cfg.reload_check_interval_ms = json["reload_check_interval_ms"].get<int>();
        }

        if (json.contains("rate_limit") && json["rate_limit"].is_object()) {
            const auto &rate_limit = json["rate_limit"];
            if (rate_limit.contains("enabled") && rate_limit["enabled"].is_boolean()) {
                cfg.rate_limit_enabled = rate_limit["enabled"].get<bool>();
            }
            if (rate_limit.contains("requests_per_second") && rate_limit["requests_per_second"].is_number_integer()) {
                cfg.rate_limit_rps = rate_limit["requests_per_second"].get<int>();
            }
            if (rate_limit.contains("burst") && rate_limit["burst"].is_number_integer()) {
                cfg.rate_limit_burst = rate_limit["burst"].get<int>();
            }
        }

        return true;
    }

    bool build_routes_from_upstreams(const nlohmann::json &json,
                                     std::vector<nlohmann::json> &routes,
                                     bool require_routes)
    {
        if (!json.contains("routes")) {
            if (require_routes) {
                std::cerr << "config 'routes' is required\n";
                return false;
            }
            return true;
        }
        if (!json["routes"].is_array()) {
            std::cerr << "config 'routes' must be an array\n";
            return false;
        }

        const nlohmann::json empty_upstreams = nlohmann::json::object();
        const auto &upstreams = (json.contains("upstreams") && json["upstreams"].is_object())
                                    ? json["upstreams"]
                                    : empty_upstreams;
        for (const auto &route_item : json["routes"]) {
            if (!route_item.is_object()) {
                std::cerr << "route item must be object\n";
                return false;
            }
            std::string route_path;
            if (route_item.contains("path") && route_item["path"].is_string()) {
                route_path = route_item["path"].get<std::string>();
            } else if (route_item.contains("location") && route_item["location"].is_string()) {
                route_path = route_item["location"].get<std::string>();
            }
            if (route_path.empty()) {
                std::cerr << "route.path(string) or route.location(string) is required\n";
                return false;
            }
            if (!route_item.contains("proxy_pass") || !route_item["proxy_pass"].is_string()) {
                std::cerr << "route.proxy_pass(string upstream name or http://host:port) is required\n";
                return false;
            }

            const std::string upstream_name = route_item["proxy_pass"].get<std::string>();
            nlohmann::json normalized = nlohmann::json::object();
            normalized["root"] = route_path;
            normalized["target"] = nlohmann::json::array();

            std::string direct_host;
            uint16_t direct_port = 0;
            std::string direct_path_prefix;
            const bool direct_proxy_pass = parse_proxy_pass_url(upstream_name, direct_host, direct_port, direct_path_prefix);

            const nlohmann::json *upstream = nullptr;
            if (!direct_proxy_pass) {
                if (!upstreams.contains(upstream_name) || !upstreams[upstream_name].is_object()) {
                    std::cerr << "undefined upstream: " << upstream_name << '\n';
                    return false;
                }
                upstream = &upstreams[upstream_name];
                if (!upstream->contains("servers") || !(*upstream)["servers"].is_array() || (*upstream)["servers"].empty()) {
                    std::cerr << "upstream '" << upstream_name << "' requires non-empty servers array\n";
                    return false;
                }
            }

            if (direct_proxy_pass) {
                nlohmann::json target = nlohmann::json::array();
                target.push_back(direct_host);
                target.push_back(direct_port);
                normalized["target"].push_back(std::move(target));
                if (!direct_path_prefix.empty() && direct_path_prefix != "/" &&
                    !route_item.contains("rewrite") && !route_item.contains("strip_prefix")) {
                    normalized["strip_prefix"] = true;
                    normalized["rewrite"] = direct_path_prefix;
                }
            } else {
                for (const auto &server : (*upstream)["servers"]) {
                    if (!server.is_object() ||
                        !server.contains("host") || !server["host"].is_string() ||
                        !server.contains("port") || !server["port"].is_number_unsigned()) {
                        std::cerr << "upstream server requires host(string) and port(unsigned)\n";
                        return false;
                    }
                    const auto port = server["port"].get<uint64_t>();
                    if (port == 0 || port > 65535) {
                        std::cerr << "upstream server port must be in range [1, 65535]\n";
                        return false;
                    }

                    nlohmann::json target = nlohmann::json::array();
                    target.push_back(server["host"].get<std::string>());
                    target.push_back(static_cast<uint16_t>(port));
                    if (server.contains("weight") && server["weight"].is_number_integer()) {
                        target.push_back(server["weight"].get<int>());
                    }
                    normalized["target"].push_back(target);
                }
            }

            auto copy_if_present = [&](const char *key) {
                if (route_item.contains(key)) {
                    normalized[key] = route_item[key];
                } else if (upstream && upstream->contains(key)) {
                    normalized[key] = (*upstream)[key];
                } else if (json.contains(key)) {
                    normalized[key] = json[key];
                }
            };

            copy_if_present("balance");
            copy_if_present("match");
            copy_if_present("location_match");
            copy_if_present("exact");
            copy_if_present("strip_prefix");
            if (route_item.contains("rewrite")) {
                normalized["rewrite"] = route_item["rewrite"];
            } else if (upstream && upstream->contains("rewrite")) {
                normalized["rewrite"] = (*upstream)["rewrite"];
            } else if (json.contains("rewrite") &&
                       (json["rewrite"].is_string() || json["rewrite"].is_boolean())) {
                normalized["rewrite"] = json["rewrite"];
            }
            if (route_item.contains("rewrites")) {
                normalized["rewrites"] = route_item["rewrites"];
            } else if (upstream && upstream->contains("rewrites")) {
                normalized["rewrites"] = (*upstream)["rewrites"];
            }
            if (route_item.contains("rewrite_rule")) {
                normalized["rewrite_rule"] = route_item["rewrite_rule"];
            } else if (upstream && upstream->contains("rewrite_rule")) {
                normalized["rewrite_rule"] = (*upstream)["rewrite_rule"];
            }
            copy_if_present("connect_timeout");
            copy_if_present("proxy_connect_timeout");
            copy_if_present("read_timeout");
            copy_if_present("proxy_read_timeout");
            copy_if_present("write_timeout");
            copy_if_present("proxy_send_timeout");
            copy_if_present("max_retries");
            copy_if_present("failure_threshold");
            copy_if_present("unhealthy_cooldown_ms");
            copy_if_present("pool_size");
            copy_if_present("idle_timeout");
            copy_if_present("proxy_cache");
            copy_if_present("proxy_cache_valid");
            copy_if_present("proxy_cache_ttl");
            copy_if_present("proxy_cache_max_size");
            copy_if_present("proxy_cache_methods");
            copy_if_present("proxy_cache_key");
            copy_if_present("proxy_cache_bypass_headers");
            copy_if_present("proxy_no_cache_headers");
            copy_if_present("proxy_cache_ignore_cache_control");
            copy_if_present("proxy_cache_ignore_set_cookie");
            copy_if_present("preserve_host");
            copy_if_present("proxy_preserve_host");
            copy_if_present("proxy_set_header");
            copy_if_present("proxy_headers");
            copy_if_present("hide_request_headers");
            copy_if_present("proxy_hide_request_headers");
            copy_if_present("proxy_set_response_header");
            copy_if_present("proxy_response_headers");
            copy_if_present("hide_response_headers");
            copy_if_present("proxy_hide_header");
            copy_if_present("proxy_redirect");
            copy_if_present("proxy_redirects");
            copy_if_present("allowed_methods");
            copy_if_present("limit_except");
            copy_if_present("allow");
            copy_if_present("deny");
            copy_if_present("access");
            if (route_item.contains("auth_basic")) {
                normalized["auth_basic"] = route_item["auth_basic"];
            } else if (upstream && upstream->contains("auth_basic")) {
                normalized["auth_basic"] = (*upstream)["auth_basic"];
            }
            if (route_item.contains("basic_auth")) {
                normalized["basic_auth"] = route_item["basic_auth"];
            } else if (upstream && upstream->contains("basic_auth")) {
                normalized["basic_auth"] = (*upstream)["basic_auth"];
            }
            if (route_item.contains("auth")) {
                normalized["auth"] = route_item["auth"];
            } else if (upstream && upstream->contains("auth")) {
                normalized["auth"] = (*upstream)["auth"];
            }
            copy_if_present("return");

            std::string error;
            if (!validate_route(normalized, error)) {
                std::cerr << "invalid normalized route: " << error << '\n';
                return false;
            }
            routes.push_back(std::move(normalized));
        }

        return true;
    }

    bool parse_route_return_rules(const std::vector<nlohmann::json> &routes,
                                  std::vector<ReturnRule> &rules)
    {
        for (const auto &route : routes) {
            if (!route.is_object() || !route.contains("root") || !route["root"].is_string()) {
                continue;
            }
            const bool exact = parse_location_match(route, false);
            if (!parse_return_rules_from_object(route, route["root"].get<std::string>(), exact, rules, "route.return")) {
                return false;
            }
        }
        return true;
    }

    bool parse_route_rewrite_rules(const std::vector<nlohmann::json> &routes,
                                   std::vector<RewriteRule> &rules)
    {
        for (const auto &route : routes) {
            if (!route.is_object() || !route.contains("root") || !route["root"].is_string()) {
                continue;
            }
            const bool exact = parse_location_match(route, false);
            if (!parse_rewrite_rules_from_object(route, route["root"].get<std::string>(), exact, rules, "route.rewrite")) {
                return false;
            }
        }
        return true;
    }

    bool parse_route_basic_auth_rules(const std::vector<nlohmann::json> &routes,
                                      std::vector<BasicAuthRule> &rules)
    {
        for (const auto &route : routes) {
            if (!route.is_object() || !route.contains("root") || !route["root"].is_string()) {
                continue;
            }
            const bool exact = parse_location_match(route, false);
            if (!parse_basic_auth_rule_from_object(route, route["root"].get<std::string>(), exact, rules, "route.auth_basic")) {
                return false;
            }
        }
        return true;
    }

    bool path_matches_return_rule(const std::string &path, const ReturnRule &rule)
    {
        if (rule.exact) {
            return path == rule.path;
        }
        return path.rfind(rule.path, 0) == 0;
    }

    bool path_matches_basic_auth_rule(const std::string &path, const BasicAuthRule &rule)
    {
        if (rule.exact) {
            return path == rule.path;
        }
        return path.rfind(rule.path, 0) == 0;
    }

    const BasicAuthRule *select_basic_auth_rule(const std::string &path,
                                                const std::vector<BasicAuthRule> &rules)
    {
        const BasicAuthRule *selected = nullptr;
        std::size_t selected_len = 0;
        for (const auto &rule : rules) {
            if (!path_matches_basic_auth_rule(path, rule)) {
                continue;
            }
            if (!selected || rule.path.size() >= selected_len) {
                selected = &rule;
                selected_len = rule.path.size();
            }
        }
        return selected;
    }

    bool constant_time_equal(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size()) {
            return false;
        }
        unsigned char diff = 0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
        }
        return diff == 0;
    }

    std::string sha1_base64(std::string_view value)
    {
        unsigned char digest[SHA_DIGEST_LENGTH] = {};
        SHA1(reinterpret_cast<const unsigned char *>(value.data()), value.size(), digest);
        return yuan::base::util::base64_encode(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(digest),
            SHA_DIGEST_LENGTH));
    }

    bool basic_auth_password_matches(std::string_view stored, std::string_view provided)
    {
        if (stored.rfind("{PLAIN}", 0) == 0) {
            return constant_time_equal(stored.substr(7), provided);
        }
        if (stored.rfind("{SHA}", 0) == 0) {
            return constant_time_equal(stored.substr(5), sha1_base64(provided));
        }
        if (stored.rfind("{SSHA}", 0) == 0 ||
            stored.rfind("$apr1$", 0) == 0 ||
            stored.rfind("$1$", 0) == 0 ||
            stored.rfind("$2a$", 0) == 0 ||
            stored.rfind("$2b$", 0) == 0 ||
            stored.rfind("$2y$", 0) == 0) {
            return false;
        }
        return constant_time_equal(stored, provided);
    }

    bool basic_auth_allowed(yuan::net::http::HttpRequest *req,
                            const BasicAuthRule &rule)
    {
        if (!req) {
            return false;
        }
        const std::string *auth = req->get_header(yuan::net::http::http_header_key::authorization);
        if (!auth || auth->empty()) {
            return false;
        }
        const auto parsed = yuan::net::http::HttpAuthorization::parse(*auth);
        if (parsed.type != yuan::net::http::authorization_type::basic) {
            return false;
        }
        const auto it = rule.users.find(parsed.username);
        return it != rule.users.end() && basic_auth_password_matches(it->second, parsed.password);
    }

    void send_basic_auth_challenge(yuan::net::http::HttpRequest *req,
                                   yuan::net::http::HttpResponse *resp,
                                   const BasicAuthRule &rule)
    {
        const std::string body = "401 Unauthorized\n";
        resp->set_response_code(yuan::net::http::ResponseCode::unauthorized);
        resp->add_header("WWW-Authenticate", "Basic realm=\"" + rule.realm + "\"");
        resp->add_header("Content-Type", "text/plain; charset=utf-8");
        if (!(req && req->is_head())) {
            resp->append_body(body);
        }
        resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
    }

    std::string append_query_if_needed(std::string target,
                                       std::string_view query,
                                       bool preserve_query)
    {
        if (!preserve_query || query.empty() || target.find('?') != std::string::npos) {
            return target;
        }
        target.push_back('?');
        target.append(query.data(), query.size());
        return target;
    }

    bool rewrite_rule_matches(const std::string &path,
                              const RewriteRule &rule,
                              std::string &target)
    {
        switch (rule.match) {
        case RewriteMatchMode::exact:
            if (path != rule.from) {
                return false;
            }
            target = rule.to;
            return true;
        case RewriteMatchMode::prefix:
            if (path.rfind(rule.from, 0) != 0) {
                return false;
            }
            target = rule.to;
            if (rule.preserve_path && path.size() > rule.from.size()) {
                if (!target.empty() && target.back() == '/' && path[rule.from.size()] == '/') {
                    target.pop_back();
                }
                target.append(path.substr(rule.from.size()));
            }
            return true;
        case RewriteMatchMode::regex:
            if (!std::regex_search(path, rule.compiled)) {
                return false;
            }
            target = std::regex_replace(path, rule.compiled, rule.to, std::regex_constants::format_first_only);
            return true;
        }
        return false;
    }

    bool apply_rewrite_rules(yuan::net::http::HttpRequest *req,
                             yuan::net::http::HttpResponse *resp,
                             const std::vector<RewriteRule> &rules)
    {
        if (!req || !resp || rules.empty()) {
            return false;
        }

        for (std::size_t pass = 0; pass < 10; ++pass) {
            const auto path_view = req->get_path();
            const auto query_view = req->get_query_string();
            const std::string path(path_view.data(), path_view.size());

            bool changed = false;
            for (const auto &rule : rules) {
                std::string target;
                if (!rewrite_rule_matches(path, rule, target)) {
                    continue;
                }
                target = append_query_if_needed(std::move(target), query_view, rule.preserve_query);
                if (is_redirect_code(rule.code)) {
                    resp->redirect(target, static_cast<yuan::net::http::ResponseCode>(rule.code));
                    resp->add_header("Content-Length", "0");
                    return true;
                }
                if (target == req->get_raw_url()) {
                    return false;
                }
                req->set_raw_url(std::move(target));
                changed = true;
                break;
            }
            if (!changed) {
                return false;
            }
        }

        resp->process_error(yuan::net::http::ResponseCode::internal_server_error);
        return true;
    }

    void send_return_response(yuan::net::http::HttpRequest *req,
                              yuan::net::http::HttpResponse *resp,
                              const ReturnRule &rule)
    {
        if (is_redirect_code(rule.code)) {
            resp->redirect(rule.location, static_cast<yuan::net::http::ResponseCode>(rule.code));
            resp->add_header("Content-Length", "0");
            return;
        }

        resp->set_response_code(static_cast<yuan::net::http::ResponseCode>(rule.code));
        const bool suppress_body = rule.code == 204 || (req && req->is_head());
        if (!rule.content_type.empty()) {
            resp->add_header("Content-Type", rule.content_type);
        }
        if (!suppress_body && !rule.body.empty()) {
            resp->append_body(rule.body);
        }

        resp->add_header("Content-Length", std::to_string(resp->body_buffer_size()));
    }

    bool validate_loaded_config(const MiniNginxConfig &cfg)
    {
        if (cfg.routes.empty() && cfg.static_mounts.empty()) {
            std::cerr << "at least one route or static mount is required\n";
            return false;
        }

        if (cfg.listen_port <= 0 || cfg.listen_port > 65535) {
            std::cerr << "server.listen must be in range [1, 65535]\n";
            return false;
        }

        return true;
    }

    bool load_and_apply_config(const std::string &path, MiniNginxConfig &cfg)
    {
        std::ifstream input(path);
        if (!input.good()) {
            std::cerr << "failed to open config file: " << path << '\n';
            return false;
        }

        nlohmann::json json;
        try {
            input >> json;
        } catch (const std::exception &ex) {
            std::cerr << "failed to parse config json: " << ex.what() << '\n';
            return false;
        }

        if (!parse_server_config(json, cfg)) {
            return false;
        }
        if (!parse_edge_features(json, cfg)) {
            return false;
        }

        cfg.routes.clear();
        if (!parse_static_mounts(json, cfg)) {
            return false;
        }

        const bool require_routes = cfg.static_mounts.empty();
        if (!build_routes_from_upstreams(json, cfg.routes, require_routes)) {
            return false;
        }
        if (!parse_route_return_rules(cfg.routes, cfg.return_rules)) {
            return false;
        }
        if (!parse_route_rewrite_rules(cfg.routes, cfg.rewrite_rules)) {
            return false;
        }
        if (!parse_route_basic_auth_rules(cfg.routes, cfg.basic_auth_rules)) {
            return false;
        }

        return validate_loaded_config(cfg);
    }
}
