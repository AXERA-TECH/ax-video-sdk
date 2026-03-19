#include "common/ax_drawer.h"

#include "ax_drawer_internal.h"

namespace axvsdk::common {

std::unique_ptr<AxDrawer> CreateDrawer() {
    return internal::CreatePlatformDrawer();
}

}  // namespace axvsdk::common
