// Two ways to feed itch::parse:
//
//   MappedFile    mmap a decompressed file and parse it in one call, zero copy.
//                 Use this for throughput measurements.
//   parse_stream  read a file descriptor in chunks (e.g. `gunzip -c day.gz |`),
//                 carrying any message split across chunks into the next read.
#pragma once

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "itch.hpp"

namespace itch {

class MappedFile {
public:
    explicit MappedFile(const std::string& path) {
        fd_ = ::open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error(path + ": " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd_, &st) != 0) {
            ::close(fd_);
            throw std::runtime_error(path + ": " + std::strerror(errno));
        }
        size_ = static_cast<size_t>(st.st_size);
        if (size_ > 0) {
            void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
            if (p == MAP_FAILED) {
                ::close(fd_);
                throw std::runtime_error(path + ": mmap: " + std::strerror(errno));
            }
            ::madvise(p, size_, MADV_SEQUENTIAL);  // read-ahead hint; we touch each page once
            data_ = static_cast<const unsigned char*>(p);
        }
    }
    ~MappedFile() {
        if (data_ != nullptr) ::munmap(const_cast<unsigned char*>(data_), size_);
        if (fd_ >= 0) ::close(fd_);
    }
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::span<const unsigned char> bytes() const noexcept { return {data_, size_}; }

private:
    int fd_ = -1;
    const unsigned char* data_ = nullptr;
    size_t size_ = 0;
};

struct StreamResult {
    uint64_t bytes = 0;     // bytes of complete messages processed
    uint64_t messages = 0;
    Status status = Status::Ok;  // Partial here means the input ended mid-message
    char bad_type = 0;
    uint16_t bad_length = 0;
};

template <class Handler>
StreamResult parse_stream(int fd, Handler& h, size_t chunk = size_t(1) << 20) {
    // Room for one chunk plus the tail of a message cut off by the previous one.
    std::vector<unsigned char> buf(chunk + kMaxMessageSize + 2);
    StreamResult out;
    size_t tail = 0;
    while (true) {
        // A message of an unknown type can be longer than any 5.0 message; if
        // the carried tail fills the buffer, grow it instead of overrunning it.
        if (tail + chunk > buf.size()) buf.resize(tail + chunk);
        ssize_t n = ::read(fd, buf.data() + tail, chunk);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("read: ") + std::strerror(errno));
        }
        if (n == 0) {
            out.status = tail == 0 ? Status::Ok : Status::Partial;
            return out;
        }
        const size_t have = tail + static_cast<size_t>(n);
        const ParseResult r = parse(std::span<const unsigned char>(buf.data(), have), h);
        out.bytes += r.consumed;
        out.messages += r.messages;
        if (r.status == Status::BadLength || r.status == Status::ZeroLength) {
            out.status = r.status;
            out.bad_type = r.bad_type;
            out.bad_length = r.bad_length;
            return out;
        }
        tail = have - r.consumed;
        std::memmove(buf.data(), buf.data() + r.consumed, tail);
    }
}

}  // namespace itch
