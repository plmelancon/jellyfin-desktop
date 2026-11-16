// Small command line tool to check the KDE output management backend against
// the running compositor, without the application:
//
//   kwin-output-tool list
//   kwin-output-tool set <output> <W>x<H>@<mHz> | <rate in Hz>
//   kwin-output-tool restore <output>
//   kwin-output-tool test <output> <W>x<H>@<mHz> | <rate in Hz> [seconds]
//   kwin-output-tool watch <seconds>
//
// "set" remembers the previous mode in $XDG_RUNTIME_DIR (or /tmp) so that
// "restore" can switch back. "test" switches, waits, and switches back.

#include "KWinOutputClient.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <string>
#include <thread>

typedef KWinOutputClient::Mode Mode;
typedef KWinOutputClient::Output Output;

///////////////////////////////////////////////////////////////////////////////////////////////////
static std::string modeString(const Mode& mode)
{
  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%dx%d@%d (%.3f Hz)%s", mode.width, mode.height, mode.refreshMilliHz,
           mode.refreshMilliHz / 1000.0, mode.interlaced ? " interlaced" : "");
  return buffer;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static std::string currentString(const Output& output)
{
  if (output.currentMode < 0)
    return "unknown";
  return modeString(output.modes[output.currentMode]);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void printOutputs(const KWinOutputClient& client)
{
  printf("device registry v%u, management v%u, %zu enabled output(s)\n", client.registryVersion(),
         client.managementVersion(), client.outputs().size());
  for (const Output& output : client.outputs())
  {
    printf("output %s uuid=%s priority=%u position=%d,%d scale=%.2f transform=%d\n", output.name.c_str(),
           output.uuid.c_str(), output.priority, output.x, output.y, output.scale, output.transform);
    for (size_t i = 0; i < output.modes.size(); i++)
    {
      const Mode& mode = output.modes[i];
      printf("  mode %2zu: %s%s%s\n", i, modeString(mode).c_str(), mode.preferred ? " preferred" : "",
             static_cast<int>(i) == output.currentMode ? " CURRENT" : "");
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Accepts "1920x1080@23976" (exact mode, rate in mHz) or "23.976" (nearest
// rate at the current resolution).
static bool parseMode(const Output& output, const std::string& spec, Mode& mode)
{
  int width, height, refresh;
  if (sscanf(spec.c_str(), "%dx%d@%d", &width, &height, &refresh) == 3)
  {
    for (const Mode& candidate : output.modes)
    {
      if (candidate.width == width && candidate.height == height && candidate.refreshMilliHz == refresh)
      {
        mode = candidate;
        return true;
      }
    }
    return false;
  }

  char* end = nullptr;
  double rate = strtod(spec.c_str(), &end);
  if (!end || *end != '\0' || rate <= 0 || output.currentMode < 0)
    return false;

  const Mode& current = output.modes[output.currentMode];
  const Mode* best = nullptr;
  for (const Mode& candidate : output.modes)
  {
    if (candidate.width != current.width || candidate.height != current.height ||
        candidate.interlaced != current.interlaced)
      continue;
    if (!best || std::fabs(candidate.refreshMilliHz - rate * 1000) < std::fabs(best->refreshMilliHz - rate * 1000))
      best = &candidate;
  }
  if (!best)
    return false;
  mode = *best;
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static std::string stateFile(const std::string& outputName)
{
  const char* dir = getenv("XDG_RUNTIME_DIR");
  return std::string(dir && *dir ? dir : "/tmp") + "/kwin-output-tool-" + outputName + ".state";
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static const char* resultString(KWinOutputClient::ApplyResult result)
{
  switch (result)
  {
    case KWinOutputClient::ApplyResult::Applied: return "applied";
    case KWinOutputClient::ApplyResult::Failed: return "failed";
    case KWinOutputClient::ApplyResult::Timeout: return "timeout";
    case KWinOutputClient::ApplyResult::Error: return "error";
  }
  return "?";
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static bool switchMode(KWinOutputClient& client, const std::string& outputName, const Mode& mode)
{
  auto start = std::chrono::steady_clock::now();
  KWinOutputClient::ApplyResult result = client.setMode(outputName, mode);
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start).count();

  const Output* output = client.findOutput(outputName);
  printf("set %s %s: %s after %lld ms, current mode now %s\n", outputName.c_str(), modeString(mode).c_str(),
         resultString(result), static_cast<long long>(elapsed), output ? currentString(*output).c_str() : "?");
  return result == KWinOutputClient::ApplyResult::Applied;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static int usage()
{
  fprintf(stderr,
          "usage: kwin-output-tool list\n"
          "       kwin-output-tool set <output> <W>x<H>@<mHz> | <rate in Hz>\n"
          "       kwin-output-tool restore <output>\n"
          "       kwin-output-tool test <output> <W>x<H>@<mHz> | <rate in Hz> [seconds]\n"
          "       kwin-output-tool watch <seconds>\n");
  return 2;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char** argv)
{
  if (argc < 2)
    return usage();
  std::string command = argv[1];

  KWinOutputClient client;
  client.setLogger([](KWinOutputClient::LogLevel level, const std::string& message)
  {
    const char* prefix = level == KWinOutputClient::LogLevel::Warning ? "warning" : "info";
    fprintf(stderr, "[%s] %s\n", prefix, message.c_str());
  });

  if (!client.connect())
  {
    fprintf(stderr, "KDE output management is not available\n");
    return 1;
  }

  if (command == "list")
  {
    printOutputs(client);
    return 0;
  }

  if (command == "watch" && argc == 3)
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(atoi(argv[2]));
    std::string last;
    while (std::chrono::steady_clock::now() < deadline)
    {
      std::string state;
      for (const Output& output : client.outputs())
        state += output.name + "=" + currentString(output) + " ";
      if (state != last)
        printf("%s\n", state.c_str());
      fflush(stdout);
      last = state;

      pollfd pfd = {client.fd(), POLLIN, 0};
      poll(&pfd, 1, 200);
      if (!client.dispatchPending())
      {
        fprintf(stderr, "connection lost\n");
        return 1;
      }
    }
    return 0;
  }

  if (argc < 3)
    return usage();
  std::string outputName = argv[2];
  const Output* output = client.findOutput(outputName);
  if (!output)
  {
    fprintf(stderr, "no enabled output named %s\n", outputName.c_str());
    return 1;
  }

  if (command == "restore")
  {
    std::ifstream in(stateFile(outputName));
    std::string spec;
    if (!(in >> spec))
    {
      fprintf(stderr, "nothing to restore for %s\n", outputName.c_str());
      return 1;
    }
    Mode mode;
    if (!parseMode(*output, spec, mode))
    {
      fprintf(stderr, "saved mode %s no longer exists\n", spec.c_str());
      return 1;
    }
    if (!switchMode(client, outputName, mode))
      return 1;
    remove(stateFile(outputName).c_str());
    return 0;
  }

  if ((command == "set" || command == "test") && argc >= 4)
  {
    Mode mode;
    if (!parseMode(*output, argv[3], mode))
    {
      fprintf(stderr, "no mode matching %s on %s\n", argv[3], outputName.c_str());
      return 1;
    }
    if (output->currentMode < 0)
    {
      fprintf(stderr, "current mode of %s is unknown\n", outputName.c_str());
      return 1;
    }
    Mode previous = output->modes[output->currentMode];
    printf("current mode of %s: %s\n", outputName.c_str(), modeString(previous).c_str());

    if (command == "set")
    {
      std::ofstream out(stateFile(outputName));
      out << previous.width << "x" << previous.height << "@" << previous.refreshMilliHz << "\n";
      out.close();
      return switchMode(client, outputName, mode) ? 0 : 1;
    }

    bool ok = switchMode(client, outputName, mode);
    int seconds = argc >= 5 ? atoi(argv[4]) : 5;
    printf("holding for %d s\n", seconds);
    fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    ok = switchMode(client, outputName, previous) && ok;
    return ok ? 0 : 1;
  }

  return usage();
}
