#include "torrent_meta.h"
#include "utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace yuan::net::bit_torrent
{

// Bencode the info dictionary for hashing
static std::string bencode_info(DicttionaryData *info_dict)
{
    // Re-bencode the info dict to compute the info_hash
    // We need to serialize the original bencoded data of the info key
    // For now, we'll do a simple re-encoding
    return ""; // handled differently - see parse()
}

// Extract the info dictionary's raw bencoded bytes from torrent data
static std::string extract_info_bencode(const char *data, size_t len)
{
    // Find "info" key in the top-level dictionary
    // Format: d...4:info<info_dict>...e
    const std::string key = "4:info";
    auto pos = std::search(data, data + len, key.begin(), key.end());
    if (pos == data + len) return "";

    const char *info_start = pos + key.size();

    // Recursively find the matching 'e' for the info dict/list
    int depth = 0;
    const char *p = info_start;
    while (p < data + len)
    {
        char ch = *p;
        if (ch == 'd' || ch == 'l') { depth++; p++; continue; }
        if (ch == 'i')
        {
            p++; // skip 'i'
            while (p < data + len && *p != 'e') p++;
            p++; // skip 'e'
            continue;
        }
        if (ch == 'e')
        {
            if (depth == 0) break;
            depth--;
            p++;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)))
        {
            // bencode string: <digits>:<data>
            const char *digit_start = p;
            while (p < data + len && std::isdigit(static_cast<unsigned char>(*p))) p++;
            if (p >= data + len || *p != ':') return "";
            p++; // skip ':'

            std::string len_str(digit_start, p - 1);
            int64_t str_len = 0;
            try { str_len = std::stoll(len_str); } catch (...) { return ""; }
            p += str_len;
            continue;
        }
        p++;
    }

    return std::string(info_start, p - info_start);
}

TorrentMeta TorrentMeta::parse(const std::string &torrent_data)
{
    TorrentMeta meta;

    BaseData *root = BencodingDataConverter::parse(torrent_data);
    if (!root || root->type_ != DataType::dictionary_) return meta;

    auto *dict = dynamic_cast<DicttionaryData *>(root);

    // announce
    if (auto *v = dict->get_val("announce"); v && v->type_ == DataType::string_)
        meta.announce_ = dynamic_cast<StringData *>(v)->get_data();

    // announce-list
    if (auto *v = dict->get_val("announce-list"); v && v->type_ == DataType::list_)
    {
        auto *list = dynamic_cast<Listdata *>(v);
        for (size_t i = 0; i < list->get_data().size(); i++)
        {
            auto *tier = list->get_data()[i];
            if (!tier || tier->type_ != DataType::list_) continue;
            auto *tier_list = dynamic_cast<Listdata *>(tier);
            std::vector<std::string> tier_urls;
            for (size_t j = 0; j < tier_list->get_data().size(); j++)
            {
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
    if (auto *v = dict->get_val("encoding"); v && v->type_ == DataType::string_)
    {
        auto enc = dynamic_cast<StringData *>(v)->get_data();
        if (enc.find("UTF-8") != std::string::npos || enc.find("utf-8") != std::string::npos)
            meta.encoding_ = 0;
    }

    // info hash - extract raw bencoded info dict and SHA-1 it
    std::string info_bencode = extract_info_bencode(torrent_data.c_str(), torrent_data.size());
    if (!info_bencode.empty())
    {
        auto hash = sha1_hash(
            reinterpret_cast<const unsigned char *>(info_bencode.data()),
            info_bencode.size());
        meta.info_hash_ = hash;
        meta.info_hash_hex_ = to_hex(hash);
    }

    // info dictionary
    if (auto *v = dict->get_val("info"); v && v->type_ == DataType::dictionary_)
    {
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
        if (auto *nv = info_dict->get_val("files"); nv && nv->type_ == DataType::list_)
        {
            auto *files_list = dynamic_cast<Listdata *>(nv);
            int64_t offset = 0;
            for (size_t i = 0; i < files_list->get_data().size(); i++)
            {
                auto *file_node = files_list->get_data()[i];
                if (!file_node || file_node->type_ != DataType::dictionary_) continue;
                auto *file_dict = dynamic_cast<DicttionaryData *>(file_node);

                TorrentFile tf;
                tf.offset_ = offset;

                if (auto *lv = file_dict->get_val("length"); lv && lv->type_ == DataType::integer_)
                    tf.length_ = dynamic_cast<IntegerData *>(lv)->get_data();

                if (auto *lv = file_dict->get_val("md5sum"); lv && lv->type_ == DataType::string_)
                    tf.md5sum_ = dynamic_cast<StringData *>(lv)->get_data();

                if (auto *lv = file_dict->get_val("path"); lv && lv->type_ == DataType::list_)
                {
                    auto *path_list = dynamic_cast<Listdata *>(lv);
                    for (size_t j = 0; j < path_list->get_data().size(); j++)
                    {
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
        meta.announce_list_.push_back({meta.announce_});

    delete root;
    return meta;
}

TorrentMeta TorrentMeta::parse_file(const std::string &file_path)
{
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) return TorrentMeta();

    std::ostringstream oss;
    oss << ifs.rdbuf();
    return parse(oss.str());
}

} // namespace yuan::net::bit_torrent
