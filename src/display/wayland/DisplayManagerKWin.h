#ifndef DISPLAYMANAGERKWIN_H_
#define DISPLAYMANAGERKWIN_H_

#include <vector>

#include "display/DisplayManager.h"
#include "KWinOutputClient.h"

class QSocketNotifier;

// Display backend for KDE Plasma on Wayland, using KWin's output management
// protocols through a private Wayland connection.
//
// KWin keeps the last applied mode across restarts, so every switch away from
// the original mode is recorded in a small file first; if the application
// does not get to switch back (crash, kill, power loss), the next start
// restores the original mode.
class DisplayManagerKWin : public DisplayManager
{
  Q_OBJECT
public:
  explicit DisplayManagerKWin(QObject* parent);
  ~DisplayManagerKWin() override;

  bool initialize() override;
  bool setDisplayMode(int display, int mode) override;
  int getCurrentDisplayMode(int display) override;
  int getMainDisplay() override;
  int getDisplayFromPoint(int x, int y) override;
  int getDisplayFromWindow(QWindow* window) override;

private Q_SLOTS:
  void onConnectionActivity();

private:
  bool connectClient();
  const KWinOutputClient::Output* liveOutput(int display);
  void restoreAfterCrash();

  KWinOutputClient m_client;
  QSocketNotifier* m_notifier;
  bool m_recoveryChecked;

  // State behind m_displays: m_privId of a display/mode indexes these vectors.
  std::vector<KWinOutputClient::Output> m_outputs;
};

#endif /* DISPLAYMANAGERKWIN_H_ */
