/**
 * @file src/platform/windows/vpad_bridge.h
 * @brief virtual input virtual gamepad backend (XInput-compatible pads).
 *
 * When the virtual input pad driver (virtual inputVpad) is installed, every pad exposes a
 * named shared-memory channel. Gamepad state is written there in XInput layout;
 * the driver feeds it to Windows' own xinputhid filter, so games see an Xbox
 * controller. Rumble requested by games comes back through the same channel.
 * Without the driver, callers fall back to libvirtualhid/ViGEm.
 *
 * Contract: the driver's report contract (vendor 0x4C43, product 0x0002).
 */
#pragma once

#include <memory>

#include "src/platform/common.h"

namespace platf::vpad_bridge {

  /**
   * @brief Open channels of the installed virtual input pads.
   */
  class context_t {
  public:
    virtual ~context_t() = default;

    /**
     * @brief Number of pads whose channel is open.
     * @return Open pad count.
     */
    virtual int count() const = 0;

    /**
     * @brief Claim pad `id.globalIndex` for a client gamepad.
     * @param id Gamepad identifiers; the global index selects the pad.
     * @param feedback_queue Queue used to return rumble to the client.
     * @return False when there is no such pad.
     */
    virtual bool alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue) = 0;

    /**
     * @brief Route an allocated pad's rumble to a resumed client.
     * @param id Gamepad identifiers of the resumed client.
     * @param feedback_queue Queue used to return rumble to the client.
     * @return False when the pad is not allocated.
     */
    virtual bool rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue) = 0;

    /**
     * @brief Whether the pad is allocated.
     * @param global_index Global gamepad index.
     * @return True when the pad is allocated.
     */
    virtual bool has(int global_index) = 0;

    /**
     * @brief Write the client's gamepad state to the pad.
     * @param global_index Global gamepad index.
     * @param state Gamepad state sent by the client.
     */
    virtual void update(int global_index, const gamepad_state_t &state) = 0;

    /**
     * @brief Return the pad to neutral and release it.
     * @param global_index Global gamepad index.
     */
    virtual void release(int global_index) = 0;
  };

  /**
   * @brief Open the pads' channels.
   * @return Context, or nullptr when the driver is not installed.
   */
  std::unique_ptr<context_t> open();

}  // namespace platf::vpad_bridge
