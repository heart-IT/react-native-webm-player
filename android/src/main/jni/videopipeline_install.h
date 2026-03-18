#pragma once

#include <memory>

namespace facebook::jsi { class Runtime; }
namespace facebook::react { class CallInvoker; }

#ifdef __cplusplus
extern "C" {
#endif

bool installVideoPipeline(facebook::jsi::Runtime& rt,
                           std::shared_ptr<facebook::react::CallInvoker> callInvoker = nullptr);
void uninstallVideoPipeline(facebook::jsi::Runtime& rt);

#ifdef __cplusplus
}
#endif
