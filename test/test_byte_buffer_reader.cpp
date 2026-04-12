#include "buffer/byte_buffer_reader.h"

#include <iostream>
#include <string>

int main()
{
    yuan::buffer::ByteBuffer first;
    first.append(std::string_view("hello\r"));

    yuan::buffer::ByteBuffer second;
    second.append(std::string_view("\nworld"));

    yuan::buffer::BufferChain chain;
    chain.push_back(std::make_unique<yuan::buffer::ByteBuffer>(std::move(first)));
    chain.push_back(std::make_unique<yuan::buffer::ByteBuffer>(std::move(second)));

    yuan::buffer::ByteBufferReader reader(chain);

    std::string line;
    if (reader.read_line(line) != 0 || line != "hello") {
        std::cerr << "read_line failed\n";
        return 1;
    }

    if (reader.remaining_bytes() != 5) {
        std::cerr << "unexpected remaining bytes after line read\n";
        return 1;
    }

    reader.mark();
    char word[6] = {};
    if (reader.read(word, 5) != 5 || std::string(word, 5) != "world") {
        std::cerr << "read failed\n";
        return 1;
    }

    if (!reader.empty()) {
        std::cerr << "reader should be empty\n";
        return 1;
    }

    reader.rollback();
    if (reader.peek_char() != 'w') {
        std::cerr << "rollback failed\n";
        return 1;
    }

    std::cout << "byte buffer reader test passed\n";
    return 0;
}
