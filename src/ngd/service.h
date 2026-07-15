// NeuralGuard Windows service. Running enforcement as a LocalSystem service is
// what removes UAC entirely: the service is always elevated, starts at boot, and
// the tray just talks to it. SCM restart-on-crash is the watchdog; the Enforcer's
// dynamic WFP session is the fail-open-on-death (a dead service leaves no filters).
#pragma once

namespace ng {

int ServiceInstall(const char* dbPath);  // create (auto-start, LocalSystem) + start; needs admin
int ServiceUninstall();                  // stop + delete; needs admin
int ServiceRun(const char* dbPath);      // SCM entry point (blocks until stopped)
// Stop the service (via the SCM) + any foreground worker; needs admin.
// recordOff distinguishes the two reasons to stop: true = the USER asked for it,
// so persist desired_mode='idle' and stay off across reboots; false = maintenance
// (an installer stopping us to replace files), where clobbering the user's intent
// would silently leave them unprotected after an upgrade.
int ServiceStop(const char* dbPath, bool recordOff);

}  // namespace ng
