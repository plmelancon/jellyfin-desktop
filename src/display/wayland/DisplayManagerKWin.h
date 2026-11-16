#ifndef DISPLAYMANAGERKWIN_H_
#define DISPLAYMANAGERKWIN_H_

#include <vector>

#include "display/DisplayManager.h"
#include "KWinOutputClient.h"

class QSocketNotifier;

// Display backend for KDE Plasma on Wayland, using KWin's output management
// protocols through a private Wayland connection.
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

  KWinOutputClient m_client;
  QSocketNotifier* m_notifier;

  // State behind m_displays: m_privId of a display/mode indexes these vectors.
  std::vector<KWinOutputClient::Output> m_outputs;
};

#endif /* DISPLAYMANAGERKWIN_H_ */
