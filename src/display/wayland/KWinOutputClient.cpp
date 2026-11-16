#include "KWinOutputClient.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <tuple>

// The generated protocol headers use C casts in their inline functions.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include <wayland-client.h>
#include "kde-output-device-v2-client-protocol.h"
#include "kde-output-management-v2-client-protocol.h"
#pragma GCC diagnostic pop

// From drm_mode.h; the cvt event carries DRM_MODE_FLAG_* values.
static const uint32_t DRM_MODE_FLAG_INTERLACE_VALUE = (1 << 4);

// The registry refuses to be bound below this version (protocol error).
static const uint32_t MIN_DEVICE_REGISTRY_VERSION = 21;

///////////////////////////////////////////////////////////////////////////////////////////////////
struct KWinOutputClient::DeviceMode
{
  KWinOutputClient::Device* device = nullptr;
  kde_output_device_mode_v2* proxy = nullptr;
  uint32_t serial = 0;
  int width = 0;
  int height = 0;
  int refresh = 0;
  bool preferred = false;
  bool interlaced = false;
  bool removed = false;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
struct KWinOutputClient::Device
{
  KWinOutputClient* client = nullptr;
  kde_output_device_v2* proxy = nullptr;
  uint32_t version = 0;
  std::string name;
  std::string uuid;
  bool enabled = false;
  int x = 0;
  int y = 0;
  double scale = 1.0;
  int transform = 0;
  uint32_t priority = 0;
  std::vector<std::unique_ptr<DeviceMode>> modes;
  DeviceMode* current = nullptr;
  uint32_t nextModeSerial = 0;
  bool announced = false;
  bool removed = false;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
static std::tuple<int, int, int, bool> modeKey(int width, int height, int refresh, bool interlaced)
{
  return std::make_tuple(width, height, refresh, interlaced);
}

static std::tuple<int, int, int, bool> modeKey(const KWinOutputClient::Mode& mode)
{
  return modeKey(mode.width, mode.height, mode.refreshMilliHz, mode.interlaced);
}

static std::tuple<int, int, int, bool> modeKey(const KWinOutputClient::DeviceMode* mode)
{
  return modeKey(mode->width, mode->height, mode->refresh, mode->interlaced);
}

static std::string describeMode(int width, int height, int refresh)
{
  return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(refresh) + "mHz";
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// kde_output_device_mode_v2
///////////////////////////////////////////////////////////////////////////////////////////////////
static kde_output_device_mode_v2_listener makeModeListener()
{
  kde_output_device_mode_v2_listener l{};
  l.size = [](void* data, kde_output_device_mode_v2*, int32_t width, int32_t height)
  {
    auto* mode = static_cast<KWinOutputClient::DeviceMode*>(data);
    mode->width = width;
    mode->height = height;
  };
  l.refresh = [](void* data, kde_output_device_mode_v2*, int32_t refresh)
  {
    static_cast<KWinOutputClient::DeviceMode*>(data)->refresh = refresh;
  };
  l.preferred = [](void* data, kde_output_device_mode_v2*)
  {
    static_cast<KWinOutputClient::DeviceMode*>(data)->preferred = true;
  };
  l.removed = [](void* data, kde_output_device_mode_v2*)
  {
    // The proxy is released later, outside of event dispatching.
    auto* mode = static_cast<KWinOutputClient::DeviceMode*>(data);
    mode->removed = true;
    if (mode->device->current == mode)
      mode->device->current = nullptr;
  };
  l.flags = [](void*, kde_output_device_mode_v2*, uint32_t) {};
  l.cvt = [](void* data, kde_output_device_mode_v2*, uint32_t, uint32_t, uint32_t, uint32_t,
             uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t flags)
  {
    static_cast<KWinOutputClient::DeviceMode*>(data)->interlaced = (flags & DRM_MODE_FLAG_INTERLACE_VALUE) != 0;
  };
  return l;
}

// libwayland aborts on a NULL listener slot: every event must be handled.
static_assert(sizeof(kde_output_device_mode_v2_listener) == 6 * sizeof(void (*)()),
              "kde_output_device_mode_v2 has new events, update makeModeListener()");
static const kde_output_device_mode_v2_listener s_modeListener = makeModeListener();

///////////////////////////////////////////////////////////////////////////////////////////////////
// kde_output_device_v2
///////////////////////////////////////////////////////////////////////////////////////////////////
static KWinOutputClient::Device* asDevice(void* data)
{
  return static_cast<KWinOutputClient::Device*>(data);
}

static kde_output_device_v2_listener makeDeviceListener()
{
  kde_output_device_v2_listener l{};
  l.geometry = [](void* data, kde_output_device_v2*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                  const char*, const char*, int32_t transform)
  {
    asDevice(data)->x = x;
    asDevice(data)->y = y;
    asDevice(data)->transform = transform;
  };
  l.current_mode = [](void* data, kde_output_device_v2*, kde_output_device_mode_v2* mode)
  {
    KWinOutputClient::DeviceMode* current = nullptr;
    if (mode)
      current = static_cast<KWinOutputClient::DeviceMode*>(
        wl_proxy_get_user_data(reinterpret_cast<wl_proxy*>(mode)));
    asDevice(data)->current = current;
  };
  l.mode = [](void* data, kde_output_device_v2*, kde_output_device_mode_v2* proxy)
  {
    KWinOutputClient::Device* device = asDevice(data);
    auto mode = std::unique_ptr<KWinOutputClient::DeviceMode>(new KWinOutputClient::DeviceMode);
    mode->device = device;
    mode->proxy = proxy;
    mode->serial = device->nextModeSerial++;
    kde_output_device_mode_v2_add_listener(proxy, &s_modeListener, mode.get());
    device->modes.push_back(std::move(mode));
  };
  l.done = [](void* data, kde_output_device_v2*)
  {
    KWinOutputClient::Device* device = asDevice(data);
    device->announced = true;
    device->client->onDeviceDone(device);
  };
  l.scale = [](void* data, kde_output_device_v2*, wl_fixed_t factor)
  {
    asDevice(data)->scale = wl_fixed_to_double(factor);
  };
  l.edid = [](void*, kde_output_device_v2*, const char*) {};
  l.enabled = [](void* data, kde_output_device_v2*, int32_t enabled)
  {
    asDevice(data)->enabled = enabled != 0;
  };
  l.uuid = [](void* data, kde_output_device_v2*, const char* uuid)
  {
    asDevice(data)->uuid = uuid ? uuid : "";
  };
  l.serial_number = [](void*, kde_output_device_v2*, const char*) {};
  l.eisa_id = [](void*, kde_output_device_v2*, const char*) {};
  l.capabilities = [](void*, kde_output_device_v2*, uint32_t) {};
  l.overscan = [](void*, kde_output_device_v2*, uint32_t) {};
  l.vrr_policy = [](void*, kde_output_device_v2*, uint32_t) {};
  l.rgb_range = [](void*, kde_output_device_v2*, uint32_t) {};
  l.name = [](void* data, kde_output_device_v2*, const char* name)
  {
    asDevice(data)->name = name ? name : "";
  };
  l.high_dynamic_range = [](void*, kde_output_device_v2*, uint32_t) {};
  l.sdr_brightness = [](void*, kde_output_device_v2*, uint32_t) {};
  l.wide_color_gamut = [](void*, kde_output_device_v2*, uint32_t) {};
  l.auto_rotate_policy = [](void*, kde_output_device_v2*, uint32_t) {};
  l.icc_profile_path = [](void*, kde_output_device_v2*, const char*) {};
  l.brightness_metadata = [](void*, kde_output_device_v2*, uint32_t, uint32_t, uint32_t) {};
  l.brightness_overrides = [](void*, kde_output_device_v2*, int32_t, int32_t, int32_t) {};
  l.sdr_gamut_wideness = [](void*, kde_output_device_v2*, uint32_t) {};
  l.color_profile_source = [](void*, kde_output_device_v2*, uint32_t) {};
  l.brightness = [](void*, kde_output_device_v2*, uint32_t) {};
  l.color_power_tradeoff = [](void*, kde_output_device_v2*, uint32_t) {};
  l.dimming = [](void*, kde_output_device_v2*, uint32_t) {};
  l.replication_source = [](void*, kde_output_device_v2*, const char*) {};
  l.ddc_ci_allowed = [](void*, kde_output_device_v2*, uint32_t) {};
  l.max_bits_per_color = [](void*, kde_output_device_v2*, uint32_t) {};
  l.max_bits_per_color_range = [](void*, kde_output_device_v2*, uint32_t, uint32_t) {};
  l.automatic_max_bits_per_color_limit = [](void*, kde_output_device_v2*, uint32_t) {};
  l.edr_policy = [](void*, kde_output_device_v2*, uint32_t) {};
  l.sharpness = [](void*, kde_output_device_v2*, uint32_t) {};
  l.priority = [](void* data, kde_output_device_v2*, uint32_t priority)
  {
    asDevice(data)->priority = priority;
  };
  l.auto_brightness = [](void*, kde_output_device_v2*, uint32_t) {};
  l.removed = [](void* data, kde_output_device_v2*)
  {
    // The proxy is released later, outside of event dispatching.
    KWinOutputClient::Device* device = asDevice(data);
    device->removed = true;
    device->client->onDeviceDone(device);
  };
  l.hdr_icc_profile_path = [](void*, kde_output_device_v2*, const char*) {};
  l.hdr_color_profile_source = [](void*, kde_output_device_v2*, uint32_t) {};
  l.abm_level = [](void*, kde_output_device_v2*, uint32_t) {};
  return l;
}

static_assert(sizeof(kde_output_device_v2_listener) == 40 * sizeof(void (*)()),
              "kde_output_device_v2 has new events, update makeDeviceListener()");
static const kde_output_device_v2_listener s_deviceListener = makeDeviceListener();

///////////////////////////////////////////////////////////////////////////////////////////////////
// kde_output_device_registry_v2
///////////////////////////////////////////////////////////////////////////////////////////////////
static kde_output_device_registry_v2_listener makeDeviceRegistryListener()
{
  kde_output_device_registry_v2_listener l{};
  // Only sent in answer to stop, which we send right before destroying the
  // proxy and disconnecting, so it is never dispatched.
  l.finished = [](void*, kde_output_device_registry_v2*) {};
  l.output = [](void* data, kde_output_device_registry_v2*, kde_output_device_v2* proxy)
  {
    auto* client = static_cast<KWinOutputClient*>(data);
    auto* device = new KWinOutputClient::Device;
    device->client = client;
    device->proxy = proxy;
    device->version = wl_proxy_get_version(reinterpret_cast<wl_proxy*>(proxy));
    kde_output_device_v2_add_listener(proxy, &s_deviceListener, device);
    client->onOutputAnnounced(device);
  };
  return l;
}

static_assert(sizeof(kde_output_device_registry_v2_listener) == 2 * sizeof(void (*)()),
              "kde_output_device_registry_v2 has new events, update makeDeviceRegistryListener()");
static const kde_output_device_registry_v2_listener s_deviceRegistryListener = makeDeviceRegistryListener();

///////////////////////////////////////////////////////////////////////////////////////////////////
// kde_output_configuration_v2
///////////////////////////////////////////////////////////////////////////////////////////////////
struct ApplyState
{
  bool done = false;
  bool applied = false;
  std::string reason;
};

static kde_output_configuration_v2_listener makeConfigurationListener()
{
  kde_output_configuration_v2_listener l{};
  l.applied = [](void* data, kde_output_configuration_v2*)
  {
    static_cast<ApplyState*>(data)->applied = true;
    static_cast<ApplyState*>(data)->done = true;
  };
  l.failed = [](void* data, kde_output_configuration_v2*)
  {
    static_cast<ApplyState*>(data)->done = true;
  };
  l.failure_reason = [](void* data, kde_output_configuration_v2*, const char* reason)
  {
    static_cast<ApplyState*>(data)->reason = reason ? reason : "";
  };
  return l;
}

static_assert(sizeof(kde_output_configuration_v2_listener) == 3 * sizeof(void (*)()),
              "kde_output_configuration_v2 has new events, update makeConfigurationListener()");
static const kde_output_configuration_v2_listener s_configurationListener = makeConfigurationListener();

///////////////////////////////////////////////////////////////////////////////////////////////////
// wl_registry
///////////////////////////////////////////////////////////////////////////////////////////////////
static const wl_registry_listener s_registryListener = {
  [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
  {
    static_cast<KWinOutputClient*>(data)->onRegistryGlobal(registry, name, interface, version);
  },
  [](void* data, wl_registry*, uint32_t name)
  {
    static_cast<KWinOutputClient*>(data)->onRegistryGlobalRemove(name);
  }
};

static const wl_callback_listener s_syncListener = {
  [](void* data, wl_callback*, uint32_t)
  {
    *static_cast<bool*>(data) = true;
  }
};

///////////////////////////////////////////////////////////////////////////////////////////////////
// KWinOutputClient
///////////////////////////////////////////////////////////////////////////////////////////////////
KWinOutputClient::KWinOutputClient() = default;

///////////////////////////////////////////////////////////////////////////////////////////////////
KWinOutputClient::~KWinOutputClient()
{
  disconnect();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::log(LogLevel level, const std::string& message) const
{
  if (m_logger)
    m_logger(level, message);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int KWinOutputClient::fd() const
{
  return m_display ? wl_display_get_fd(m_display) : -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::onRegistryGlobal(wl_registry*, uint32_t name, const char* interface, uint32_t version)
{
  if (strcmp(interface, kde_output_device_registry_v2_interface.name) == 0)
  {
    m_deviceRegistryName = name;
    m_deviceRegistryAdvertised = version;
  }
  else if (strcmp(interface, kde_output_management_v2_interface.name) == 0)
  {
    m_managementName = name;
    m_managementAdvertised = version;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::onRegistryGlobalRemove(uint32_t name)
{
  if (name != 0 && (name == m_deviceRegistryName || name == m_managementName))
  {
    log(LogLevel::Warning, "KDE output management global was removed by the compositor");
    m_failed = true;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::onOutputAnnounced(Device* device)
{
  m_devices.emplace_back(device);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::onDeviceDone(Device*)
{
  rebuildOutputs();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool KWinOutputClient::checkConnection()
{
  if (!m_display)
    return false;

  int error = wl_display_get_error(m_display);
  if (error != 0 && !m_failed)
  {
    m_failed = true;

    const wl_interface* interface = nullptr;
    uint32_t id = 0;
    uint32_t code = wl_display_get_protocol_error(m_display, &interface, &id);
    if (error == EPROTO)
      log(LogLevel::Warning, std::string("Wayland protocol error ") + std::to_string(code) + " on " +
                             (interface ? interface->name : "unknown interface"));
    else
      log(LogLevel::Warning, std::string("Wayland connection error: ") + strerror(error));
  }

  return !m_failed;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
KWinOutputClient::WaitResult KWinOutputClient::dispatchUntil(const std::function<bool()>& condition,
                                                             int timeoutMs)
{
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  int displayFd = wl_display_get_fd(m_display);

  while (!condition())
  {
    if (!checkConnection())
      return WaitResult::Error;

    if (wl_display_prepare_read(m_display) != 0)
    {
      // Events are already queued: dispatch them first.
      if (wl_display_dispatch_pending(m_display) < 0)
      {
        checkConnection();
        return WaitResult::Error;
      }
      continue;
    }

    if (wl_display_flush(m_display) < 0 && errno != EAGAIN)
    {
      wl_display_cancel_read(m_display);
      checkConnection();
      return WaitResult::Error;
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0)
    {
      wl_display_cancel_read(m_display);
      return WaitResult::Timeout;
    }

    pollfd pfd = {displayFd, POLLIN, 0};
    int ret = poll(&pfd, 1, static_cast<int>(remaining));
    if (ret <= 0)
    {
      wl_display_cancel_read(m_display);
      if (ret < 0 && errno != EINTR)
      {
        log(LogLevel::Warning, std::string("poll() failed: ") + strerror(errno));
        m_failed = true;
        return WaitResult::Error;
      }
      continue;
    }

    if (wl_display_read_events(m_display) < 0 || wl_display_dispatch_pending(m_display) < 0)
    {
      checkConnection();
      return WaitResult::Error;
    }
  }

  return WaitResult::Done;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
KWinOutputClient::WaitResult KWinOutputClient::roundtrip(int timeoutMs)
{
  bool done = false;
  wl_callback* callback = wl_display_sync(m_display);
  wl_callback_add_listener(callback, &s_syncListener, &done);

  WaitResult result = dispatchUntil([&done]() { return done; }, timeoutMs);

  wl_callback_destroy(callback);
  return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool KWinOutputClient::connect(int timeoutMs)
{
  disconnect();

  m_display = wl_display_connect(nullptr);
  if (!m_display)
  {
    log(LogLevel::Info, "Could not connect to the Wayland compositor");
    return false;
  }

  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &s_registryListener, this);

  if (roundtrip(timeoutMs) != WaitResult::Done)
  {
    log(LogLevel::Warning, "No answer from the Wayland compositor");
    disconnect();
    return false;
  }

  if (m_deviceRegistryName == 0 || m_managementName == 0)
  {
    log(LogLevel::Info, "The compositor does not offer KDE output management");
    disconnect();
    return false;
  }

  if (m_deviceRegistryAdvertised < MIN_DEVICE_REGISTRY_VERSION)
  {
    log(LogLevel::Info, "The compositor's kde_output_device_registry_v2 is too old (version " +
                        std::to_string(m_deviceRegistryAdvertised) + ")");
    disconnect();
    return false;
  }

  m_registryVersion = std::min<uint32_t>(m_deviceRegistryAdvertised,
                                         kde_output_device_registry_v2_interface.version);
  m_managementVersion = std::min<uint32_t>(m_managementAdvertised,
                                           kde_output_management_v2_interface.version);

  m_deviceRegistry = static_cast<kde_output_device_registry_v2*>(
    wl_registry_bind(m_registry, m_deviceRegistryName, &kde_output_device_registry_v2_interface,
                     m_registryVersion));
  kde_output_device_registry_v2_add_listener(m_deviceRegistry, &s_deviceRegistryListener, this);

  m_management = static_cast<kde_output_management_v2*>(
    wl_registry_bind(m_registry, m_managementName, &kde_output_management_v2_interface,
                     m_managementVersion));

  // Binding the registry makes the compositor announce every output with all
  // its properties and modes, followed by done: one roundtrip is enough.
  if (roundtrip(timeoutMs) != WaitResult::Done)
  {
    log(LogLevel::Warning, "No answer from the compositor while reading the outputs");
    disconnect();
    return false;
  }

  collectGarbage();
  rebuildOutputs();

  log(LogLevel::Info, "Connected to KDE output management (device registry v" +
                      std::to_string(m_registryVersion) + " of " + std::to_string(m_deviceRegistryAdvertised) +
                      ", management v" + std::to_string(m_managementVersion) + " of " +
                      std::to_string(m_managementAdvertised) + ", " + std::to_string(m_outputs.size()) +
                      " enabled output(s))");
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void releaseMode(KWinOutputClient::DeviceMode* mode)
{
  if (!mode->proxy)
    return;

  if (kde_output_device_mode_v2_get_version(mode->proxy) >= KDE_OUTPUT_DEVICE_MODE_V2_RELEASE_SINCE_VERSION)
    kde_output_device_mode_v2_release(mode->proxy);
  else
    kde_output_device_mode_v2_destroy(mode->proxy);
  mode->proxy = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void releaseDevice(KWinOutputClient::Device* device)
{
  for (auto& mode : device->modes)
    releaseMode(mode.get());
  device->modes.clear();
  device->current = nullptr;

  if (!device->proxy)
    return;

  if (device->version >= KDE_OUTPUT_DEVICE_V2_RELEASE_SINCE_VERSION)
    kde_output_device_v2_release(device->proxy);
  else
    kde_output_device_v2_destroy(device->proxy);
  device->proxy = nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::collectGarbage()
{
  // Proxies are only released here, never while an apply is awaited or from
  // inside an event handler.
  if (m_applyDepth > 0)
    return;

  for (auto it = m_devices.begin(); it != m_devices.end();)
  {
    Device* device = it->get();
    if (device->removed)
    {
      releaseDevice(device);
      it = m_devices.erase(it);
      continue;
    }

    auto& modes = device->modes;
    for (auto& mode : modes)
    {
      if (mode->removed)
        releaseMode(mode.get());
    }
    modes.erase(std::remove_if(modes.begin(), modes.end(),
                               [](const std::unique_ptr<DeviceMode>& mode) { return mode->removed; }),
                modes.end());
    ++it;
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::disconnect()
{
  if (m_display)
  {
    for (auto& device : m_devices)
      releaseDevice(device.get());

    if (m_deviceRegistry)
    {
      if (!m_failed)
        kde_output_device_registry_v2_stop(m_deviceRegistry);
      kde_output_device_registry_v2_destroy(m_deviceRegistry);
    }
    if (m_management)
      kde_output_management_v2_destroy(m_management);
    if (m_registry)
      wl_registry_destroy(m_registry);

    wl_display_flush(m_display);
    wl_display_disconnect(m_display);
  }

  m_devices.clear();
  m_outputs.clear();
  m_display = nullptr;
  m_registry = nullptr;
  m_deviceRegistry = nullptr;
  m_management = nullptr;
  m_deviceRegistryName = 0;
  m_deviceRegistryAdvertised = 0;
  m_managementName = 0;
  m_managementAdvertised = 0;
  m_registryVersion = 0;
  m_managementVersion = 0;
  m_failed = false;
  m_applyDepth = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool KWinOutputClient::dispatchPending()
{
  if (!m_display || m_failed || m_applyDepth > 0)
    return isConnected();

  while (wl_display_prepare_read(m_display) != 0)
  {
    if (wl_display_dispatch_pending(m_display) < 0)
      return checkConnection();
  }

  if (wl_display_flush(m_display) < 0 && errno != EAGAIN)
  {
    wl_display_cancel_read(m_display);
    return checkConnection();
  }

  pollfd pfd = {wl_display_get_fd(m_display), POLLIN, 0};
  if (poll(&pfd, 1, 0) > 0)
  {
    if (wl_display_read_events(m_display) < 0)
      return checkConnection();
  }
  else
  {
    wl_display_cancel_read(m_display);
  }

  if (wl_display_dispatch_pending(m_display) < 0)
    return checkConnection();

  collectGarbage();
  return checkConnection();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void KWinOutputClient::rebuildOutputs()
{
  m_outputs.clear();

  for (const auto& devicePtr : m_devices)
  {
    const Device* device = devicePtr.get();
    if (device->removed || !device->announced || !device->enabled || device->name.empty())
      continue;

    Output output;
    output.name = device->name;
    output.uuid = device->uuid;
    output.x = device->x;
    output.y = device->y;
    output.scale = device->scale;
    output.transform = device->transform;
    output.priority = device->priority;

    for (const auto& modePtr : device->modes)
    {
      const DeviceMode* mode = modePtr.get();
      if (mode->removed || mode->width <= 0 || mode->height <= 0 || mode->refresh <= 0)
        continue;

      Mode entry;
      entry.width = mode->width;
      entry.height = mode->height;
      entry.refreshMilliHz = mode->refresh;
      entry.interlaced = mode->interlaced;
      entry.preferred = mode->preferred;

      auto existing = std::find_if(output.modes.begin(), output.modes.end(), [&entry](const Mode& other)
      {
        return modeKey(other) == modeKey(entry);
      });
      if (existing != output.modes.end())
        existing->preferred = existing->preferred || entry.preferred;
      else
        output.modes.push_back(entry);
    }

    std::sort(output.modes.begin(), output.modes.end(), [](const Mode& a, const Mode& b)
    {
      return modeKey(a) < modeKey(b);
    });

    if (device->current && !device->current->removed)
    {
      for (size_t i = 0; i < output.modes.size(); i++)
      {
        if (modeKey(output.modes[i]) == modeKey(device->current))
        {
          output.currentMode = static_cast<int>(i);
          break;
        }
      }
    }

    m_outputs.push_back(output);
  }

  std::sort(m_outputs.begin(), m_outputs.end(), [](const Output& a, const Output& b)
  {
    return a.name < b.name;
  });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
const KWinOutputClient::Output* KWinOutputClient::findOutput(const std::string& name) const
{
  for (const auto& output : m_outputs)
  {
    if (output.name == name)
      return &output;
  }
  return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
KWinOutputClient::DeviceMode* KWinOutputClient::findDeviceMode(Device* device, const Mode& mode) const
{
  // Several mode objects can share the same size and rate (different timings).
  // Prefer the current one, then the preferred one, then the first announced.
  DeviceMode* best = nullptr;
  for (const auto& candidatePtr : device->modes)
  {
    DeviceMode* candidate = candidatePtr.get();
    if (candidate->removed || modeKey(candidate) != modeKey(mode))
      continue;

    if (candidate == device->current)
      return candidate;

    if (!best || (candidate->preferred && !best->preferred))
      best = candidate;
  }
  return best;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
KWinOutputClient::ApplyResult KWinOutputClient::setMode(const std::string& outputName, const Mode& mode,
                                                        int timeoutMs, std::string* failureReason)
{
  if (!dispatchPending())
  {
    log(LogLevel::Warning, "Cannot set mode: not connected to KDE output management");
    return ApplyResult::Error;
  }

  Device* device = nullptr;
  for (const auto& candidate : m_devices)
  {
    if (!candidate->removed && candidate->announced && candidate->enabled && candidate->name == outputName)
    {
      device = candidate.get();
      break;
    }
  }

  std::string target = describeMode(mode.width, mode.height, mode.refreshMilliHz) +
                       (mode.interlaced ? " interlaced" : "") + " on " + outputName;

  if (!device)
  {
    log(LogLevel::Warning, "Cannot set mode " + target + ": unknown or disabled output");
    return ApplyResult::Error;
  }

  DeviceMode* deviceMode = findDeviceMode(device, mode);
  if (!deviceMode)
  {
    log(LogLevel::Warning, "Cannot set mode " + target + ": the output has no such mode");
    return ApplyResult::Error;
  }

  if (deviceMode == device->current)
  {
    log(LogLevel::Info, "Mode " + target + " is already current");
    return ApplyResult::Applied;
  }

  log(LogLevel::Info, "Applying mode " + target);

  auto start = std::chrono::steady_clock::now();

  ApplyState state;
  kde_output_configuration_v2* configuration = kde_output_management_v2_create_configuration(m_management);
  kde_output_configuration_v2_add_listener(configuration, &s_configurationListener, &state);
  kde_output_configuration_v2_mode(configuration, device->proxy, deviceMode->proxy);
  kde_output_configuration_v2_apply(configuration);

  m_applyDepth++;
  WaitResult wait = dispatchUntil([&state]() { return state.done; }, timeoutMs);
  m_applyDepth--;

  kde_output_configuration_v2_destroy(configuration);

  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  // The compositor updates current_mode before it answers, so the device state
  // already tells whether the requested mode is in use.
  auto isCurrent = [device, &mode]()
  {
    return !device->removed && device->current && modeKey(device->current) == modeKey(mode);
  };

  ApplyResult result = ApplyResult::Error;
  if (wait == WaitResult::Error)
  {
    log(LogLevel::Warning, "Connection error while applying mode " + target);
    result = ApplyResult::Error;
  }
  else if (wait == WaitResult::Done && state.applied)
  {
    if (isCurrent())
    {
      log(LogLevel::Info, "Mode " + target + " applied in " + std::to_string(elapsed) + " ms");
      result = ApplyResult::Applied;
    }
    else
    {
      std::string current = device->current ? describeMode(device->current->width, device->current->height,
                                                           device->current->refresh) : "unknown";
      log(LogLevel::Warning, "Compositor applied the configuration but the current mode of " + outputName +
                             " is " + current);
      result = ApplyResult::Failed;
    }
  }
  else if (wait == WaitResult::Done)
  {
    log(LogLevel::Warning, "Compositor refused mode " + target +
                           (state.reason.empty() ? std::string() : ": " + state.reason));
    if (failureReason)
      *failureReason = state.reason;
    result = ApplyResult::Failed;
  }
  else
  {
    // Unknown outcome: decide from what the compositor reports now.
    dispatchPending();
    if (isCurrent())
    {
      log(LogLevel::Warning, "No answer in " + std::to_string(timeoutMs) + " ms, but mode " + target +
                             " is now current");
      result = ApplyResult::Applied;
    }
    else
    {
      log(LogLevel::Warning, "No answer in " + std::to_string(timeoutMs) + " ms while applying mode " + target);
      result = ApplyResult::Timeout;
    }
  }

  collectGarbage();
  rebuildOutputs();
  return result;
}
