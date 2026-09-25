#ifndef LAPIS_DESKTOP_APP_PATHS_HPP
#define LAPIS_DESKTOP_APP_PATHS_HPP
#include <QString>

namespace lapis::desktop {
// Where lapis keeps lapis.json and runtime/: the checkout for a developer
// build, ~/.lapis for the downloaded app (short, because agent sockets live in
// runtime/ and a socket path has about 100 bytes), or LAPIS_HOME when set.
[[nodiscard]] QString data_directory();
// The session service that owns each agent: beside the app's executable in
// the downloaded app, the build tree's copy in a developer build.
[[nodiscard]] QString session_service_program();
// The Vulkan library Qt loads: the app's own MoltenVK, or the one the
// developer build was configured with.
[[nodiscard]] QString vulkan_library();
// Where an explicit terminal starts without --cwd: the checkout for a
// developer build, home for the downloaded app.
[[nodiscard]] QString default_working_directory();
} // namespace lapis::desktop
#endif
