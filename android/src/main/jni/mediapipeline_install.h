#pragma once

#include <memory>

namespace facebook::jsi { class Runtime; }
namespace facebook::react { class CallInvoker; }

#ifdef __cplusplus
extern "C" {
#endif

bool installMediaPipeline(facebook::jsi::Runtime& rt,
                          std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
void uninstallMediaPipeline(facebook::jsi::Runtime& rt);

#ifdef __cplusplus
}
#endif
