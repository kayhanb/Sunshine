/**
 * @file src/platform/windows/display_shared.cpp
 * @brief Definitions for capture from a shared frame source published by an indirect display driver.
 */
// platform includes
#include <d3d11_1.h>
#include <dxgi1_2.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "utf_utils.h"

namespace platf {
  using namespace std::literals;
}

namespace platf::dxgi {
  /**
   * @brief Header the driver keeps in the `<name>Meta` file mapping.
   *
   * The layout is the contract with the driver: 64 bytes, little endian.
   */
  struct shared_frame_header_t {
    std::uint32_t magic;  ///< FRAME_HEADER_MAGIC once the current texture holds a frame.
    std::uint32_t version;  ///< Layout version.
    std::uint32_t width;  ///< Texture width in pixels.
    std::uint32_t height;  ///< Texture height in pixels.
    std::uint32_t format;  ///< Texture DXGI_FORMAT.
    std::uint32_t generation;  ///< Suffix of the texture name; changes when the driver recreates it.
    std::uint64_t frame;  ///< Count of published frames.
    std::int64_t qpc;  ///< Performance counter value taken right after the last frame was copied.
    std::uint64_t skipped;  ///< Frames the driver did not publish because a reader held the texture.
    LUID adapter;  ///< Adapter the texture was created on.
    std::uint64_t reserved;  ///< Unused.
  };
  static_assert(sizeof(shared_frame_header_t) == 64, "the shared frame header layout is fixed");

  namespace {
    constexpr std::uint32_t FRAME_HEADER_MAGIC = 0x5846434C;
    constexpr std::uint32_t FRAME_HEADER_VERSION = 1;
    /// How long init() waits for the driver to publish the source and its first frame.
    constexpr auto SOURCE_WAIT = 2500ms;
    /// How long a frame may wait for the driver to finish copying into the texture.
    constexpr DWORD TEXTURE_LOCK_WAIT_MS = 8;

    /**
     * @brief Make the desktop produce a frame.
     *
     * An indirect display only receives a frame when the desktop changes. Every
     * window is invalidated and the pointer is moved by one pixel; the next call
     * moves it back.
     *
     * @param forward Direction of the one pixel pointer move.
     */
    void nudge_desktop(bool forward) {
      RedrawWindow(nullptr, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

      INPUT move {};
      move.type = INPUT_MOUSE;
      move.mi.dx = forward ? 1 : -1;
      move.mi.dwFlags = MOUSEEVENTF_MOVE;
      SendInput(1, &move, sizeof(move));
    }
  }  // namespace

  shared_capture_t::~shared_capture_t() {
    release_frame();
    mutex.reset();
    texture.reset();
    if (header) {
      UnmapViewOfFile(const_cast<shared_frame_header_t *>(header));
    }
    if (frame_event) {
      CloseHandle(frame_event);
    }
    if (mapping) {
      CloseHandle(mapping);
    }
  }

  int shared_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    (void) config;
    const auto &base = config::video.shared_capture_name;
    if (base.empty()) {
      BOOST_LOG(error) << "Shared capture: shared_capture_name is not set"sv;
      return -1;
    }

    const auto prefix = L"Global\\"s + utf_utils::from_utf8(base);
    const auto meta_name = prefix + L"Meta";
    const auto event_name = prefix + L"Event";

    // The driver creates the header and the event when it notices that publishing
    // was turned on, and marks the header valid with the next frame. A still screen
    // produces no frame, so the desktop is asked to change until one arrives.
    DWORD last_error = ERROR_SUCCESS;
    bool forward = true;
    const auto deadline = std::chrono::steady_clock::now() + SOURCE_WAIT;
    for (;;) {
      if (!mapping && !(mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, meta_name.c_str()))) {
        last_error = GetLastError();
      }
      if (mapping && !header) {
        header = static_cast<const shared_frame_header_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(shared_frame_header_t)));
        if (!header) {
          BOOST_LOG(error) << "Shared capture: failed to map the frame header [0x"sv << util::hex(GetLastError()).to_string_view() << ']';
          return -1;
        }
      }
      if (header && !frame_event && !(frame_event = OpenEventW(SYNCHRONIZE, FALSE, event_name.c_str()))) {
        last_error = GetLastError();
      }
      if (frame_event && header->magic == FRAME_HEADER_MAGIC) {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        if (!frame_event) {
          BOOST_LOG(error) << "Shared capture: frame source ["sv << base << "] is not published [0x"sv << util::hex(last_error).to_string_view() << ']';
        } else {
          BOOST_LOG(error) << "Shared capture: frame source ["sv << base << "] has not published a frame"sv;
        }
        return -1;
      }
      nudge_desktop(forward);
      forward = !forward;
      Sleep(50);
    }
    if (header->version != FRAME_HEADER_VERSION) {
      BOOST_LOG(error) << "Shared capture: unsupported frame header version ["sv << header->version << ']';
      return -1;
    }

    // A shared texture opens only on a device of the adapter it was created on.
    DXGI_ADAPTER_DESC adapter_desc;
    display->adapter->GetDesc(&adapter_desc);
    const LONG source_high = header->adapter.HighPart;
    const DWORD source_low = header->adapter.LowPart;
    if (adapter_desc.AdapterLuid.HighPart != source_high || adapter_desc.AdapterLuid.LowPart != source_low) {
      BOOST_LOG(error) << "Shared capture: the frame source is on another adapter [source "sv << util::hex(source_low).to_string_view() << ", capture "sv << util::hex(adapter_desc.AdapterLuid.LowPart).to_string_view() << ']';
      return -1;
    }

    HRESULT status;
    device1_t device1;
    if (FAILED(status = display->device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1))) {
      BOOST_LOG(error) << "Shared capture: failed to query ID3D11Device1 [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    generation = header->generation;
    const auto texture_name = prefix + L"-" + std::to_wstring(generation);
    if (FAILED(status = device1->OpenSharedResourceByName(texture_name.c_str(), DXGI_SHARED_RESOURCE_READ, __uuidof(ID3D11Texture2D), (void **) &texture))) {
      BOOST_LOG(error) << "Shared capture: failed to open the frame texture [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }
    if (FAILED(status = texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &mutex))) {
      BOOST_LOG(error) << "Shared capture: failed to query the texture mutex [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    display->capture_format = static_cast<DXGI_FORMAT>(static_cast<std::uint32_t>(header->format));
    first_frame = true;

    BOOST_LOG(info) << "Shared capture: reading frame source ["sv << base << "] "sv << header->width << 'x' << header->height;
    return 0;
  }

  capture_e shared_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    release_frame();

    // The driver clears the header when its swap chain ends and bumps the
    // generation when it recreates the texture (mode change).
    if (header->magic != FRAME_HEADER_MAGIC || header->generation != generation) {
      return capture_e::reinit;
    }

    // The texture already holds the last published frame: a new session must not
    // wait for the screen to change before it has anything to show.
    if (!first_frame) {
      const auto wait = WaitForSingleObject(frame_event, static_cast<DWORD>(timeout.count()));
      if (wait == WAIT_TIMEOUT) {
        return capture_e::timeout;
      }
      if (wait != WAIT_OBJECT_0) {
        return capture_e::error;
      }
      if (header->magic != FRAME_HEADER_MAGIC || header->generation != generation) {
        return capture_e::reinit;
      }
    }

    const auto status = mutex->AcquireSync(0, TEXTURE_LOCK_WAIT_MS);
    if (status == static_cast<HRESULT>(WAIT_TIMEOUT)) {
      return capture_e::timeout;
    }
    if (status != S_OK) {
      BOOST_LOG(warning) << "Shared capture: failed to lock the frame texture [0x"sv << util::hex(status).to_string_view() << ']';
      return capture_e::reinit;
    }
    locked = true;
    first_frame = false;

    texture->AddRef();
    *out = texture.get();
    out_time = static_cast<uint64_t>(static_cast<std::int64_t>(header->qpc));
    return capture_e::ok;
  }

  capture_e shared_capture_t::release_frame() {
    if (locked) {
      mutex->ReleaseSync(0);
      locked = false;
    }
    return capture_e::ok;
  }
}  // namespace platf::dxgi
