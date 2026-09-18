#include "platform/posix/unique_fd.hpp"

#include <array>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace {

using lapis::session::posix::UniqueFd;

static_assert(!std::is_copy_constructible_v<UniqueFd>);
static_assert(!std::is_copy_assignable_v<UniqueFd>);
static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct Pipe {
    UniqueFd reader;
    UniqueFd writer;
};

Pipe make_pipe() {
    std::array<int, 2> descriptors{-1, -1};
    require(::pipe(descriptors.data()) == 0, "pipe creation failed");
    return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

bool is_closed(int descriptor) {
    errno = 0;
    return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

void check_transfer_and_io() {
    auto pipe = make_pipe();
    const int original = pipe.writer.get();
    {
        UniqueFd moved(std::move(pipe.writer));
        require(!pipe.writer && moved.get() == original, "move lost exclusive ownership");
        constexpr char expected = 'x';
        require(::write(moved.get(), &expected, 1) == 1, "moved descriptor cannot write");
        char observed = 0;
        require(::read(pipe.reader.get(), &observed, 1) == 1 && observed == expected,
                "moved descriptor did not preserve pipe I/O");
    }
    require(is_closed(original), "destruction did not close the moved descriptor");
    char observed = 0;
    require(::read(pipe.reader.get(), &observed, 1) == 0, "closed writer did not produce EOF");
}

void check_move_assignment() {
    auto first = make_pipe();
    auto second = make_pipe();
    const int replaced = first.writer.get();
    const int transferred = second.writer.get();
    first.writer = std::move(second.writer);
    require(is_closed(replaced), "move assignment leaked the old descriptor");
    require(!second.writer && first.writer.get() == transferred,
            "move assignment lost exclusive ownership");
    // Exercise self-move through an alias without suppressing compiler diagnostics.
    auto& alias = first.writer;
    first.writer = std::move(alias);
    require(first.writer.get() == transferred && !is_closed(transferred),
            "self-move closed the owned descriptor");
}

void check_release_and_reset() {
    auto pipe = make_pipe();
    UniqueFd released;
    {
        UniqueFd temporary(std::move(pipe.writer));
        released.reset(temporary.release());
        require(!temporary, "release retained ownership");
    }
    require(!is_closed(released.get()), "release closed the transferred descriptor");
    released.reset(released.get());
    require(!is_closed(released.get()), "reset to the same descriptor closed it");
    const int previous = released.get();
    released.reset();
    require(!released && is_closed(previous), "reset failed to close the descriptor");
    released.reset();
    require(released.release() == -1, "empty release returned an owned descriptor");
}

} // namespace

int main() try {
    check_transfer_and_io();
    check_move_assignment();
    check_release_and_reset();
    std::cout << "PASS: descriptor transfer, real pipe I/O, close, release and reset\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Session platform ownership failed: " << error.what() << '\n';
    return 1;
}
