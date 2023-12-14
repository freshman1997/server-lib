#ifndef __BUFFER_H__
#define __BUFFER_H__

#include <algorithm>
#include <cstddef>
#include <stdint.h>
#include <string>
#include <cstring>
#include <vector>

#include "endian/endian.hpp"

// TODO fix 复用
class Buffer
{
public:
    int8_t read_uint8()
    {
        int8_t be8;
        ::memcpy(&be8, peek(), sizeof(int8_t));
        read_index += sizeof(int8_t);
        return be8;
    }

    uint8_t read_int8()
    {
        uint8_t ube8;
        ::memcpy(&ube8, peek(), sizeof(uint8_t));
        read_index += sizeof(uint8_t);
        return ube8;
    }

    int16_t read_uint16()
    {
        int16_t be16;
        ::memcpy(&be16, peek(), sizeof(int16_t));
        read_index += sizeof(int16_t);
        return endian::networkToHost16(be16);
    }

    uint16_t read_int16()
    {
        uint16_t ube16;
        ::memcpy(&ube16, peek(), sizeof(uint16_t));
        read_index += sizeof(uint16_t);
        return endian::networkToHost16(ube16);
    }

    uint32_t read_uint32()
    {
        uint32_t ube32;
        ::memcpy(&ube32, peek(), sizeof(uint32_t));
        read_index += sizeof(uint32_t);
        return endian::networkToHost32(ube32);
    }

    int32_t read_int32()
    {
        int32_t be32;
        ::memcpy(&be32, peek(), sizeof(int32_t));
        read_index += sizeof(int32_t);
        return endian::networkToHost32(be32);
    }

    uint64_t read_uint64()
    {
        uint64_t be64;
        ::memcpy(&be64, peek(), sizeof(uint64_t));
        read_index += sizeof(uint64_t);
        return endian::networkToHost64(be64);
    }

    int64_t read_int64()
    {
        int64_t be64;
        ::memcpy(&be64, peek(), sizeof(int64_t));
        read_index += sizeof(int64_t);
        return endian::networkToHost64(be64);
    }

public:
    void write_uint8(uint8_t val)
    {
        write((void *)&val, sizeof(uint8_t));
    }

    void write_int8(int8_t val)
    {
        write((void *)&val, sizeof(int8_t));
    }

    void write_uint16(uint16_t val)
    {
        uint16_t ube16 = endian::hostToNetwork16(val);
        write((void *)&ube16, sizeof(uint16_t));
    }

    void write_int16(int16_t val)
    {
        int16_t be16 = endian::hostToNetwork16(val);
        write((void *)&be16, sizeof(int16_t));
    }

    void write_uint32(uint32_t val)
    {
        uint32_t ube32 = endian::hostToNetwork32(val);
        write((void *)&ube32, sizeof(uint32_t));
    }

    void write_int32(int32_t val)
    {
        int32_t be32 = endian::hostToNetwork32(val);
        write((void *)&be32, sizeof(int32_t));
    }

    void write_uint64(uint64_t val)
    {
        uint64_t ube64 = endian::hostToNetwork64(val);
        write((void *)&ube64, sizeof(uint64_t));
    }

    void write_int64(int64_t val)
    {
        int64_t be64 = endian::hostToNetwork64(val);
        write((void *)&be64, sizeof(int64_t));
    }

    void write_bool(bool val)
    {
        write_int8(val ? 1 : 0);
    }

    void write_string(std::string &str)
    {
        write(str.c_str(), str.size());
    }

    void write_string(const char *str)
    {
        const char *p = str;
        while (*p) {
            buffs.push_back(*p);
            ++p;
        }

        write_index += p - str;
    }
public:
    char* begin()
    { 
        return &*buffs.begin();
    }

    const char* begin() const
    { 
        return &*buffs.begin();
    }

    const char* peek() const
    { 
        return begin() + read_index;
    }

    size_t remain_bytes() const
    {
        return 0;
    }

    size_t readable_bytes() const
    {
        return write_index - read_index;
    }

    size_t get_read_index() const 
    {
        return read_index;
    }

    size_t get_write_index() const 
    {
        return write_index;
    }

    void reset_read_index(size_t idx)
    {
        if (idx <= 0 || idx >= buffs.size()) {
            return;
        }

        read_index = idx;
    }

    void rewind()
    {
        read_index = 0;
        write_index = 0;
        if (buffs.empty()) {
            buffs.resize(1024);
        }
    }

    void set_write_index(size_t idx)
    {
        write_index = idx;
    }

    size_t writable_size() const
    {
        return buffs.size() - write_index;
    }

    void fill(size_t bytes)
    {
        write_index += bytes;
    }

    void resize()
    {
        buffs.resize(buffs.size() + 1024);
    }

private:
    void write(void * data, size_t len)
    {
        const char* d = static_cast<const char*>(data);
        std::copy(d, d + len, begin() + read_index);
        write_index += len;
    }

    void write(const char * data, size_t len)
    {
        if (buffs.size() < len) {
            buffs.reserve(len);
        }

        for (int i = 0; i < len; ++i) {
            buffs.push_back(data[i]);
        }

        write_index += len;
    }

public:
    Buffer() : read_index(0), write_index(0)
    {
        buffs.resize(1024);
    }

private:
    size_t read_index;
    size_t write_index;
    std::vector<char> buffs;
};

#endif // __BUFFER_H__