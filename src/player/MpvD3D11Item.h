#ifndef MPVD3D11ITEM_H
#define MPVD3D11ITEM_H

// Windows-only replacement for MpvQt's MpvAbstractItem.
//
// MpvAbstractItem is a QQuickFramebufferObject, so it only works with the
// OpenGL scene graph. That rules out d3d11va zero-copy decoding on Windows
// (Qt 6 has no ANGLE), and with it mpv's d3d11vpp filter, which is what
// drives NVIDIA RTX Video Super Resolution and Intel VSR.
//
// This item runs Qt Quick on Direct3D 11 and uses libmpv's D3D11 render API
// (mpv 0.42 / client API 2.6): mpv decodes, filters and renders on Qt's own
// ID3D11Device, straight into the item's color texture.
//
// The public surface (signals, mpvController()) mirrors MpvAbstractItem so
// MpvVideoItem and PlayerComponent work unchanged.

#include <QtQuick/QQuickRhiItem>
#include <QMutex>
#include <QStringList>
#include <QVariant>
#include <memory>

#include <mpv/client.h>
#include <mpv/render.h>

class MpvController;
class QThread;

// Shared between the item (GUI thread) and its renderer (render thread).
struct MpvD3D11RenderState
{
  QMutex mutex;
  mpv_handle* mpv = nullptr;
  mpv_render_context* ctx = nullptr;
  QQuickItem* item = nullptr; // target of mpv's update callback

  // Must be called with the mutex held.
  void freeContext();
};

class MpvD3D11Item : public QQuickRhiItem
{
  Q_OBJECT

public:
  explicit MpvD3D11Item(QQuickItem* parent = nullptr);
  ~MpvD3D11Item() override;

Q_SIGNALS:
  void ready();
  void observeProperty(const QString& property, mpv_format format, uint64_t id = 0);
  void setProperty(const QString& property, const QVariant& value);
  void command(const QStringList& params);

protected:
  QQuickRhiItemRenderer* createRenderer() override;
  MpvController* mpvController() { return m_controller; }

private:
  QThread* m_workerThread = nullptr;
  MpvController* m_controller = nullptr;
  std::shared_ptr<MpvD3D11RenderState> m_state;
};

#endif // MPVD3D11ITEM_H
