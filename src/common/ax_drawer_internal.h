#pragma once

#include <memory>

#include "common/ax_drawer.h"

namespace axvsdk::common::internal {

std::unique_ptr<AxDrawer> CreatePlatformDrawer();

}  // namespace axvsdk::common::internal
