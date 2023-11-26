#ifndef __BUFFER_H__
#define __BUFFER_H__

#include <algorithm>
#include <stdint.h>
#include <string>
#include <vcruntime_string.h>
#include <vector>

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
        return 0;
    }

    uint16_t read_int16()
    {
        return 0;
    }

    uint32_t read_uint32()
    {
        return 0;
    }

    int32_t read_int32()
    {
        return 0;
    }

    uint64_t read_uint64()
    {
        return 0;
    }

    int64_t read_int64()
    {
        return 0;
    }

    float read_float32()
    {
        return 0;
    }

    double read_float64()
    {
        return 0;
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

    }

    void write_int16(int16_t val)
    {

    }

    void write_uint32(uint32_t val)
    {

    }

    void write_int32(int32_t val)
    {

    }

    void write_uint64(uint64_t val)
    {

    }

    void write_int64(int64_t val)
    {

    }

    void write_float32(float val)
    {

    }

    void write_float64(float val)
    {

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
        while (p) {
            buffs.push_back(*p);
            ++p;
        }
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
        return buffs.size() - read_index;
    }

    size_t get_read_index() const 
    {
        return read_index;
    }

    size_t get_write_index() const 
    {
        return read_index;
    }

    void reset_read_index(size_t idx)
    {
        if (idx <= 0 || idx >= buffs.size()) {
            return;
        }

        read_index = idx;
    }

private:
    void write(void * data, size_t len)
    {
        write(static_cast<const char *>(data), len);
    }

    void write(const char * data, size_t len)
    {
        if (buffs.size() < len) {
            buffs.reserve(len);
        }

        for (int i = 0; i < len; ++i) {
            buffs.push_back(data[i]);
        }
    }

public:
    Buffer() : read_index(0), write_index(0)
    {
        buffs.reserve(10);
    }


private:
    size_t read_index;
    size_t write_index;
    std::vector<char> buffs;
};

#endif // __BUFFER_H__