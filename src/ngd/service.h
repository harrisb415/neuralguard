// NeuralGuard Windows service. Running enforcement as a LocalSystem service is
// what removes UAC entirely: the service is always elevated, starts at boot, and
// the tray just talks to it. SCM restart-on-crash is the watchdog; the Enforcer's
// dynamic WFP session is the fail-open-on-death (a dead service leaves no filters).
#pragma once

namespace ng {

int ServiceInstall(const char* dbPath);  // create (auto-start, LocalSystem) + start; needs admin
int ServiceUninstall();                  // stop + delete; needs admin
int ServiceRun(const char* dbPath);      // SCM entry point (blocks until stopped)
int ServiceStop(const char* dbPath);     // stop the service (SCM) + any foreground worker; needs admin

}  // namespace ng
