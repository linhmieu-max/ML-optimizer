#include "policy/policy_store.hpp"

#include "core/atomic_file.hpp"
#include "core/sysfs.hpp"
#include "policy/profiles.hpp"

namespace opt::policy {
namespace fs = std::filesystem;

Result<UserMode> load_user_mode(const fs::path& file) {
  UserMode m;
  std::error_code ec;
  if (!fs::exists(file, ec)) return m;
  auto text = sysfs::read_text(file, 4096);
  if (!text) return fail(errno_error(Errc::Io, "cannot read " + file.string()));
  bool have_mode = false;
  std::string mode, profile, locked;
  for (auto line : sysfs::split_lines(*text)) {
    const auto eq = line.find('=');
    if (eq == std::string_view::npos) return fail(make_error(Errc::Parse, "malformed policy state line"));
    const auto k = line.substr(0, eq);
    const std::string v(line.substr(eq + 1));
    if (k == "mode") { mode = v; have_mode = true; }
    else if (k == "profile") profile = v;
    else if (k == "locked") locked = v;
    else return fail(make_error(Errc::Parse, "unknown key in policy state"));
  }
  if (!have_mode || (mode != "auto" && mode != "manual")) return fail(make_error(Errc::Parse, "invalid mode in policy state"));
  if (mode == "manual") {
    auto p = parse_user_profile(profile);
    if (!p) return fail(make_error(Errc::Parse, "invalid profile in policy state"));
    if (locked != "0" && locked != "1") return fail(make_error(Errc::Parse, "invalid locked flag in policy state"));
    m = {true, *p, locked == "1"};
  }
  return m;
}

Result<void> save_user_mode(const fs::path& file, const UserMode& m) {
  const std::string content = m.manual ? "mode=manual\nprofile=" + std::string(user_name(m.profile)) + "\nlocked=" + (m.locked ? "1" : "0") + "\n"
                                       : std::string("mode=auto\n");
  return write_file_atomic(file, content);
}

}  // namespace opt::policy
