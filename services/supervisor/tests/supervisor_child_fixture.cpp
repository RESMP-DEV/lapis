#include <cerrno>
#include <fcntl.h>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

bool hex_token(std::string_view value) {
    if (value.size() != 64)
        return false;
    for (const auto character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        if (!digit && !lower)
            return false;
    }
    return true;
}

std::string argument(int argc, char** argv, std::string_view name) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view value = argv[index];
        if (value.starts_with(name) && value.size() > name.size() && value[name.size()] == '=') {
            return std::string{value.substr(name.size() + 1)};
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    const auto token = argument(argc, argv, "--supervisor-token");
    const auto owner = argument(argc, argv, "--supervisor-owner");
    if (!hex_token(token) || owner.empty() || owner.front() != '/')
        return 2;

    const auto temporary = owner + "." + std::to_string(::getpid()) + ".tmp";
    const int ownership = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (ownership < 0) {
        return 5;
    }
    const std::string bytes = "LAPIS-SUP-OWNER-1\n" + token + "\n" + std::to_string(::getpid());
    bool wrote =
        static_cast<std::size_t>(::write(ownership, bytes.data(), bytes.size())) == bytes.size();
    wrote = wrote && ::fsync(ownership) == 0 && ::close(ownership) == 0;
    wrote = wrote && ::rename(temporary.c_str(), owner.c_str()) == 0;
    if (!wrote)
        return 6;

    while (::pause() == -1) {
        if (errno != EINTR)
            return 7;
    }
}
