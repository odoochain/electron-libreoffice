// Copyright (c) 2022 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

// Portions from PdfWebViewPlugin Copyright 2020 The Chromium Authors. All
// rights reserved. Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file in src/.

#ifndef OFFICE_OFFICE_WEB_PLUGIN_H_
#define OFFICE_OFFICE_WEB_PLUGIN_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "cc/paint/paint_image.h"
#include "include/core/SkImage.h"
#include "office/document_client.h"
#include "office/event_bus.h"
#include "office/lok_tilebuffer.h"
#include "office/office_client.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_text_input_type.h"
#include "third_party/blink/public/web/web_plugin.h"
#include "third_party/blink/public/web/web_plugin_container.h"
#include "third_party/blink/public/web/web_plugin_params.h"
#include "third_party/blink/public/web/web_print_params.h"
#include "third_party/libreofficekit/LibreOfficeKit.hxx"
#include "ui/base/cursor/cursor.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/geometry/vector2d_f.h"
#include "v8/include/v8-template.h"
#include "v8/include/v8-value.h"

namespace content {
class RenderFrame;
}  // namespace content

namespace electron {

namespace office {
// MIME type of the internal office plugin.
extern const char kInternalPluginMimeType[];
// Tries to create an instance of the internal PDF plugin, returning `nullptr`
// if the plugin cannot be created.
blink::WebPlugin* CreateInternalPlugin(blink::WebPluginParams params,
                                       content::RenderFrame* render_frame);
}  // namespace office

class OfficeWebPlugin : public blink::WebPlugin {
 public:
  OfficeWebPlugin(blink::WebPluginParams params,
                  content::RenderFrame* render_frame);

  // disable copy
  OfficeWebPlugin(const OfficeWebPlugin& other) = delete;
  OfficeWebPlugin& operator=(const OfficeWebPlugin& other) = delete;

  // blink::WebPlugin {
  bool Initialize(blink::WebPluginContainer* container) override;
  void Destroy() override;
  blink::WebPluginContainer* Container() const override;
  v8::Local<v8::Object> V8ScriptableObject(v8::Isolate* isolate) override;
  bool SupportsKeyboardFocus() const override;

  void UpdateAllLifecyclePhases(blink::DocumentUpdateReason reason) override;
  void Paint(cc::PaintCanvas* canvas, const gfx::Rect& rect) override;

  void UpdateGeometry(const gfx::Rect& window_rect,
                      const gfx::Rect& clip_rect,
                      const gfx::Rect& unobscured_rect,
                      bool is_visible) override;
  void UpdateFocus(bool focused, blink::mojom::FocusType focus_type) override;
  void UpdateVisibility(bool visibility) override;
  blink::WebInputEventResult HandleInputEvent(
      const blink::WebCoalescedInputEvent& event,
      ui::Cursor* cursor) override;

  // no-op
  void DidReceiveResponse(const blink::WebURLResponse& response) override;
  void DidReceiveData(const char* data, size_t data_length) override;
  void DidFinishLoading() override;
  void DidFailLoading(const blink::WebURLError& error) override;

  // TODO: See if we need to support printing through Chromium?
  // bool SupportsPaginatedPrint() override;
  // bool GetPrintPresetOptionsFromDocument(
  //     blink::WebPrintPresetOptions* print_preset_options) override;
  // int PrintBegin(const blink::WebPrintParams& print_params) override;
  // void PrintPage(int page_number, cc::PaintCanvas* canvas) override;
  // void PrintEnd() override;

  // TODO: Support copy/paste
  // bool HasSelection() const override;
  // blink::WebString SelectionAsText() const override;
  // blink::WebString SelectionAsMarkup() const override;
  bool CanEditText() const override;
  bool HasEditableText() const override;

  bool CanUndo() const override;
  bool CanRedo() const override;

  // TODO: Update after Electron's Chromium is bumped, this is introduced in
  // newer versions
  // bool CanCopy() const override;

  // TODO: Check if we need this
  // bool ExecuteEditCommand(const blink::WebString& name,
  //                         const blink::WebString& value) override;

  // TODO: This could probably be used for defs, but it might make more sense to
  // just use an event handler
  // blink::WebURL LinkAtPosition(const gfx::Point& /*position*/) const
  // override;

  // TODO: Check if we need this
  // bool StartFind(const blink::WebString& search_text,
  //                bool case_sensitive,
  //                int identifier) override;
  // void SelectFindResult(bool forward, int identifier) override;
  // void StopFind() override;
  // bool CanRotateView() override;
  // void RotateView(blink::WebPlugin::RotationType type) override;

  // TODO: Support IME events
  // bool ShouldDispatchImeEventsToPlugin() override;
  // blink::WebTextInputType GetPluginTextInputType() override;
  // gfx::Rect GetPluginCaretBounds() override;
  // void ImeSetCompositionForPlugin(
  //     const blink::WebString& text,
  //     const std::vector<ui::ImeTextSpan>& ime_text_spans,
  //     const gfx::Range& replacement_range,
  //     int selection_start,
  //     int selection_end) override;
  // void ImeCommitTextForPlugin(
  //     const blink::WebString& text,
  //     const std::vector<ui::ImeTextSpan>& ime_text_spans,
  //     const gfx::Range& replacement_range,
  //     int relative_cursor_pos) override;
  // void ImeFinishComposingTextForPlugin(bool keep_selection) override;

  // } blink::WebPlugin

  void InvalidatePluginContainer();

  content::RenderFrame* render_frame() const;

  void TriggerFullRerender();
  base::WeakPtr<OfficeWebPlugin> GetWeakPtr();

 private:
  // call `Destroy()` instead.
  ~OfficeWebPlugin() override;
  blink::WebInputEventResult HandleKeyEvent(const blink::WebKeyboardEvent event,
                                            ui::Cursor* cursor);
  blink::WebInputEventResult HandleCutCopyEvent(std::string event);
  blink::WebInputEventResult HandlePasteEvent();
  bool HandleMouseEvent(blink::WebInputEvent::Type type,
                        gfx::PointF position,
                        int modifiers,
                        int clickCount,
                        ui::Cursor* cursor);

  // Updates the available area
  void OnGeometryChanged(double old_zoom, float old_device_scale);

  // Computes document width/height in device pixels, based on the total scale
  gfx::Size GetDocumentPixelSize();

  void OnViewportChanged(const gfx::Rect& plugin_rect_in_css_pixel,
                         float new_device_scale);

  void UpdateScroll(int y_position);

  float TwipToPx(float in);
  float TotalScale();

  // Exposed methods {
  gfx::Size GetDocumentCSSPixelSize();
  std::vector<gfx::Rect> PageRects() const;
  void SetZoom(float zoom);
  float GetZoom();
  float TwipToCSSPx(float in);

  void UpdateScrollInTask(int y_position);

  // prepares the embed as the document client's mounted viewer
  bool RenderDocument(v8::Isolate* isolate,
                      gin::Handle<office::DocumentClient> client);
  // }

  void ResetTileBuffers();

  // LOK event handlers {
  void HandleInvalidateTiles(std::string payload);
  void HandleDocumentSizeChanged(std::string payload);
  // }

  // owns this class
  blink::WebPluginContainer* container_;

  // Painting State {
  // plugin rect in CSS pixels
  gfx::Rect css_plugin_rect_;
  // plugin rectangle in device pixels.
  gfx::Rect plugin_rect_;
  // Remaining area, in pixels, to render the view in after accounting for
  // horizontal centering.
  gfx::Rect available_area_;
  gfx::Rect available_area_twips_;
  // browser viewport zoom factor
  double viewport_zoom_ = 1.0;
  // current device scale factor. viewport * device_scale_ == screen, screen /
  // device_scale_ == viewport
  float device_scale_ = 1.0f;
  float zoom_ = 1.0f;
  float old_zoom_ = 1.0f;
  // first paint, requiring the full canvas of tiles to be painted
  bool first_paint_ = true;
  // first paint, requiring the full canvas of tiles to be painted
  bool reset_canvas_ = false;
  // currently painting, to track deferred invalidates
  bool in_paint_ = false;
  // the offset for input events, adjusted by the scroll position
  int scroll_y_position_ = 0;

  // If this is true, then don't scroll the plugin in response to calls to
  // `UpdateScroll()`. This will be true when the extension page is in the
  // process of zooming the plugin so that flickering doesn't occur while
  // zooming.
  bool stop_scrolling_ = false;
  // }

  // UI State {
  // current cursor
  ui::mojom::CursorType cursor_type_ = ui::mojom::CursorType::kPointer;
  bool has_focus_;
  // }

  // owned by
  content::RenderFrame* render_frame_ = nullptr;

  // maybe has a
  lok::Document* document_ = nullptr;
  office::DocumentClient* document_client_ = nullptr;
  int view_id_ = -1;

  std::vector<office::TileBuffer> part_tile_buffer_;

  scoped_refptr<base::SequencedTaskRunner> task_runner_;

  v8::Global<v8::ObjectTemplate> v8_template_;
  v8::Global<v8::Object> v8_object_;

  // invalidates when destroy() is called
  base::WeakPtrFactory<OfficeWebPlugin> weak_factory_{this};
};

}  // namespace electron
#endif  // OFFICE_OFFICE_WEB_PLUGIN_H_
