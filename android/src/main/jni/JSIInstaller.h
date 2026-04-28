#pragma once

#include <memory>

namespace facebook::jsi { class Runtime; }
namespace facebook::react { class CallInvoker; }

bool installJSIInstaller(
    facebook::jsi::Runtime& rt,
    std::shared_ptr<facebook::react::CallInvoker> jsCallInvoker
);

void uninstallJSIInstaller(facebook::jsi::Runtime& rt);