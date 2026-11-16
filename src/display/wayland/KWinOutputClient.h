#ifndef KWINOUTPUTCLIENT_H_
#define KWINOUTPUTCLIENT_H_

// Minimal client for KDE's output device and output management Wayland
// protocols (kde-output-device-v2 / kde-output-management-v2), used to read
// and switch the video mode of an output under KWin.
//
// This class deliberately depends on libwayland-client only (no Qt), so it
// can be exercised by a small command line tool outside of the application.
// It opens its own connection to the compositor and never blocks for longer
// than the given timeouts.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct wl_display;
struct wl_registry;
struct kde_output_device_registry_v2;
struct kde_output_management_v2;

class KWinOutputClient
{
public:
  struct Mode
  {
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    bool interlaced = false;
    bool preferred = false;
  };

  struct Output
  {
    std::string name;
    std::string uuid;
    int x = 0;
    int y = 0;
    double scale = 1.0;
    int transform = 0;
    uint32_t priority = 0;
    // Sorted by (width, height, refresh, interlaced), without duplicates.
    std::vector<Mode> modes;
    // Index into modes, -1 if unknown.
    int currentMode = -1;
  };

  enum class LogLevel { Debug, Info, Warning };
  typedef std::function<void(LogLevel, const std::string&)> Logger;

  enum class ApplyResult
  {
    Applied,      // the compositor reports the requested mode as current
    Failed,       // the compositor refused the configuration
    Timeout,      // no answer in time and the requested mode is not current
    Error         // not connected, unknown output/mode or connection error
  };

  struct Device;
  struct DeviceMode;

  KWinOutputClient();
  ~KWinOutputClient();

  KWinOutputClient(const KWinOutputClient&) = delete;
  KWinOutputClient& operator=(const KWinOutputClient&) = delete;

  void setLogger(Logger logger) { m_logger = std::move(logger); }

  // Connect to the compositor given by WAYLAND_DISPLAY, bind the KDE output
  // globals and read the initial state. Returns false if the compositor does
  // not offer the protocols (e.g. not KWin) or does not answer in time.
  bool connect(int timeoutMs = 2000);
  void disconnect();

  bool isConnected() const { return m_display != nullptr && !m_failed; }
  bool hasFailed() const { return m_failed; }

  // File descriptor of the private connection, for an external event loop.
  int fd() const;

  // Read and dispatch whatever is available without blocking. Returns false
  // if the connection failed.
  bool dispatchPending();

  // Enabled outputs that have been fully announced, sorted by name.
  const std::vector<Output>& outputs() const { return m_outputs; }
  const Output* findOutput(const std::string& name) const;

  // Version of the bound interfaces (0 when not bound).
  uint32_t registryVersion() const { return m_registryVersion; }
  uint32_t managementVersion() const { return m_managementVersion; }

  // Switch the given output to the given mode and wait (bounded) for the
  // compositor's answer. Only the mode of this one output is changed.
  ApplyResult setMode(const std::string& outputName, const Mode& mode,
                      int timeoutMs = 2000, std::string* failureReason = nullptr);

  // Internal, called from the Wayland listeners.
  void onRegistryGlobal(wl_registry* registry, uint32_t name, const char* interface, uint32_t version);
  void onRegistryGlobalRemove(uint32_t name);
  void onOutputAnnounced(Device* device);
  void onDeviceDone(Device* device);

private:
  enum class WaitResult { Done, Timeout, Error };
  WaitResult dispatchUntil(const std::function<bool()>& condition, int timeoutMs);
  WaitResult roundtrip(int timeoutMs);
  bool checkConnection();
  void collectGarbage();
  void rebuildOutputs();
  DeviceMode* findDeviceMode(Device* device, const Mode& mode) const;
  void log(LogLevel level, const std::string& message) const;

  Logger m_logger;

  wl_display* m_display = nullptr;
  wl_registry* m_registry = nullptr;
  kde_output_device_registry_v2* m_deviceRegistry = nullptr;
  kde_output_management_v2* m_management = nullptr;

  uint32_t m_deviceRegistryName = 0;
  uint32_t m_deviceRegistryAdvertised = 0;
  uint32_t m_managementName = 0;
  uint32_t m_managementAdvertised = 0;
  uint32_t m_registryVersion = 0;
  uint32_t m_managementVersion = 0;

  bool m_failed = false;
  int m_applyDepth = 0;

  std::vector<std::unique_ptr<Device>> m_devices;
  std::vector<Output> m_outputs;
};

#endif /* KWINOUTPUTCLIENT_H_ */
