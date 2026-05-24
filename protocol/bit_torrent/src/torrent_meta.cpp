#include "torrent_meta.h"
#include "utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace yuan::net::bit_torrent
{

    static const char *skip_bencode_value(const char *p, const char *end)
    {
        if (!p || p >= end) {
            return nullptr;
        }

        if (*p == 'i') {
            ++p;
            while (p < end && *p != 'e') {
                ++p;
            }
            return p < end ? p + 1 : nullptr;
        }

        if (*p == 'l' || *p == 'd') {
            ++p;
            while (p < end && *p != 'e') {
                p = skip_bencode_value(p, end);
                if (!p) {
                    return nullptr;
                }
            }
            return p < end ? p + 1 : nullptr;
        }

        if (std::isdigit(static_cast<unsigned char>(*p))) {
            const char *digit_start = p;
            while (p < end && std::isdigit(static_cast<unsigned char>(*p))) {
                ++p;
            }
            if (p >= end || *p != ':') {
                return nullptr;
            }

            int64_t str_len = 0;
            try {
                str_len = std::stoll(std::string(digit_start, p - digit_start));
            } catch (...) {
                return nullptr;
            }
            if (str_len < 0 || end - (p + 1) < str_len) {
                return nullptr;
            }
            return p + 1 + str_len;
        }

        return nullptr;
    }

    // Extract the top-level info dictionary's exact raw bencoded bytes.
    static std::string extract_info_bencode(const char *data, size_t len)
    {
        const char *p = data;
        const char *end = data + len;
        if (p >= end || *p != 'd') {
            return "";
        }

        ++p;
        while (p < end && *p != 'e') {
            if (!std::isdigit(static_cast<unsigned char>(*p))) {
                return "";
            }

            const char *key_start = p;
            const char *key_end = skip_bencode_value(p, end);
            if (!key_end) {
                return "";
            }

            std::string key_bencode(key_start, key_end - key_start);
            auto *key_node = BencodingDataConverter::parse(key_bencode);
            if (!key_node || key_node->type_ != DataType::string_) {
                delete key_node;
                return "";
            }
            const auto key = static_cast<StringData *>(key_node)->get_data();
            delete key_node;

            const char *value_start = key_end;
            const char *value_end = skip_bencode_value(value_start, end);
            if (!value_end) {
                return "";
            }

            if (key == "info") {
                return std::string(value_start, value_end - value_start);
            }

            p = value_end;
        }

        return "";
    }

    static bool validate_info(TorrentInfo &info)
    {
        if (info.piece_length_ <= 0 || info.total_length_ <= 0) {
            return false;
        }
        const auto count = info.piece_count();
        if (count <= 0 || info.pieces_.size() != static_cast<size_t>(count) * 20U) {
            return false;
        }
        return true;
    }

    TorrentMeta TorrentMeta::parse(const std::string & torrent_data)
    {
        TorrentMeta meta;

        BaseData *root = BencodingDataConverter::parse(torrent_data);
        if (!root || root->type_ != DataType::dictionary_)
            return meta;

        auto *dict = dynamic_cast<DicttionaryData *>(root);

        // announce
        if (auto *v = dict->get_val("announce"); v && v->type_ == DataType::string_)
            meta.announce_ = dynamic_cast<StringData *>(v)->get_data();

        // announce-list
        if (auto *v = dict->get_val("announce-list"); v && v->type_ == DataType::list_) {
            auto *list = dynamic_cast<Listdata *>(v);
            for (size_t i = 0; i < list->get_data().size(); i++) {
                auto *tier = list->get_data()[i];
                if (!tier || tier->type_ != DataType::list_)
                    continue;
                auto *tier_list = dynamic_cast<Listdata *>(tier);
                std::vector<std::string> tier_urls;
                for (size_t j = 0; j < tier_list->get_data().size(); j++) {
                    auto *url_node = tier_list->get_data()[j];
                    if (url_node && url_node->type_ == DataType::string_)
                        tier_urls.push_back(dynamic_cast<StringData *>(url_node)->get_data());
                }
                if (!tier_urls.empty())
                    meta.announce_list_.push_back(std::move(tier_urls));
            }
        }

        // comment
        if (auto *v = dict->get_val("comment"); v && v->type_ == DataType::string_)
            meta.comment_ = dynamic_cast<StringData *>(v)->get_data();

        // created by
        if (auto *v = dict->get_val("created by"); v && v->type_ == DataType::string_)
            meta.created_by_ = dynamic_cast<StringData *>(v)->get_data();

        // creation date
        if (auto *v = dict->get_val("creation date"); v && v->type_ == DataType::integer_)
            meta.creation_date_ = dynamic_cast<IntegerData *>(v)->get_data();

        // encoding
        if (auto *v = dict->get_val("encoding"); v && v->type_ == DataType::string_) {
            auto enc = dynamic_cast<StringData *>(v)->get_data();
            if (enc.find("UTF-8") != std::string::npos || enc.find("utf-8") != std::string::npos)
                meta.encoding_ = 0;
        }

        // info hash - extract raw bencoded info dict and SHA-1 it
        std::string info_bencode = extract_info_bencode(torrent_data.c_str(), torrent_data.size());
        if (!info_bencode.empty()) {
            meta.info_bencode_ = info_bencode;
            auto hash = sha1_hash(
                reinterpret_cast<const unsigned char *>(info_bencode.data()),
                info_bencode.size());
            meta.info_hash_ = hash;
            meta.info_hash_hex_ = to_hex(hash);
        }

        // info dictionary
        if (auto *v = dict->get_val("info"); v && v->type_ == DataType::dictionary_) {
            auto *info_dict = dynamic_cast<DicttionaryData *>(v);

            // name
            if (auto *nv = info_dict->get_val("name"); nv && nv->type_ == DataType::string_)
                meta.info.name_ = dynamic_cast<StringData *>(nv)->get_data();

            // piece length
            if (auto *nv = info_dict->get_val("piece length"); nv && nv->type_ == DataType::integer_)
                meta.info.piece_length_ = dynamic_cast<IntegerData *>(nv)->get_data();

            // pieces
            if (auto *nv = info_dict->get_val("pieces"); nv && nv->type_ == DataType::string_)
                meta.info.pieces_ = dynamic_cast<StringData *>(nv)->get_data();

            // private
            if (auto *nv = info_dict->get_val("private"); nv && nv->type_ == DataType::integer_)
                meta.info.private_ = dynamic_cast<IntegerData *>(nv)->get_data() != 0;

            // length (single-file mode)
            if (auto *nv = info_dict->get_val("length"); nv && nv->type_ == DataType::integer_)
                meta.info.total_length_ = dynamic_cast<IntegerData *>(nv)->get_data();

            // files (multi-file mode)
            if (auto *nv = info_dict->get_val("files"); nv && nv->type_ == DataType::list_) {
                auto *files_list = dynamic_cast<Listdata *>(nv);
                int64_t offset = 0;
                for (size_t i = 0; i < files_list->get_data().size(); i++) {
                    auto *file_node = files_list->get_data()[i];
                    if (!file_node || file_node->type_ != DataType::dictionary_)
                        continue;
                    auto *file_dict = dynamic_cast<DicttionaryData *>(file_node);

                    TorrentFile tf;
                    tf.offset_ = offset;

                    if (auto *lv = file_dict->get_val("length"); lv && lv->type_ == DataType::integer_)
                        tf.length_ = dynamic_cast<IntegerData *>(lv)->get_data();

                    if (auto *lv = file_dict->get_val("md5sum"); lv && lv->type_ == DataType::string_)
                        tf.md5sum_ = dynamic_cast<StringData *>(lv)->get_data();

                    if (auto *lv = file_dict->get_val("path"); lv && lv->type_ == DataType::list_) {
                        auto *path_list = dynamic_cast<Listdata *>(lv);
                        for (size_t j = 0; j < path_list->get_data().size(); j++) {
                            auto *p_node = path_list->get_data()[j];
                            if (p_node && p_node->type_ == DataType::string_)
                                tf.path_.push_back(dynamic_cast<StringData *>(p_node)->get_data());
                        }
                    }

                    meta.info.files_.push_back(tf);
                    offset += tf.length_;
                }

                // In multi-file mode, total_length is the sum
                meta.info.total_length_ = offset;
            }
        }

        // Fallback: if no announce-list, build one from announce
        if (meta.announce_list_.empty() && !meta.announce_.empty())
            meta.announce_list_.push_back({ meta.announce_ });

        if (!validate_info(meta.info)) {
            meta.info_hash_.clear();
            meta.info_hash_hex_.clear();
        }

        delete root;
        return meta;
    }

    TorrentMeta TorrentMeta::parse_file(const std::string & file_path)
    {
        std::ifstream ifs(file_path, std::ios::binary);
        if (!ifs.is_open())
            return TorrentMeta();

        std::ostringstream oss;
        oss << ifs.rdbuf();
        return parse(oss.str());
    }

} // namespace yuan::net::bit_torrent
