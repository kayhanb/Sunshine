/**
 * @file src/platform/windows/lecafe_pad.cpp
 * @brief LeCafe virtual gamepad backend (XInput-compatible pads).
 */
// platform includes
#include <windows.h>

// standard includes
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "src/logging.h"
#include "src/platform/windows/lecafe_pad.h"

using namespace std::literals;

namespace platf::lecafe_pad {

  namespace {
    // Contract: apps/agent/drivers/vpad/lecafe_vpad.h
    constexpr std::uint32_t shared_magic = 0x5056434C;  // "LCVP"
    constexpr std::uint16_t shared_layout_version = 1;
    constexpr std::size_t shared_size = 64;
    constexpr int max_pads = 10;

#pragma pack(push, 1)

    struct shared_t {
      std::uint32_t magic;
      std::uint16_t layout_version;
      std::uint16_t pad_index;
      std::uint32_t input_sequence;  // written by us (seqlock)
      std::uint16_t buttons;  // XInput wButtons
      std::uint8_t left_trigger;
      std::uint8_t right_trigger;
      std::int16_t thumb_lx;
      std::int16_t thumb_ly;  // up is positive
      std::int16_t thumb_rx;
      std::int16_t thumb_ry;
      std::uint32_t rumble_sequence;  // written by the driver (seqlock)
      std::uint8_t rumble_left;  // large motor, 0..100
      std::uint8_t rumble_right;  // small motor, 0..100
      std::uint8_t rumble_left_trigger;
      std::uint8_t rumble_right_trigger;
      std::uint8_t rumble_raw_length;
      std::uint8_t rumble_raw[9];
    };

#pragma pack(pop)

    static_assert(offsetof(shared_t, buttons) == 12);
    static_assert(offsetof(shared_t, rumble_sequence) == 24);
    static_assert(sizeof(shared_t) == 42);

    /**
     * @brief Moonlight button flags to XInput wButtons (0x0400 = Xbox button).
     */
    std::uint16_t xinput_buttons(std::uint32_t flags) {
      constexpr std::array<std::pair<std::uint32_t, std::uint16_t>, 15> map {{
        {DPAD_UP, 0x0001},
        {DPAD_DOWN, 0x0002},
        {DPAD_LEFT, 0x0004},
        {DPAD_RIGHT, 0x0008},
        {START, 0x0010},
        {BACK, 0x0020},
        {LEFT_STICK, 0x0040},
        {RIGHT_STICK, 0x0080},
        {LEFT_BUTTON, 0x0100},
        {RIGHT_BUTTON, 0x0200},
        {HOME | MISC_BUTTON, 0x0400},
        {A, 0x1000},
        {B, 0x2000},
        {X, 0x4000},
        {Y, 0x8000},
      }};
      unsigned buttons = 0;
      for (const auto &[moonlight, xinput] : map) {
        if (flags & moonlight) {
          buttons |= xinput;
        }
      }
      return static_cast<std::uint16_t>(buttons);
    }

    std::uint16_t magnitude_to_u16(std::uint8_t magnitude) {
      return static_cast<std::uint16_t>(std::min<unsigned>(magnitude, 100) * 65535u / 100u);
    }

    std::uint32_t read_sequence(const std::uint32_t &sequence) {
      const volatile std::uint32_t &value = sequence;
      return value;
    }

    struct pad_t {
      HANDLE section = nullptr;
      shared_t *shared = nullptr;
      HANDLE input_event = nullptr;
      HANDLE rumble_event = nullptr;
      std::mutex mutex;
      bool allocated = false;
      std::uint16_t client_relative_index = 0;
      feedback_queue_t feedback_queue;
      std::uint16_t last_low = 0;
      std::uint16_t last_high = 0;
      std::uint16_t last_trigger_left = 0;
      std::uint16_t last_trigger_right = 0;
    };

    void close_pad(pad_t &pad) {
      if (pad.shared) {
        UnmapViewOfFile(pad.shared);
        pad.shared = nullptr;
      }
      for (auto handle : {&pad.section, &pad.input_event, &pad.rumble_event}) {
        if (*handle) {
          CloseHandle(*handle);
          *handle = nullptr;
        }
      }
    }

    bool open_pad(int index, pad_t &pad) {
      const std::wstring base = L"Global\\LeCafeVpad" + std::to_wstring(index);
      pad.section = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, base.c_str());
      if (!pad.section) {
        return false;
      }
      pad.shared = static_cast<shared_t *>(MapViewOfFile(pad.section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, shared_size));
      if (!pad.shared || pad.shared->magic != shared_magic || pad.shared->layout_version != shared_layout_version) {
        BOOST_LOG(warning) << "LeCafe pad "sv << index << ": unexpected channel layout, pad skipped"sv;
        close_pad(pad);
        return false;
      }
      pad.input_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (base + L".input").c_str());
      pad.rumble_event = OpenEventW(SYNCHRONIZE, FALSE, (base + L".rumble").c_str());
      if (!pad.input_event || !pad.rumble_event) {
        BOOST_LOG(warning) << "LeCafe pad "sv << index << ": channel events missing ["sv << GetLastError() << ']';
        close_pad(pad);
        return false;
      }
      return true;
    }

    /**
     * @brief Seqlock writer: odd sequence while the fields are inconsistent.
     */
    void write_state(pad_t &pad, const gamepad_state_t &state) {
      auto *shared = pad.shared;
      volatile std::uint32_t &sequence = shared->input_sequence;
      const std::uint32_t base = sequence & ~1u;
      sequence = base + 1;
      shared->buttons = xinput_buttons(state.buttonFlags);
      shared->left_trigger = state.lt;
      shared->right_trigger = state.rt;
      shared->thumb_lx = state.lsX;
      shared->thumb_ly = state.lsY;
      shared->thumb_rx = state.rsX;
      shared->thumb_ry = state.rsY;
      sequence = base + 2;
      SetEvent(pad.input_event);
    }

    class context_impl_t: public context_t {
    public:
      ~context_impl_t() override {
        if (stop_event) {
          SetEvent(stop_event);
        }
        if (rumble_thread.joinable()) {
          rumble_thread.join();
        }
        if (stop_event) {
          CloseHandle(stop_event);
        }
        for (int i = 0; i < opened; ++i) {
          write_state(pads[i], {});
          close_pad(pads[i]);
        }
      }

      bool start() {
        while (opened < max_pads && open_pad(opened, pads[opened])) {
          ++opened;
        }
        if (opened == 0) {
          return false;
        }
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event) {
          return false;
        }
        rumble_thread = std::thread {&context_impl_t::rumble_loop, this};
        return true;
      }

      int count() const override {
        return opened;
      }

      bool alloc(const gamepad_id_t &id, feedback_queue_t feedback_queue) override {
        if (id.globalIndex < 0 || id.globalIndex >= opened) {
          return false;
        }
        auto &pad = pads[id.globalIndex];
        std::lock_guard lock {pad.mutex};
        pad.allocated = true;
        pad.client_relative_index = static_cast<std::uint16_t>(id.clientRelativeIndex);
        pad.feedback_queue = std::move(feedback_queue);
        pad.last_low = pad.last_high = pad.last_trigger_left = pad.last_trigger_right = 0;
        write_state(pad, {});
        return true;
      }

      bool rebind(const gamepad_id_t &id, feedback_queue_t feedback_queue) override {
        if (id.globalIndex < 0 || id.globalIndex >= opened) {
          return false;
        }
        auto &pad = pads[id.globalIndex];
        std::lock_guard lock {pad.mutex};
        if (!pad.allocated) {
          return false;
        }
        pad.client_relative_index = static_cast<std::uint16_t>(id.clientRelativeIndex);
        pad.feedback_queue = std::move(feedback_queue);
        pad.last_low = pad.last_high = pad.last_trigger_left = pad.last_trigger_right = 0;
        return true;
      }

      bool has(int global_index) override {
        if (global_index < 0 || global_index >= opened) {
          return false;
        }
        std::lock_guard lock {pads[global_index].mutex};
        return pads[global_index].allocated;
      }

      void update(int global_index, const gamepad_state_t &state) override {
        if (global_index < 0 || global_index >= opened) {
          return;
        }
        auto &pad = pads[global_index];
        std::lock_guard lock {pad.mutex};
        if (pad.allocated) {
          write_state(pad, state);
        }
      }

      void release(int global_index) override {
        if (global_index < 0 || global_index >= opened) {
          return;
        }
        auto &pad = pads[global_index];
        std::lock_guard lock {pad.mutex};
        write_state(pad, {});
        pad.allocated = false;
        pad.feedback_queue = feedback_queue_t {};
      }

    private:
      void rumble_loop() {
        std::vector<HANDLE> waits {stop_event};
        for (int i = 0; i < opened; ++i) {
          waits.push_back(pads[i].rumble_event);
        }
        for (;;) {
          const DWORD result = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
          if (result <= WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + waits.size()) {
            return;
          }
          publish_rumble(pads[result - WAIT_OBJECT_0 - 1]);
        }
      }

      static void publish_rumble(pad_t &pad) {
        std::uint8_t left = 0;
        std::uint8_t right = 0;
        std::uint8_t trigger_left = 0;
        std::uint8_t trigger_right = 0;
        bool consistent = false;
        for (int attempt = 0; attempt < 64 && !consistent; ++attempt) {
          const std::uint32_t before = read_sequence(pad.shared->rumble_sequence);
          if (before & 1) {
            YieldProcessor();
            continue;
          }
          left = pad.shared->rumble_left;
          right = pad.shared->rumble_right;
          trigger_left = pad.shared->rumble_left_trigger;
          trigger_right = pad.shared->rumble_right_trigger;
          consistent = read_sequence(pad.shared->rumble_sequence) == before;
        }
        if (!consistent) {
          return;
        }

        std::lock_guard lock {pad.mutex};
        if (!pad.allocated || !pad.feedback_queue) {
          return;
        }
        const auto low = magnitude_to_u16(left);
        const auto high = magnitude_to_u16(right);
        if (low != pad.last_low || high != pad.last_high) {
          pad.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble(pad.client_relative_index, low, high));
          pad.last_low = low;
          pad.last_high = high;
        }
        const auto tl = magnitude_to_u16(trigger_left);
        const auto tr = magnitude_to_u16(trigger_right);
        if (tl != pad.last_trigger_left || tr != pad.last_trigger_right) {
          pad.feedback_queue->raise(gamepad_feedback_msg_t::make_rumble_triggers(pad.client_relative_index, tl, tr));
          pad.last_trigger_left = tl;
          pad.last_trigger_right = tr;
        }
      }

      std::array<pad_t, max_pads> pads;
      int opened = 0;
      HANDLE stop_event = nullptr;
      std::thread rumble_thread;
    };
  }  // namespace

  std::unique_ptr<context_t> open() {
    auto context = std::make_unique<context_impl_t>();
    if (!context->start()) {
      return nullptr;
    }
    BOOST_LOG(info) << "LeCafe pad driver: "sv << context->count() << " XInput pad(s) available"sv;
    return context;
  }

}  // namespace platf::lecafe_pad
