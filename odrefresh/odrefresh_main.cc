/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/stat.h>

#include <string>
#include <string_view>

#include "android-base/parseint.h"
#include "android-base/properties.h"
#include "android-base/stringprintf.h"
#include "android-base/strings.h"
#include "arch/instruction_set.h"
#include "base/file_utils.h"
#include "base/globals.h"
#include "odr_common.h"
#include "odr_compilation_log.h"
#include "odr_config.h"
#include "odr_metrics.h"
#include "odrefresh.h"
#include "odrefresh/odrefresh.h"

namespace {

using ::art::odrefresh::CompilationOptions;
using ::art::odrefresh::ExitCode;
using ::art::odrefresh::OdrCompilationLog;
using ::art::odrefresh::OdrConfig;
using ::art::odrefresh::OdrMetrics;
using ::art::odrefresh::OnDeviceRefresh;
using ::art::odrefresh::QuotePath;
using ::art::odrefresh::ZygoteKind;

void UsageErrorV(const char* fmt, va_list ap) {
  std::string error;
  android::base::StringAppendV(&error, fmt, ap);
  if (isatty(fileno(stderr))) {
    std::cerr << error << std::endl;
  } else {
    LOG(ERROR) << error;
  }
}

void UsageError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
}

NO_RETURN void ArgumentError(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  UsageErrorV(fmt, ap);
  va_end(ap);
  UsageError("Try '--help' for more information.");
  exit(EX_USAGE);
}

bool ParseZygoteKind(const char* input, ZygoteKind* zygote_kind) {
  std::string_view z(input);
  if (z == "zygote32") {
    *zygote_kind = ZygoteKind::kZygote32;
    return true;
  } else if (z == "zygote32_64") {
    *zygote_kind = ZygoteKind::kZygote32_64;
    return true;
  } else if (z == "zygote64_32") {
    *zygote_kind = ZygoteKind::kZygote64_32;
    return true;
  } else if (z == "zygote64") {
    *zygote_kind = ZygoteKind::kZygote64;
    return true;
  }
  return false;
}

std::string GetEnvironmentVariableOrDie(const char* name) {
  const char* value = getenv(name);
  LOG_ALWAYS_FATAL_IF(value == nullptr, "%s is not defined.", name);
  return value;
}

std::string GetEnvironmentVariableOrDefault(const char* name, std::string default_value) {
  const char* value = getenv(name);
  if (value == nullptr) {
    return default_value;
  }
  return value;
}

bool ArgumentMatches(std::string_view argument, std::string_view prefix, std::string* value) {
  if (android::base::StartsWith(argument, prefix)) {
    *value = std::string(argument.substr(prefix.size()));
    return true;
  }
  return false;
}

bool ArgumentEquals(std::string_view argument, std::string_view expected) {
  return argument == expected;
}

int InitializeConfig(int argc, char** argv, OdrConfig* config) {
  config->SetApexInfoListFile("/apex/apex-info-list.xml");
  config->SetArtBinDir(art::GetArtBinDir());
  config->SetBootClasspath(GetEnvironmentVariableOrDie("BOOTCLASSPATH"));
  config->SetDex2oatBootclasspath(GetEnvironmentVariableOrDie("DEX2OATBOOTCLASSPATH"));
  config->SetSystemServerClasspath(GetEnvironmentVariableOrDie("SYSTEMSERVERCLASSPATH"));
  config->SetStandaloneSystemServerJars(
      GetEnvironmentVariableOrDefault("STANDALONE_SYSTEMSERVER_JARS", /*default_value=*/""));
  config->SetIsa(art::kRuntimeISA);

  std::string zygote;
  int n = 1;
  for (; n < argc - 1; ++n) {
    const char* arg = argv[n];
    std::string value;
    if (ArgumentMatches(arg, "--use-compilation-os=", &value)) {
      int cid;
      if (!android::base::ParseInt(value, &cid)) {
        ArgumentError("Failed to parse CID: %s", value.c_str());
      }
      config->SetCompilationOsAddress(cid);
    } else if (ArgumentMatches(arg, "--dalvik-cache=", &value)) {
      art::OverrideDalvikCacheSubDirectory(value);
      config->SetArtifactDirectory(GetApexDataDalvikCacheDirectory(art::InstructionSet::kNone));
    } else if (ArgumentMatches(arg, "--max-execution-seconds=", &value)) {
      int seconds;
      if (!android::base::ParseInt(value, &seconds)) {
        ArgumentError("Failed to parse integer: %s", value.c_str());
      }
      config->SetMaxExecutionSeconds(seconds);
    } else if (ArgumentMatches(arg, "--max-child-process-seconds=", &value)) {
      int seconds;
      if (!android::base::ParseInt(value, &seconds)) {
        ArgumentError("Failed to parse integer: %s", value.c_str());
      }
      config->SetMaxChildProcessSeconds(seconds);
    } else if (ArgumentMatches(arg, "--zygote-arch=", &value)) {
      zygote = value;
    } else if (ArgumentMatches(arg, "--staging-dir=", &value)) {
      config->SetStagingDir(value);
    } else if (ArgumentEquals(arg, "--dry-run")) {
      config->SetDryRun();
    } else if (ArgumentEquals(arg, "--partial-compilation")) {
      config->SetPartialCompilation(true);
    } else if (ArgumentEquals(arg, "--no-refresh")) {
      config->SetRefresh(false);
    } else {
      UsageError("Unrecognized argument: '%s'", arg);
    }
  }

  if (zygote.empty()) {
    // Use ro.zygote by default, if not overridden by --zygote-arch flag.
    zygote = android::base::GetProperty("ro.zygote", {});
  }
  ZygoteKind zygote_kind;
  if (!ParseZygoteKind(zygote.c_str(), &zygote_kind)) {
    LOG(FATAL) << "Unknown zygote: " << QuotePath(zygote);
  }
  config->SetZygoteKind(zygote_kind);

  return n;
}

void OptionsHelp() {
  UsageError("--dry-run");
  UsageError("--partial-compilation            Only generate artifacts that are out-of-date or");
  UsageError("                                 missing.");
  UsageError("--no-refresh                     Do not refresh existing artifacts.");
  UsageError("--use-compilation-os=<CID>       Run compilation in the VM with the given CID.");
  UsageError("                                 (0 = do not use VM, -1 = use composd's VM)");
  UsageError("--dalvik-cache=<DIR>             Write artifacts to .../<DIR> rather than");
  UsageError("                                 .../dalvik-cache");
  UsageError("--max-execution-seconds=<N>      Maximum timeout of all compilation combined");
  UsageError("--max-child-process-seconds=<N>  Maximum timeout of each compilation task");
  UsageError("--staging-dir=<DIR>              Write temporary artifacts to <DIR> rather than");
  UsageError("                                 .../staging");
  UsageError("--zygote-arch=<STRING>           Zygote kind that overrides ro.zygote");
}

NO_RETURN void UsageHelp(const char* argv0) {
  std::string name(android::base::Basename(argv0));
  UsageError("Usage: %s [OPTION...] ACTION", name.c_str());
  UsageError("On-device refresh tool for boot class path extensions and system server");
  UsageError("following an update of the ART APEX.");
  UsageError("");
  UsageError("Valid ACTION choices are:");
  UsageError("");
  UsageError("--check          Check compilation artifacts are up-to-date based on metadata.");
  UsageError("--compile        Compile boot class path extensions and system_server jars");
  UsageError("                 when necessary.");
  UsageError("--force-compile  Unconditionally compile the boot class path extensions and");
  UsageError("                 system_server jars.");
  UsageError("--help           Display this help information.");
  UsageError("");
  UsageError("Available OPTIONs are:");
  UsageError("");
  OptionsHelp();

  exit(EX_USAGE);
}

}  // namespace

int main(int argc, char** argv) {
  // odrefresh is launched by `init` which sets the umask of forked processed to
  // 077 (S_IRWXG | S_IRWXO). This blocks the ability to make files and directories readable
  // by others and prevents system_server from loading generated artifacts.
  umask(S_IWGRP | S_IWOTH);

  // Explicitly initialize logging (b/201042799).
  android::base::InitLogging(argv, android::base::LogdLogger(android::base::SYSTEM));

  OdrConfig config(argv[0]);
  int n = InitializeConfig(argc, argv, &config);
  argv += n;
  argc -= n;
  if (argc != 1) {
    UsageError("Expected 1 argument, but have %d.", argc);
    exit(EX_USAGE);
  }

  OdrMetrics metrics(config.GetArtifactDirectory());
  OnDeviceRefresh odr(config);

  std::string_view action(argv[0]);
  CompilationOptions compilation_options;
  if (action == "--check") {
    // Fast determination of whether artifacts are up to date.
    return odr.CheckArtifactsAreUpToDate(metrics, &compilation_options);
  } else if (action == "--compile") {
    const ExitCode exit_code = odr.CheckArtifactsAreUpToDate(metrics, &compilation_options);
    if (exit_code != ExitCode::kCompilationRequired) {
      return exit_code;
    }
    OdrCompilationLog compilation_log;
    if (!compilation_log.ShouldAttemptCompile(metrics.GetTrigger())) {
      LOG(INFO) << "Compilation skipped because it was attempted recently";
      return ExitCode::kOkay;
    }
    ExitCode compile_result = odr.Compile(metrics, compilation_options);
    compilation_log.Log(metrics.GetArtApexVersion(),
                        metrics.GetArtApexLastUpdateMillis(),
                        metrics.GetTrigger(),
                        compile_result);
    return compile_result;
  } else if (action == "--force-compile") {
    // Clean-up existing files.
    if (!odr.RemoveArtifactsDirectory()) {
      metrics.SetStatus(OdrMetrics::Status::kIoError);
      return ExitCode::kCleanupFailed;
    }
    return odr.Compile(metrics,
                       CompilationOptions{
                         .compile_boot_extensions_for_isas = config.GetBootExtensionIsas(),
                         .system_server_jars_to_compile = odr.AllSystemServerJars(),
                       });
  } else if (action == "--help") {
    UsageHelp(argv[0]);
  } else {
    UsageError("Unknown argument: ", action);
    exit(EX_USAGE);
  }
}
