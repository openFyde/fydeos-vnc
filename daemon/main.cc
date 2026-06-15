// Copyright 2026 The FydeOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// fydeos-vncd: LAN-only VNC server daemon for FydeOS. See
// develope/task/3/fydeos-vnc-design.html for architecture.

#include <sysexits.h>
#include <termios.h>
#include <unistd.h>

#include <iostream>
#include <memory>
#include <string>

#include <base/command_line.h>
#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/logging.h>
#include <brillo/daemons/dbus_daemon.h>
#include <brillo/syslog_logging.h>

#include "fydeos-vnc/daemon/dbus_service.h"
#include "fydeos-vnc/daemon/pipeline.h"
#include "fydeos-vnc/mojo/chrome_client.h"
#include "fydeos-vnc/net/password.h"
#include "fydeos-vnc/net/tls.h"

namespace {

constexpr char kVncManagerServiceName[] = "org.fydeos.VncManager";
constexpr int kDefaultRfbPort = 5900;
constexpr char kDefaultStateDir[] = "/var/lib/fydeos-vncd";

constexpr char kSwitchPort[] = "port";
constexpr char kSwitchDisplay[] = "display";
constexpr char kSwitchStateDir[] = "state-dir";
constexpr char kSwitchStartEnabled[] = "start-enabled";
constexpr char kSwitchSetPassword[] = "set-password";
constexpr char kSwitchClearPassword[] = "clear-password";
constexpr char kSwitchShowFingerprint[] = "show-fingerprint";

base::FilePath CertPath(const base::FilePath& dir) {
  return dir.Append("cert.pem");
}
base::FilePath KeyPath(const base::FilePath& dir) {
  return dir.Append("key.pem");
}
base::FilePath PasswordPath(const base::FilePath& dir) {
  return dir.Append("password.bcrypt");
}

std::string ReadPasswordFromTty(const std::string& prompt) {
  std::cerr << prompt << std::flush;
  termios old_t{}, new_t{};
  const bool is_tty = isatty(STDIN_FILENO) != 0;
  if (is_tty && tcgetattr(STDIN_FILENO, &old_t) == 0) {
    new_t = old_t;
    new_t.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_t);
  }
  std::string pw;
  std::getline(std::cin, pw);
  if (is_tty) {
    tcsetattr(STDIN_FILENO, TCSANOW, &old_t);
    std::cerr << "\n";
  }
  return pw;
}

int RunPasswordSet(const base::FilePath& dir) {
  base::CreateDirectory(dir);
  const std::string pw = ReadPasswordFromTty("New VNC password: ");
  if (pw.empty()) {
    std::cerr << "Password empty; nothing written.\n";
    return EX_USAGE;
  }
  if (pw != ReadPasswordFromTty("Confirm: ")) {
    std::cerr << "Passwords differ; nothing written.\n";
    return EX_DATAERR;
  }
  auto hash = fydeos::vnc::net::HashPassword(pw);
  if (!hash || !fydeos::vnc::net::SavePasswordHash(PasswordPath(dir),
                                                    *hash)) {
    std::cerr << "Failed to write hash.\n";
    return EX_IOERR;
  }
  std::cerr << "Password updated.\n";
  return EX_OK;
}

int RunPasswordClear(const base::FilePath& dir) {
  base::DeleteFile(PasswordPath(dir));
  std::cerr << "Password cleared.\n";
  return EX_OK;
}

int RunShowFingerprint(const base::FilePath& dir) {
  if (!fydeos::vnc::net::EnsureSelfSignedCertificate(CertPath(dir),
                                                     KeyPath(dir))) {
    return EX_SOFTWARE;
  }
  std::cout << fydeos::vnc::net::GetCertificateFingerprint(CertPath(dir))
            << "\n";
  return EX_OK;
}

class FydeosVncDaemon : public brillo::DBusServiceDaemon {
 public:
  FydeosVncDaemon(fydeos::vnc::daemon::Pipeline::Config pipeline_config,
                  bool start_enabled)
      : DBusServiceDaemon(kVncManagerServiceName),
        pipeline_config_(std::move(pipeline_config)),
        start_enabled_(start_enabled) {}
  FydeosVncDaemon(const FydeosVncDaemon&) = delete;
  FydeosVncDaemon& operator=(const FydeosVncDaemon&) = delete;

 protected:
  void OnShutdown(int* exit_code) override {
    service_.reset();
    pipeline_.reset();
    DBusServiceDaemon::OnShutdown(exit_code);
  }

  void RegisterDBusObjectsAsync(
      brillo::dbus_utils::AsyncEventSequencer* sequencer) override {
    // Bootstrap Chrome Mojo connection first so it's available when the
    // pipeline initializes the source selector. Failure is non-fatal — the
    // selector will stay on KMS+uinput if Chrome can't be reached.
    chrome_client_ =
        std::make_unique<fydeos::vnc::mojo_client::ChromeClient>();
    if (!chrome_client_->Bootstrap(bus_.get())) {
      LOG(WARNING) << "Chrome Mojo bootstrap unavailable; Selector will "
                      "stay on KMS path only.";
    }

    pipeline_ = std::make_unique<fydeos::vnc::daemon::Pipeline>(
        pipeline_config_, chrome_client_.get(), bus_.get());
    service_ = std::make_unique<fydeos::vnc::daemon::VncManagerService>(
        bus_, pipeline_.get());
    service_->RegisterAsync(sequencer);

    if (start_enabled_) {
      pipeline_->SetEnabled(true);
    }
  }

 private:
  const fydeos::vnc::daemon::Pipeline::Config pipeline_config_;
  const bool start_enabled_;
  std::unique_ptr<fydeos::vnc::daemon::Pipeline> pipeline_;
  std::unique_ptr<fydeos::vnc::daemon::VncManagerService> service_;
  std::unique_ptr<fydeos::vnc::mojo_client::ChromeClient> chrome_client_;
};

}  // namespace

int main(int argc, char* argv[]) {
  base::CommandLine::Init(argc, argv);
  brillo::InitLog(brillo::kLogToSyslog | brillo::kLogToStderrIfTty);

  auto* cmdline = base::CommandLine::ForCurrentProcess();
  base::FilePath state_dir(kDefaultStateDir);
  if (cmdline->HasSwitch(kSwitchStateDir)) {
    state_dir =
        base::FilePath(cmdline->GetSwitchValueASCII(kSwitchStateDir));
  }

  // Utility modes — don't enter the daemon loop.
  if (cmdline->HasSwitch(kSwitchSetPassword)) {
    return RunPasswordSet(state_dir);
  }
  if (cmdline->HasSwitch(kSwitchClearPassword)) {
    return RunPasswordClear(state_dir);
  }
  if (cmdline->HasSwitch(kSwitchShowFingerprint)) {
    return RunShowFingerprint(state_dir);
  }

  fydeos::vnc::daemon::Pipeline::Config cfg;
  cfg.state_dir = state_dir;
  cfg.port = kDefaultRfbPort;
  if (cmdline->HasSwitch(kSwitchPort)) {
    cfg.port =
        std::atoi(cmdline->GetSwitchValueASCII(kSwitchPort).c_str());
    if (cfg.port <= 0 || cfg.port > 65535) {
      LOG(ERROR) << "Invalid --port";
      return EX_USAGE;
    }
  }
  if (cmdline->HasSwitch(kSwitchDisplay)) {
    const std::string s = cmdline->GetSwitchValueASCII(kSwitchDisplay);
    if (s == "internal") {
      cfg.display_spec =
          fydeos::vnc::capture::CrtcFinder::Spec::kInternalDisplay;
    } else if (s == "external") {
      cfg.display_spec =
          fydeos::vnc::capture::CrtcFinder::Spec::kExternalDisplay;
    } else if (s != "any") {
      LOG(ERROR) << "Invalid --display=" << s;
      return EX_USAGE;
    }
  }

  return FydeosVncDaemon(std::move(cfg),
                          cmdline->HasSwitch(kSwitchStartEnabled))
      .Run();
}
