/**
 * @file src/platform/windows/lecafe_hid.h
 * @brief LeCafe virtual HID driver backend (keyboard + mouse).
 *
 * When the LeCafe UMDF HID minidriver (LeCafeVhid) is installed, keyboard and
 * mouse input is written to its control collection as HID input reports and
 * therefore enters the input stack like a physical USB keyboard/mouse. Games
 * guarded by anti-cheat (XIGNCODE3, Vanguard, ...) drop SendInput-injected
 * events; they accept this path. Without the driver, callers fall back to the
 * regular libvirtualhid/SendInput route.
 *
 * Report contract: apps/agent/drivers/vhid/lecafe_vhid.h in the LeCafe repo.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "src/platform/common.h"

namespace platf::lecafe_hid {

  /**
   * @brief Open handle to the driver's control collection plus keyboard/mouse state.
   */
  class device_t {
  public:
    ~device_t();

    /**
     * @brief Find and open the LeCafe control collection.
     * @return Device, or nullptr when the driver is not installed.
     */
    static std::unique_ptr<device_t> open();

    /**
     * @brief Press or release a key.
     * @param vk Windows virtual key code (US layout as normalized by Moonlight).
     * @param pressed True on key down, false on key up.
     */
    void key(std::uint16_t vk, bool pressed);

    /**
     * @brief Relative mouse motion.
     * @param dx Horizontal delta in pixels.
     * @param dy Vertical delta in pixels.
     */
    void move(int dx, int dy);

    /**
     * @brief Absolute mouse motion.
     * @param x Horizontal position in the coordinate space of `width`.
     * @param y Vertical position in the coordinate space of `height`.
     * @param width Width of the coordinate space.
     * @param height Height of the coordinate space.
     */
    void move_absolute(float x, float y, int width, int height);

    /**
     * @brief Press or release a mouse button.
     * @param button Moonlight button identifier (BUTTON_LEFT, ...).
     * @param pressed True on button down, false on button up.
     */
    void button(int button, bool pressed);

    /**
     * @brief Vertical scroll.
     * @param high_res_distance Distance in 1/120 detent units.
     */
    void scroll(int high_res_distance);

    /**
     * @brief Horizontal scroll.
     * @param high_res_distance Distance in 1/120 detent units.
     */
    void hscroll(int high_res_distance);

    /**
     * @brief Release every pressed key and button (client disconnect).
     */
    void release_all();

  private:
    explicit device_t(void *handle);
    bool inject(const std::uint8_t *report, std::size_t length);
    void send_keyboard();
    void send_mouse(int dx, int dy, int wheel, int pan);

    void *handle_;
    std::mutex mutex_;
    std::uint8_t modifiers_ = 0;
    std::uint8_t keys_[6] {};
    std::uint8_t buttons_ = 0;
    int wheel_remainder_ = 0;
    int pan_remainder_ = 0;
  };

  /**
   * @brief Get the device attached to a platform input context.
   * @param input Platform input context.
   * @return The device, or nullptr when the driver is not installed.
   */
  device_t *get(input_t &input);

}  // namespace platf::lecafe_hid
