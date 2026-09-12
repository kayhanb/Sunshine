/**
 * @file src/platform/windows/lecafe_pad.h
 * @brief LeCafe virtual gamepad backend (XInput-compatible pads).
 *
 * When the LeCafe pad driver (LeCafeVpad) is installed, every pad exposes a
 * named shared-memory channel. Gamepad state is written there in XInput layout;
 * the driver feeds it to Windows' own xinputhid filter, so games see an Xbox
 * controller. Rumble requested by games comes back through the same channel.
 * Without the driver, callers fall back to libvirtualhid/ViGEm.
 *
 * Contract: apps/agent/drivers/vpad/lecafe_vpad.h in the LeCafe repo (ADR 0047).
 */
#pragma once

#include <memory>

#include "src/platform/common.h"

namespace platf::lecafe_pad {

  /**
   * @brief Open channels of the installed LeCafe pads.
   */
  class context_t {
  public:
    virtual ~context_t() = default;

    /**
     * @brief Number of pads whose channel is open.
     */
    virtual int count() const = 0;

    /**
     * @brief Claim pad `id.globalIndex` for a client gamepad.
     * @return False when there is no such pad.
     */
    virtual bool alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue) = 0;

    /**
     * @brief Route an allocated pad's rumble to a resumed client.
     */
    virtual bool rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue) = 0;

    /**
     * @brief Whether the pad is allocated.
     */
    virtual bool has(int global_index) = 0;

    /**
     * @brief Write the client's gamepad state to the pad.
     */
    virtual void update(int global_index, const gamepad_state_t &state) = 0;

    /**
     * @brief Return the pad to neutral and release it.
     */
    virtual void release(int global_index) = 0;
  };

  /**
   * @brief Open the pads' channels.
   * @return Context, or nullptr when the driver is not installed.
   */
  std::unique_ptr<context_t> open();

}  // namespace platf::lecafe_pad
