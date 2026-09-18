#ifndef LAPIS_SESSION_PLATFORM_POSIX_UNIQUE_FD_HPP
#define LAPIS_SESSION_PLATFORM_POSIX_UNIQUE_FD_HPP

namespace lapis::session::posix {

// Internal to the service's POSIX backend. Takes ownership of a descriptor or -1;
// callers synchronize transfers and never close a borrowed get() themselves.
class UniqueFd final {
  public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int descriptor) noexcept;
    ~UniqueFd() noexcept;

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] int release() noexcept;
    void reset(int descriptor = -1) noexcept;

  private:
    int descriptor_{-1};
};

} // namespace lapis::session::posix

#endif // LAPIS_SESSION_PLATFORM_POSIX_UNIQUE_FD_HPP
