#pragma once

namespace axvsdk::common::internal {

bool EnsureAxclThreadContext(int device_id = -1) noexcept;
void ReleaseAxclThreadContext() noexcept;

}  // namespace axvsdk::common::internal
