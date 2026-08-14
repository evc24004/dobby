#pragma once

namespace dobby {

// Runs once during mod_init. The sweep constructs local packet instances and
// writes them only to isolated in-memory streams; it never touches transport.
void dumpProtocolOnStartup();

} // namespace dobby
