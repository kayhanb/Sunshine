/**
 * @file src/platform/windows/lecafe_hid.cpp
 * @brief LeCafe virtual HID driver backend (keyboard + mouse).
 */
// platform includes
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

// standard includes
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/windows/lecafe_hid.h"

using namespace std::literals;


namespace platf::lecafe_hid {

  namespace {
    // Contract: apps/agent/drivers/vhid/lecafe_vhid.h
    constexpr USHORT vendor_id = 0x4C43;
    constexpr USHORT product_id = 0x0001;
    constexpr USAGE control_usage_page = 0xFF00;
    constexpr std::uint8_t report_keyboard = 1;
    constexpr std::uint8_t report_mouse = 2;
    constexpr std::uint8_t report_absolute = 3;
    constexpr std::uint8_t report_control = 4;
    constexpr std::size_t control_size = 16;
    constexpr int absolute_max = 32767;

    /**
     * @brief Windows virtual key (US layout, as Moonlight normalizes it) to USB HID usage.
     * @return 0 when unmapped; 0xE0..0xE7 are modifier bits, not array keys.
     */
    std::uint8_t vk_to_usage(std::uint16_t vk) {
      if (vk >= 'A' && vk <= 'Z') {
        return static_cast<std::uint8_t>(4 + (vk - 'A'));
      }
      if (vk >= '1' && vk <= '9') {
        return static_cast<std::uint8_t>(30 + (vk - '1'));
      }
      if (vk >= VK_F1 && vk <= VK_F12) {
        return static_cast<std::uint8_t>(58 + (vk - VK_F1));
      }
      if (vk >= VK_F13 && vk <= VK_F24) {
        return static_cast<std::uint8_t>(104 + (vk - VK_F13));
      }
      if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) {
        return static_cast<std::uint8_t>(89 + (vk - VK_NUMPAD1));
      }
      switch (vk) {
        case '0':
          return 39;
        case VK_RETURN:
          return 40;
        case VK_ESCAPE:
          return 41;
        case VK_BACK:
          return 42;
        case VK_TAB:
          return 43;
        case VK_SPACE:
          return 44;
        case VK_OEM_MINUS:
          return 45;
        case VK_OEM_PLUS:
          return 46;
        case VK_OEM_4:
          return 47;
        case VK_OEM_6:
          return 48;
        case VK_OEM_5:
          return 49;
        case VK_OEM_1:
          return 51;
        case VK_OEM_7:
          return 52;
        case VK_OEM_3:
          return 53;
        case VK_OEM_COMMA:
          return 54;
        case VK_OEM_PERIOD:
          return 55;
        case VK_OEM_2:
          return 56;
        case VK_CAPITAL:
          return 57;
        case VK_SNAPSHOT:
          return 70;
        case VK_SCROLL:
          return 71;
        case VK_PAUSE:
          return 72;
        case VK_INSERT:
          return 73;
        case VK_HOME:
          return 74;
        case VK_PRIOR:
          return 75;
        case VK_DELETE:
          return 76;
        case VK_END:
          return 77;
        case VK_NEXT:
          return 78;
        case VK_RIGHT:
          return 79;
        case VK_LEFT:
          return 80;
        case VK_DOWN:
          return 81;
        case VK_UP:
          return 82;
        case VK_NUMLOCK:
          return 83;
        case VK_DIVIDE:
          return 84;
        case VK_MULTIPLY:
          return 85;
        case VK_SUBTRACT:
          return 86;
        case VK_ADD:
          return 87;
        case VK_NUMPAD0:
          return 98;
        case VK_DECIMAL:
          return 99;
        case VK_OEM_102:
          return 100;
        case VK_APPS:
          return 101;
        case VK_CONTROL:
        case VK_LCONTROL:
          return 0xE0;
        case VK_SHIFT:
        case VK_LSHIFT:
          return 0xE1;
        case VK_MENU:
        case VK_LMENU:
          return 0xE2;
        case VK_LWIN:
          return 0xE3;
        case VK_RCONTROL:
          return 0xE4;
        case VK_RSHIFT:
          return 0xE5;
        case VK_RMENU:
          return 0xE6;
        case VK_RWIN:
          return 0xE7;
        default:
          return 0;
      }
    }

    std::uint8_t button_bit(int button) {
      switch (button) {
        case BUTTON_LEFT:
          return 0x01;
        case BUTTON_RIGHT:
          return 0x02;
        case BUTTON_MIDDLE:
          return 0x04;
        case BUTTON_X1:
          return 0x08;
        case BUTTON_X2:
          return 0x10;
        default:
          return 0;
      }
    }

    /**
     * @brief Whether an open HID handle is the LeCafe control collection.
     */
    bool is_control_collection(HANDLE handle) {
      HIDD_ATTRIBUTES attributes {};
      attributes.Size = sizeof(attributes);
      if (!HidD_GetAttributes(handle, &attributes) || attributes.VendorID != vendor_id || attributes.ProductID != product_id) {
        return false;
      }
      PHIDP_PREPARSED_DATA preparsed = nullptr;
      if (!HidD_GetPreparsedData(handle, &preparsed)) {
        return false;
      }
      HIDP_CAPS caps {};
      const auto status = HidP_GetCaps(preparsed, &caps);
      HidD_FreePreparsedData(preparsed);
      return status == HIDP_STATUS_SUCCESS && caps.UsagePage == control_usage_page && caps.OutputReportByteLength == control_size;
    }
  }  // namespace

  device_t::device_t(void *handle):
      handle_ {handle} {}

  device_t::~device_t() {
    release_all();
    CloseHandle(static_cast<HANDLE>(handle_));
  }

  std::unique_ptr<device_t> device_t::open() {
    GUID guid {};
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
      return nullptr;
    }

    std::unique_ptr<device_t> found;
    SP_DEVICE_INTERFACE_DATA data {};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, index, &data); ++index) {
      DWORD needed = 0;
      SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &needed, nullptr);
      if (needed == 0) {
        continue;
      }
      std::vector<std::uint8_t> buffer(needed);
      auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
      detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
      if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, needed, nullptr, nullptr)) {
        continue;
      }
      HANDLE handle = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        continue;
      }
      if (is_control_collection(handle)) {
        found.reset(new device_t(handle));
        BOOST_LOG(info) << "LeCafe virtual HID driver found; keyboard and mouse use the HID path"sv;
      } else {
        CloseHandle(handle);
      }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
  }

  bool device_t::inject(const std::uint8_t *report, std::size_t length) {
    std::uint8_t frame[control_size] {};
    frame[0] = report_control;
    std::memcpy(frame + 1, report, std::min(length, control_size - 1));
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(handle_), frame, static_cast<DWORD>(sizeof(frame)), &written, nullptr) || written != sizeof(frame)) {
      BOOST_LOG(warning) << "LeCafe virtual HID write failed: "sv << GetLastError();
      return false;
    }
    return true;
  }

  void device_t::send_keyboard() {
    std::uint8_t report[9] {report_keyboard, modifiers_, 0};
    std::memcpy(report + 3, keys_, sizeof(keys_));
    inject(report, sizeof(report));
  }

  void device_t::send_mouse(int dx, int dy, int wheel, int pan) {
    const auto x = static_cast<std::int16_t>(std::clamp(dx, -32767, 32767));
    const auto y = static_cast<std::int16_t>(std::clamp(dy, -32767, 32767));
    const auto w = static_cast<std::int8_t>(std::clamp(wheel, -127, 127));
    const auto p = static_cast<std::int8_t>(std::clamp(pan, -127, 127));
    const std::uint8_t report[8] {
      report_mouse,
      buttons_,
      static_cast<std::uint8_t>(x & 0xFF),
      static_cast<std::uint8_t>((x >> 8) & 0xFF),
      static_cast<std::uint8_t>(y & 0xFF),
      static_cast<std::uint8_t>((y >> 8) & 0xFF),
      static_cast<std::uint8_t>(w),
      static_cast<std::uint8_t>(p),
    };
    inject(report, sizeof(report));
  }

  void device_t::key(std::uint16_t vk, bool pressed) {
    const auto usage = vk_to_usage(vk);
    if (usage == 0) {
      BOOST_LOG(debug) << "LeCafe virtual HID: unmapped virtual key "sv << vk;
      return;
    }
    std::lock_guard lock(mutex_);
    if (usage >= 0xE0) {
      const auto bit = static_cast<std::uint8_t>(1U << (usage - 0xE0));
      modifiers_ = pressed ? (modifiers_ | bit) : (modifiers_ & ~bit);
    } else {
      auto *slot = std::find(std::begin(keys_), std::end(keys_), usage);
      if (pressed) {
        if (slot == std::end(keys_)) {
          slot = std::find(std::begin(keys_), std::end(keys_), 0);
          if (slot == std::end(keys_)) {
            return;  // 6-key rollover exhausted
          }
          *slot = usage;
        }
      } else if (slot != std::end(keys_)) {
        *slot = 0;
      } else {
        return;
      }
    }
    send_keyboard();
  }

  void device_t::move(int dx, int dy) {
    std::lock_guard lock(mutex_);
    send_mouse(dx, dy, 0, 0);
  }

  void device_t::move_absolute(float x, float y, int width, int height) {
    if (width <= 0 || height <= 0) {
      return;
    }
    const auto ax = static_cast<std::uint16_t>(std::clamp(std::lround(x * absolute_max / width), 0L, static_cast<long>(absolute_max)));
    const auto ay = static_cast<std::uint16_t>(std::clamp(std::lround(y * absolute_max / height), 0L, static_cast<long>(absolute_max)));
    std::lock_guard lock(mutex_);
    // Buttons belong to the relative collection only. Repeating them here made
    // Windows see a second "button down" from another device on every move
    // while dragging, so drag-select on the desktop kept restarting (user
    // report 2026-09-08).
    const std::uint8_t report[7] {
      report_absolute,
      0,
      static_cast<std::uint8_t>(ax & 0xFF),
      static_cast<std::uint8_t>(ax >> 8),
      static_cast<std::uint8_t>(ay & 0xFF),
      static_cast<std::uint8_t>(ay >> 8),
      0,
    };
    inject(report, sizeof(report));
  }

  void device_t::button(int button, bool pressed) {
    const auto bit = button_bit(button);
    if (bit == 0) {
      return;
    }
    std::lock_guard lock(mutex_);
    buttons_ = pressed ? (buttons_ | bit) : (buttons_ & ~bit);
    send_mouse(0, 0, 0, 0);
  }

  void device_t::scroll(int high_res_distance) {
    std::lock_guard lock(mutex_);
    wheel_remainder_ += high_res_distance;
    const int detents = wheel_remainder_ / WHEEL_DELTA;
    if (detents == 0) {
      return;
    }
    wheel_remainder_ -= detents * WHEEL_DELTA;
    send_mouse(0, 0, detents, 0);
  }

  void device_t::hscroll(int high_res_distance) {
    std::lock_guard lock(mutex_);
    pan_remainder_ += high_res_distance;
    const int detents = pan_remainder_ / WHEEL_DELTA;
    if (detents == 0) {
      return;
    }
    pan_remainder_ -= detents * WHEEL_DELTA;
    send_mouse(0, 0, 0, detents);
  }

  void device_t::release_all() {
    std::lock_guard lock(mutex_);
    if (modifiers_ != 0 || std::any_of(std::begin(keys_), std::end(keys_), [](auto k) { return k != 0; })) {
      modifiers_ = 0;
      std::memset(keys_, 0, sizeof(keys_));
      send_keyboard();
    }
    if (buttons_ != 0) {
      buttons_ = 0;
      send_mouse(0, 0, 0, 0);
    }
  }

}  // namespace platf::lecafe_hid
