#include "DisplayManagerKWin.h"

#include <QDebug>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScreen>
#include <QSocketNotifier>
#include <QStringList>
#include <QWindow>

#include "Paths.h"

typedef KWinOutputClient::Mode KWinMode;
typedef KWinOutputClient::Output KWinOutput;

// Upper bound for each blocking exchange with the compositor.
static const int COMPOSITOR_TIMEOUT_MS = 2000;

///////////////////////////////////////////////////////////////////////////////////////////////////
static bool sameMode(const KWinMode& a, const KWinMode& b)
{
  return a.width == b.width && a.height == b.height && a.refreshMilliHz == b.refreshMilliHz &&
         a.interlaced == b.interlaced;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static QString modeName(const KWinMode& mode)
{
  return QString("%1x%2@%3mHz%4").arg(mode.width).arg(mode.height).arg(mode.refreshMilliHz)
                                 .arg(mode.interlaced ? "i" : "");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Restore record: written before switching an output away from its original
// mode, removed only once the original mode is confirmed back. Shared by all
// profiles, since the display is.
///////////////////////////////////////////////////////////////////////////////////////////////////
struct RestoreRecord
{
  QString output;
  KWinMode from;      // the mode to go back to
  KWinMode to;        // the mode last requested
  KWinMode previous;  // the mode in use when it was requested
  bool hasPrevious = false;
};

enum class RecordState { Missing, Valid, Unreadable };

static QString restoreRecordPath()
{
  return Paths::globalDataDir("display-mode-restore.json");
}

static QJsonObject modeToJson(const KWinMode& mode)
{
  QJsonObject obj;
  obj["width"] = mode.width;
  obj["height"] = mode.height;
  obj["refresh_mhz"] = mode.refreshMilliHz;
  obj["interlaced"] = mode.interlaced;
  return obj;
}

static bool modeFromJson(const QJsonValue& value, KWinMode& mode)
{
  QJsonObject obj = value.toObject();
  mode.width = obj["width"].toInt();
  mode.height = obj["height"].toInt();
  mode.refreshMilliHz = obj["refresh_mhz"].toInt();
  mode.interlaced = obj["interlaced"].toBool();
  return mode.width > 0 && mode.height > 0 && mode.refreshMilliHz > 0;
}

static RecordState readRestoreRecord(RestoreRecord& record)
{
  QFile file(restoreRecordPath());
  if (!file.exists())
    return RecordState::Missing;
  if (!file.open(QIODevice::ReadOnly))
    return RecordState::Unreadable;

  QJsonParseError error;
  QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !doc.isObject())
    return RecordState::Unreadable;

  QJsonObject obj = doc.object();
  record.output = obj["output"].toString();
  if (record.output.isEmpty() || !modeFromJson(obj["from"], record.from) || !modeFromJson(obj["to"], record.to))
    return RecordState::Unreadable;
  record.hasPrevious = obj.contains("previous") && modeFromJson(obj["previous"], record.previous);
  return RecordState::Valid;
}

static bool writeRestoreRecord(const RestoreRecord& record)
{
  QJsonObject obj;
  obj["output"] = record.output;
  obj["from"] = modeToJson(record.from);
  obj["to"] = modeToJson(record.to);
  if (record.hasPrevious)
    obj["previous"] = modeToJson(record.previous);

  QSaveFile file(restoreRecordPath());
  if (!file.open(QIODevice::WriteOnly))
    return false;
  file.write(QJsonDocument(obj).toJson());
  return file.commit();
}

static void removeRestoreRecord()
{
  QFile file(restoreRecordPath());
  if (file.exists() && !file.remove())
    qWarning() << "Could not remove" << restoreRecordPath() << ":" << file.errorString();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayManagerKWin::DisplayManagerKWin(QObject* parent)
  : DisplayManager(parent), m_notifier(nullptr), m_recoveryChecked(false)
{
  m_client.setLogger([](KWinOutputClient::LogLevel level, const std::string& message)
  {
    if (level == KWinOutputClient::LogLevel::Warning)
      qWarning() << "KWin output management:" << message.c_str();
    else if (level == KWinOutputClient::LogLevel::Info)
      qInfo() << "KWin output management:" << message.c_str();
    else
      qDebug() << "KWin output management:" << message.c_str();
  });
}

///////////////////////////////////////////////////////////////////////////////////////////////////
DisplayManagerKWin::~DisplayManagerKWin()
{
  // The notifier must go before the connection closes its socket.
  delete m_notifier;
  m_notifier = nullptr;
  m_client.disconnect();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManagerKWin::connectClient()
{
  delete m_notifier;
  m_notifier = nullptr;

  if (!m_client.connect(COMPOSITOR_TIMEOUT_MS))
    return false;

  // Keep the cached state current when outputs or modes change elsewhere.
  m_notifier = new QSocketNotifier(m_client.fd(), QSocketNotifier::Read, this);
  connect(m_notifier, &QSocketNotifier::activated, this, &DisplayManagerKWin::onConnectionActivity);

  for (const KWinOutput& output : m_client.outputs())
  {
    QStringList modes;
    for (const KWinMode& mode : output.modes)
      modes << modeName(mode);
    qInfo() << "KWin output" << output.name.c_str() << "modes:" << qPrintable(modes.join(" "))
            << "current:" << (output.currentMode >= 0 ? modeName(output.modes[output.currentMode]) : "unknown");
  }

  // Only at start-up: after a reconnection the record belongs to this run.
  if (!m_recoveryChecked)
  {
    m_recoveryChecked = true;
    restoreAfterCrash();
  }
  return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void DisplayManagerKWin::onConnectionActivity()
{
  if (!m_client.dispatchPending())
  {
    // Never spin on a dead socket; reconnect at the next initialize().
    qWarning() << "Lost the connection to KWin output management.";
    if (m_notifier)
      m_notifier->setEnabled(false);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void DisplayManagerKWin::restoreAfterCrash()
{
  RestoreRecord record;
  RecordState state = readRestoreRecord(record);
  if (state == RecordState::Missing)
    return;

  if (state == RecordState::Unreadable)
  {
    qWarning() << "Cannot read" << restoreRecordPath()
               << "- refresh rate switching stays off until it is fixed or removed";
    return;
  }

  const KWinOutput* output = m_client.findOutput(record.output.toStdString());
  if (!output || output->currentMode < 0)
  {
    qInfo() << "Keeping the display mode restore record: output" << record.output << "is not available";
    return;
  }

  const KWinMode current = output->modes[output->currentMode];
  if (sameMode(current, record.from))
  {
    removeRestoreRecord();
    return;
  }

  // The last requested mode, or the one before it if that request never applied.
  bool ours = sameMode(current, record.to) || (record.hasPrevious && sameMode(current, record.previous));
  if (!ours)
  {
    qInfo() << "Discarding the display mode restore record for" << record.output
            << "(the mode was changed since)";
    removeRestoreRecord();
    return;
  }

  qInfo() << "The previous run left" << record.output << "at" << modeName(current)
          << "- restoring" << modeName(record.from);
  if (m_client.setMode(record.output.toStdString(), record.from, COMPOSITOR_TIMEOUT_MS) ==
      KWinOutputClient::ApplyResult::Applied)
    removeRestoreRecord();
  else
    qWarning() << "Could not restore" << record.output << "- keeping the record for the next start";
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManagerKWin::initialize()
{
  m_displays.clear();
  m_outputs.clear();

  if (!m_client.isConnected())
  {
    if (!connectClient())
      return false;
  }
  else if (!m_client.dispatchPending())
  {
    if (m_notifier)
      m_notifier->setEnabled(false);
    return false;
  }

  m_outputs = m_client.outputs();

  for (size_t o = 0; o < m_outputs.size(); o++)
  {
    const KWinOutput& output = m_outputs[o];

    DMDisplayPtr display = DMDisplayPtr(new DMDisplay());
    display->m_id = static_cast<int>(o);
    display->m_privId = static_cast<int>(o);
    display->m_name = QString::fromStdString(output.name);
    m_displays[display->m_id] = display;

    for (size_t m = 0; m < output.modes.size(); m++)
    {
      const KWinMode& kmode = output.modes[m];

      DMVideoModePtr mode = DMVideoModePtr(new DMVideoMode());
      mode->m_id = static_cast<int>(m);
      mode->m_privId = static_cast<int>(m);
      mode->m_width = kmode.width;
      mode->m_height = kmode.height;
      mode->m_bitsPerPixel = 0;
      mode->m_refreshRate = kmode.refreshMilliHz / 1000.0f;
      mode->m_interlaced = kmode.interlaced;
      display->m_videoModes[mode->m_id] = mode;
    }
  }

  if (m_displays.empty())
    return false;

  return DisplayManager::initialize();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
const KWinOutput* DisplayManagerKWin::liveOutput(int display)
{
  if (!isValidDisplay(display) || !m_client.isConnected())
    return nullptr;
  return m_client.findOutput(m_displays[display]->m_name.toStdString());
}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool DisplayManagerKWin::setDisplayMode(int display, int mode)
{
  if (!isValidDisplayMode(display, mode))
    return false;

  // Start from the compositor's latest state, not from what was cached.
  m_client.dispatchPending();

  const KWinOutput* output = liveOutput(display);
  if (!output || output->currentMode < 0)
  {
    qWarning() << "KWin output management: the current mode of display" << display << "is unknown, not switching";
    return false;
  }

  const QString name = m_displays[display]->m_name;
  const KWinMode target = m_outputs[m_displays[display]->m_privId].modes[m_displays[display]->m_videoModes[mode]->m_privId];
  const KWinMode current = output->modes[output->currentMode];
  output = nullptr;

  qInfo() << "Switching" << name << "from" << modeName(current) << "to" << modeName(target);

  // Write-ahead record, so that a crash cannot leave the display in our mode.
  // It keeps the original mode across further switches, and also the mode in
  // use now, which stays if this request does not apply.
  RestoreRecord record;
  RecordState state = readRestoreRecord(record);
  if (state == RecordState::Unreadable)
  {
    qWarning() << "Cannot read" << restoreRecordPath() << "- not switching";
    return false;
  }

  bool haveRecord = state == RecordState::Valid && record.output == name;
  if (!haveRecord && !sameMode(target, current))
  {
    if (state == RecordState::Valid)
      qWarning() << "Replacing the display mode restore record for" << record.output;
    record = RestoreRecord();
    record.output = name;
    record.from = current;
    haveRecord = true;
  }

  bool restoring = haveRecord && sameMode(target, record.from);
  if (haveRecord && !restoring)
  {
    record.to = target;
    record.previous = current;
    record.hasPrevious = !sameMode(current, record.from);
    if (!writeRestoreRecord(record))
    {
      qWarning() << "Could not write" << restoreRecordPath() << "- not switching";
      return false;
    }
  }

  KWinOutputClient::ApplyResult result = m_client.setMode(name.toStdString(), target, COMPOSITOR_TIMEOUT_MS);

  // Only a confirmed return to the original mode ends the need to restore.
  if (restoring && result == KWinOutputClient::ApplyResult::Applied)
    removeRestoreRecord();

  return result == KWinOutputClient::ApplyResult::Applied;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKWin::getCurrentDisplayMode(int display)
{
  // Served from the cache that the socket notifier keeps up to date.
  const KWinOutput* output = liveOutput(display);
  if (!output || output->currentMode < 0)
    return -1;

  const KWinMode& current = output->modes[output->currentMode];
  const KWinOutput& known = m_outputs[m_displays[display]->m_privId];
  for (size_t m = 0; m < known.modes.size(); m++)
  {
    if (sameMode(known.modes[m], current))
      return static_cast<int>(m);
  }
  return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKWin::getMainDisplay()
{
  // KWin gives the primary output the lowest priority value.
  int main = -1;
  for (int display : m_displays.keys())
  {
    if (main < 0 ||
        m_outputs[m_displays[display]->m_privId].priority < m_outputs[m_displays[main]->m_privId].priority)
      main = display;
  }
  return main;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKWin::getDisplayFromPoint(int x, int y)
{
  for (int display : m_displays.keys())
  {
    const KWinOutput* output = liveOutput(display);
    if (!output || output->currentMode < 0 || output->scale <= 0)
      continue;

    const KWinMode& mode = output->modes[output->currentMode];
    // Transforms 1, 3, 5 and 7 rotate by 90 or 270 degrees.
    bool rotated = output->transform % 2 == 1;
    // In floating point: a tiny scale or a far position must not overflow.
    double width = (rotated ? mode.height : mode.width) / output->scale;
    double height = (rotated ? mode.width : mode.height) / output->scale;
    double left = output->x;
    double top = output->y;

    if (x >= left && y >= top && x < left + width && y < top + height)
      return display;
  }
  return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
int DisplayManagerKWin::getDisplayFromWindow(QWindow* window)
{
  // Wayland clients cannot know their global position, but Qt names each
  // screen after its output, as KWin does.
  if (!window || !window->screen())
    return -1;

  const QString screenName = window->screen()->name();
  for (int display : m_displays.keys())
  {
    if (m_displays[display]->m_name == screenName)
      return display;
  }

  qInfo() << "No KWin output named" << screenName;
  return -1;
}
