#include "buffer/buffer_chain.h"
#include "buffer/byte_buffer.h"

#include <iostream>
#include <string>

int main()
{
    yuan::buffer::ByteBuffer buffer(8);
    buffer.append(std::string_view("ab"));
    buffer.append_u16(0x1234);
    buffer.append_i8(-5);

    if (buffer.readable_bytes() != 5) {
        std::cerr << "unexpected readable bytes\n";
        return 1;
    }

    if (buffer.read_ptr()[0] != 'a' || buffer.read_ptr()[1] != 'b') {
        std::cerr << "unexpected prefix bytes\n";
        return 1;
    }

    buffer.consume(2);
    if (buffer.read_u16() != 0x1234) {
        std::cerr << "u16 roundtrip failed\n";
        return 1;
    }

    if (buffer.read_i8() != -5) {
        std::cerr << "i8 roundtrip failed\n";
        return 1;
    }

    yuan::buffer::ByteBuffer from_text(std::string_view("xyz"));
    if (std::string(from_text.read_ptr(), from_text.readable_bytes()) != "xyz") {
        std::cerr << "string_view constructor failed\n";
        return 1;
    }

    buffer.clear();
    buffer.append(std::string_view("hello"));
    buffer.consume(2);
    buffer.ensure_writable(16);
    buffer.compact();
    if (std::string(buffer.read_ptr(), buffer.readable_bytes()) != "llo") {
        std::cerr << "compact failed\n";
        return 1;
    }

    yuan::buffer::BufferChain chain;
    auto *first = chain.emplace_back();
    first->append(std::string_view("one"));
    auto *second = chain.emplace_back();
    second->append(std::string_view("two"));

    std::string joined;
    chain.for_each_readable([&joined](const yuan::buffer::ByteBuffer &part) {
        joined.append(part.read_ptr(), part.readable_bytes());
        return true;
    });

    if (joined != "onetwo") {
        std::cerr << "buffer chain join failed\n";
        return 1;
    }

    if (chain.readable_bytes() != 6) {
        std::cerr << "buffer chain readable byte count failed\n";
        return 1;
    }

    auto front = chain.pop_front();
    if (!front || std::string(front->read_ptr(), front->readable_bytes()) != "one") {
        std::cerr << "buffer chain pop_front failed\n";
        return 1;
    }

    yuan::buffer::ByteBuffer copied = second->copy_readable();
    front->append(copied);
    if (std::string(front->read_ptr(), front->readable_bytes()) != "onetwo") {
        std::cerr << "byte buffer append/copy failed\n";
        return 1;
    }

    std::cout << "buffer model test passed\n";
    return 0;
}
