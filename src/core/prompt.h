// Client side of the block-notify-retry prompt: ask the ngtray app (running in
// the user's session) about a connection and get the user's decision back.
#pragma once

#include <string>

namespace ng {

// Prompt the tray about `app -> dest:port`. Returns the decision:
//   'A' always allow, 'O' allow once, 'B' block, or 0 if the tray is unreachable.
// Blocks until the user answers (or the tray is gone).
char PromptTray(const std::string& app, const std::string& dest, int port);

}  // namespace ng
