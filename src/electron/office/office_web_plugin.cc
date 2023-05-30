// Copyright (c) 2022 Macro.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "office/office_web_plugin.h"

#include <chrono>
#include <memory>

#include "LibreOfficeKit/LibreOfficeKit.hxx"
#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/notreached.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread_task_runner_handle.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_image.h"
#include "cc/paint/paint_image_builder.h"
#include "components/plugins/renderer/plugin_placeholder.h"
#include "content/public/renderer/render_frame.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/handle.h"
#include "gin/object_template_builder.h"
#include "include/core/SkColor.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkTextBlob.h"
#include "office/cancellation_flag.h"
#include "office/document_client.h"
#include "office/event_bus.h"
#include "office/lok_callback.h"
#include "office/lok_tilebuffer.h"
#include "office/office_client.h"
#include "office/office_keys.h"
#include "office/paint_manager.h"
#include "shell/common/gin_converters/gfx_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/function_template_extensions.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_keyboard_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_pointer_properties.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_input_event_result.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_plugin_params.h"
#include "third_party/blink/public/web/web_widget.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/cursor/platform_cursor.h"
#include "ui/events/blink/blink_event_util.h"
#include "ui/gfx/geometry/point3_f.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/geometry/size_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/native_widget_types.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-object.h"

namespace electron {

namespace office {
extern const char kInternalPluginMimeType[] = "application/x-libreoffice";

blink::WebPlugin* CreateInternalPlugin(blink::WebPluginParams params,
                                       content::RenderFrame* render_frame) {
  return new OfficeWebPlugin(params, render_frame);
}

}  // namespace office

OfficeWebPlugin::OfficeWebPlugin(blink::WebPluginParams params,
                                 content::RenderFrame* render_frame)
    : render_frame_(render_frame),
      task_runner_(render_frame->GetTaskRunner(
          blink::TaskType::kInternalMediaRealTime)) {
  tile_buffer_ = std::make_unique<office::TileBuffer>();
  paint_manager_ = std::make_unique<office::PaintManager>(this);
};

// blink::WebPlugin {
bool OfficeWebPlugin::Initialize(blink::WebPluginContainer* container) {
  container_ = container;
  // This prevents the wheel event hit test data from causing a crash, wheel
  // events are handled by the scroll container anyway
  container->SetWantsWheelEvents(false);

  // TODO: figure out what false means?
  return true;
}

void OfficeWebPlugin::Destroy() {
  delete this;
}

v8::Local<v8::Object> OfficeWebPlugin::V8ScriptableObject(
    v8::Isolate* isolate) {
  if (v8_template_.IsEmpty()) {
    v8::Local<v8::ObjectTemplate> template_ =
        gin::ObjectTemplateBuilder(isolate, "OfficeWebPlugin")
            .SetMethod("renderDocument",
                       base::BindRepeating(&OfficeWebPlugin::RenderDocument,
                                           base::Unretained(this)))
            .SetMethod("updateScroll",
                       base::BindRepeating(&OfficeWebPlugin::UpdateScroll,
                                           base::Unretained(this)))
            .SetMethod("getZoom", base::BindRepeating(&OfficeWebPlugin::GetZoom,
                                                      base::Unretained(this)))
            .SetMethod("setZoom", base::BindRepeating(&OfficeWebPlugin::SetZoom,
                                                      base::Unretained(this)))
            .SetMethod("twipToPx",
                       base::BindRepeating(&OfficeWebPlugin::TwipToCSSPx,
                                           base::Unretained(this)))
            .SetProperty(
                "documentSize",
                base::BindRepeating(&OfficeWebPlugin::GetDocumentCSSPixelSize,
                                    base::Unretained(this)))
            .SetProperty("pageRects",
                         base::BindRepeating(&OfficeWebPlugin::PageRects,
                                             base::Unretained(this)))
            .Build();
    v8_template_.Reset(isolate, template_);
  }

  if (v8_object_.IsEmpty()) {
    v8_object_.Reset(isolate, v8_template_.Get(isolate)
                                  ->NewInstance(isolate->GetCurrentContext())
                                  .ToLocalChecked());
  }
  return v8_object_.Get(isolate);
}

blink::WebPluginContainer* OfficeWebPlugin::Container() const {
  return container_;
}

bool OfficeWebPlugin::SupportsKeyboardFocus() const {
  return true;
}

void OfficeWebPlugin::UpdateAllLifecyclePhases(
    blink::DocumentUpdateReason reason) {}

void OfficeWebPlugin::Paint(cc::PaintCanvas* canvas, const gfx::Rect& rect) {
  base::AutoReset<bool> auto_reset_in_paint(&in_paint_, true);

  SkRect invalidate_rect =
      gfx::RectToSkRect(gfx::IntersectRects(css_plugin_rect_, rect));
  cc::PaintCanvasAutoRestore auto_restore(canvas, true);

  if (scale_pending_ || first_paint_)
    canvas->drawColor(SK_ColorTRANSPARENT, SkBlendMode::kSrc);

  canvas->clipRect(invalidate_rect);

  // not mounted
  if (!document_client_)
    return;

  if (!plugin_rect_.origin().IsOrigin())
    canvas->translate(plugin_rect_.x(), plugin_rect_.y());

  if (scale_pending_) {
    canvas->scale(zoom_ / old_zoom_);
  }

  document_->setView(view_id_);

  std::vector<office::TileRange> missing = tile_buffer_->PaintToCanvas(
      paint_cancel_flag_, canvas, gfx::Rect(rect.size()));

  // the temporary scale is painted, now
  if (scale_pending_) {
    scale_pending_ = false;
    tile_buffer_->ResetScale(TotalScale());
    ScheduleAvailableAreaPaint();
    first_paint_ = false;
  } else {
    paint_manager_->ScheduleNextPaint(missing);
    first_paint_ = false;
  }
}

void OfficeWebPlugin::UpdateGeometry(const gfx::Rect& window_rect,
                                     const gfx::Rect& clip_rect,
                                     const gfx::Rect& unobscured_rect,
                                     bool is_visible) {
  // nothing to render inside of
  if (window_rect.IsEmpty())
    return;

  // get the document root frame's scale factor so that any widget scaling does
  // not affect the device scale
  blink::WebWidget* widget =
      container_->GetDocument().GetFrame()->LocalRoot()->FrameWidget();
  OnViewportChanged(window_rect,
                    widget->GetOriginalScreenInfo().device_scale_factor);
}

void OfficeWebPlugin::UpdateFocus(bool focused,
                                  blink::mojom::FocusType focus_type) {
  if (has_focus_ != focused) {
    // TODO: update focus, input state, and selection bounds
  }

  has_focus_ = focused;
}

void OfficeWebPlugin::UpdateVisibility(bool visibility) {}

blink::WebInputEventResult OfficeWebPlugin::HandleInputEvent(
    const blink::WebCoalescedInputEvent& event,
    ui::Cursor* cursor) {
  blink::WebInputEvent::Type event_type = event.Event().GetType();

  // TODO: handle gestures
  if (blink::WebInputEvent::IsGestureEventType(event_type))
    return blink::WebInputEventResult::kNotHandled;

  if (container_ && container_->WasTargetForLastMouseEvent()) {
    *cursor = cursor_type_;
  }

  if (blink::WebInputEvent::IsKeyboardEventType(event_type))
    return HandleKeyEvent(
        std::move(static_cast<const blink::WebKeyboardEvent&>(event.Event())),
        cursor);

  std::unique_ptr<blink::WebInputEvent> transformed_event =
      ui::TranslateAndScaleWebInputEvent(
          event.Event(), gfx::Vector2dF(-available_area_.x(), 0), 1.0);

  const blink::WebInputEvent& event_to_handle =
      transformed_event ? *transformed_event : event.Event();

  switch (event_type) {
    case blink::WebInputEvent::Type::kMouseDown:
    case blink::WebInputEvent::Type::kMouseUp:
    case blink::WebInputEvent::Type::kMouseMove:
      break;
    default:
      return blink::WebInputEventResult::kNotHandled;
  }

  blink::WebMouseEvent mouse_event =
      static_cast<const blink::WebMouseEvent&>(event_to_handle);

  int modifiers = mouse_event.GetModifiers();
  return HandleMouseEvent(event_type, mouse_event.PositionInWidget(), modifiers,
                          mouse_event.ClickCount(), cursor)
             ? blink::WebInputEventResult::kHandledApplication
             : blink::WebInputEventResult::kNotHandled;
}

blink::WebInputEventResult OfficeWebPlugin::HandleKeyEvent(
    const blink::WebKeyboardEvent event,
    ui::Cursor* cursor) {
  if (!document_ || view_id_ == -1)
    return blink::WebInputEventResult::kNotHandled;

  blink::WebInputEvent::Type type = event.GetType();

  // supress scroll event for any containers when pressing space
  if (type == blink::WebInputEvent::Type::kChar &&
      event.dom_code == office::DomCode::SPACE) {
    return blink::WebInputEventResult::kHandledApplication;
  }

  // only handle provided key events
  switch (type) {
    case blink::WebInputEvent::Type::kRawKeyDown:
    case blink::WebInputEvent::Type::kKeyUp:
      break;
    default:
      return blink::WebInputEventResult::kNotHandled;
  }

#if BUILDFLAG(IS_MAC)
  office::Modifiers base_modifier = office::Modifiers::kMetaKey;
#else
  office::Modifiers base_modifier = office::Modifiers::kControlKey;
#endif

  // intercept some special key events
  if (event.GetModifiers() & base_modifier) {
    switch (event.dom_code) {
      // don't close the internal LO window
      case office::DomCode::US_W:
        return blink::WebInputEventResult::kNotHandled;
      case office::DomCode::US_C:
        LOG(ERROR) << "HANDLING COPY";
        return HandleCutCopyEvent(".uno:Copy");
      case office::DomCode::US_V:
        LOG(ERROR) << "HANDLING PASTE";
        return HandlePasteEvent();
      case office::DomCode::US_X:
        LOG(ERROR) << "HANDLING CUT";
        return HandleCutCopyEvent(".uno:Cut");
    }
  }

  int modifiers = event.GetModifiers();

#if BUILDFLAG(IS_MAC)
  modifiers &= ~office::Modifiers::kControlKey;
  if (modifiers & office::Modifiers::kMetaKey) {
    modifiers |= office::Modifiers::kControlKey;
    modifiers &= ~office::Modifiers::kMetaKey;
  }
#endif

  int lok_key_code = office::DOMKeyCodeToLOKKeyCode(event.dom_code, modifiers);

  task_runner_->PostTask(FROM_HERE,
                         base::BindOnce(&lok::Document::setView,
                                        base::Unretained(document_), view_id_));
  task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&lok::Document::postKeyEvent, base::Unretained(document_),
                     type == blink::WebInputEvent::Type::kKeyUp
                         ? LOK_KEYEVENT_KEYUP
                         : LOK_KEYEVENT_KEYINPUT,
                     event.text[0], lok_key_code));

  return blink::WebInputEventResult::kHandledApplication;
}

blink::WebInputEventResult OfficeWebPlugin::HandleCutCopyEvent(
    std::string event) {
  LOG(ERROR) << "HandleCutCopyEvent" << event;
  document_client_->PostUnoCommandInternal(event, nullptr, true);
  return blink::WebInputEventResult::kHandledApplication;
}

blink::WebInputEventResult OfficeWebPlugin::HandlePasteEvent() {
  ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
  std::vector<std::u16string> res =
      clipboard->ReadAvailableStandardAndCustomFormatNames(
          ui::ClipboardBuffer::kCopyPaste, nullptr);

  std::string clipboard_type = "";

  for (std::u16string r : res) {
    if (r == u"text/plain") {
      clipboard_type = "text/plain;charset=utf-8";
      break;
    } else if (r == u"image/png") {
      clipboard_type = "image/png";
      break;
    }
  }

  if (!document_client_->OnPasteEvent(clipboard, clipboard_type)) {
    LOG(ERROR) << "Failed to set lok clipboard";
    return blink::WebInputEventResult::kNotHandled;
  }

  return blink::WebInputEventResult::kHandledApplication;
}

bool OfficeWebPlugin::HandleMouseEvent(blink::WebInputEvent::Type type,
                                       gfx::PointF position,
                                       int modifiers,
                                       int clickCount,
                                       ui::Cursor* cursor) {
  if (!document_ || view_id_ == -1)
    return false;

  LibreOfficeKitMouseEventType event_type;
  switch (type) {
    case blink::WebInputEvent::Type::kMouseDown:
      event_type = LOK_MOUSEEVENT_MOUSEBUTTONDOWN;
      break;
    case blink::WebInputEvent::Type::kMouseUp:
      event_type = LOK_MOUSEEVENT_MOUSEBUTTONUP;
      break;
    case blink::WebInputEvent::Type::kMouseMove:
      event_type = LOK_MOUSEEVENT_MOUSEMOVE;
      break;
    default:
      return false;
  }

  // allow focus even if not in area
  if (!available_area_.Contains(gfx::ToCeiledPoint(position))) {
    return event_type == LOK_MOUSEEVENT_MOUSEBUTTONDOWN;
  }

  // offset by the scroll position
  position.Offset(0, scroll_y_position_);

  gfx::Point pos = gfx::ToRoundedPoint(gfx::ScalePoint(
      position, office::lok_callback::kTwipPerPx / TotalScale()));

  int buttons = 0;
  if (modifiers & blink::WebInputEvent::kLeftButtonDown)
    buttons |= 1;
  if (modifiers & blink::WebInputEvent::kMiddleButtonDown)
    buttons |= 2;
  if (modifiers & blink::WebInputEvent::kRightButtonDown)
    buttons |= 4;

  if (buttons > 0) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&lok::Document::setView,
                                  base::Unretained(document_), view_id_));
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&lok::Document::postMouseEvent,
                       base::Unretained(document_), event_type, pos.x(),
                       pos.y(), clickCount, buttons,
                       office::EventModifiersToLOKModifiers(modifiers)));
    return true;
  }

  return false;
}

void OfficeWebPlugin::DidReceiveResponse(
    const blink::WebURLResponse& response) {}

// no-op
void OfficeWebPlugin::DidReceiveData(const char* data, size_t data_length) {}
void OfficeWebPlugin::DidFinishLoading() {}
void OfficeWebPlugin::DidFailLoading(const blink::WebURLError& error) {}

bool OfficeWebPlugin::CanEditText() const {
  return true;
}

bool OfficeWebPlugin::HasEditableText() const {
  return true;
}

bool OfficeWebPlugin::CanUndo() const {
  return document_client_ && document_client_->CanUndo();
}

bool OfficeWebPlugin::CanRedo() const {
  return document_client_ && document_client_->CanRedo();
}

content::RenderFrame* OfficeWebPlugin::render_frame() const {
  return render_frame_;
}

OfficeWebPlugin::~OfficeWebPlugin() = default;

void OfficeWebPlugin::InvalidateWeakContainer() {
  container_->Invalidate();
}

void OfficeWebPlugin::InvalidatePluginContainer() {
  if (container_) {
    task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&OfficeWebPlugin::InvalidateWeakContainer,
                                  GetWeakPtr()));
  }
}

void OfficeWebPlugin::OnGeometryChanged(double old_zoom,
                                        float old_device_scale) {
  if (!document_client_)
    return;

  if (viewport_zoom_ != old_zoom || device_scale_ != old_device_scale) {
    tile_buffer_->ResetScale(TotalScale());
  }

  available_area_ = gfx::Rect(plugin_rect_.size());
  gfx::Size doc_size = GetDocumentPixelSize();
  if (doc_size.width() < available_area_.width()) {
    available_area_.set_width(doc_size.width());
  }

  // The distance between top of the plugin and the bottom of the document in
  // pixels.
  int bottom_of_document = doc_size.height();
  if (bottom_of_document < plugin_rect_.height())
    available_area_.set_height(bottom_of_document);

  available_area_twips_ = gfx::ScaleToEnclosingRect(
      available_area_, office::lok_callback::kTwipPerPx);
}

std::vector<gfx::Rect> OfficeWebPlugin::PageRects() const {
  std::vector<gfx::Rect> result;

  if (!document_client_)
    return result;
  auto page_rects_ = document_client_->PageRects();

  float scale = zoom_ / office::lok_callback::kTwipPerPx;
  for (auto& rect : page_rects_) {
    result.emplace_back(gfx::ScaleToCeiledPoint(rect.origin(), scale),
                        gfx::ScaleToCeiledSize(rect.size(), scale));
  }
  return result;
}

gfx::Size OfficeWebPlugin::GetDocumentPixelSize() {
  auto size = document_client_->DocumentSizeTwips();
  return gfx::Size(ceil(TwipToPx(size.width())), ceil(TwipToPx(size.height())));
}

gfx::Size OfficeWebPlugin::GetDocumentCSSPixelSize() {
  auto size = document_client_->DocumentSizeTwips();
  return gfx::Size(
      ceil(office::lok_callback::TwipToPixel(size.width(), zoom_)),
      ceil(office::lok_callback::TwipToPixel(size.height(), zoom_)));
}

void OfficeWebPlugin::OnViewportChanged(
    const gfx::Rect& plugin_rect_in_css_pixel,
    float new_device_scale) {
  DCHECK_GT(new_device_scale, 0.0f);

  css_plugin_rect_ = plugin_rect_in_css_pixel;

  if (new_device_scale == device_scale_ &&
      plugin_rect_in_css_pixel == plugin_rect_) {
    return;
  }

  bool need_fresh_paint =
      plugin_rect_in_css_pixel.height() != plugin_rect_.height();

  const float old_device_scale = device_scale_;
  device_scale_ = new_device_scale;
  plugin_rect_ = plugin_rect_in_css_pixel;

  OnGeometryChanged(viewport_zoom_, old_device_scale);

  if (!document_client_)
    return;

  if (need_fresh_paint)
    ScheduleAvailableAreaPaint();
}

void OfficeWebPlugin::HandleInvalidateTiles(std::string payload) {
  // not mounted
  if (view_id_ == -1)
    return;

  std::string_view payload_sv(payload);

  // TODO: handle non-text document types for parts
  if (payload_sv == "EMPTY") {
    ScheduleAvailableAreaPaint();
  } else {
    std::string_view::const_iterator start = payload_sv.begin();
    gfx::Rect dirty_rect =
        office::lok_callback::ParseRect(start, payload_sv.end());

    if (dirty_rect.IsEmpty())
      return;

    float view_height = plugin_rect_.height() * device_scale_;
    auto range = tile_buffer_->InvalidateTilesInTwipRect(dirty_rect);
    auto limit = tile_buffer_->LimitIndex(scroll_y_position_, view_height);

    // avoid scheduling out of bounds paints
    if (range.index_start > limit.index_end ||
        range.index_end < limit.index_start)
      return;
    range.index_start = std::max(range.index_start, limit.index_start);
    range.index_end = std::min(range.index_end, limit.index_end);

    paint_manager_->SchedulePaint(document_, scroll_y_position_, view_height,
                                  TotalScale(), false, {range});
  }
}

void OfficeWebPlugin::HandleDocumentSizeChanged(std::string payload) {
  if (!document_)
    return;
  long width, height;
  document_->getDocumentSize(&width, &height);
  tile_buffer_->Resize(width, height);
}

float OfficeWebPlugin::TotalScale() {
  return zoom_ * device_scale_ * viewport_zoom_;
}

void OfficeWebPlugin::SetZoom(float zoom) {
  if (abs(zoom_ - zoom) < 0.0001f) {
    return;
  }

  old_zoom_ = zoom_;
  scroll_y_position_ = zoom / zoom_ * scroll_y_position_;
  zoom_ = zoom;

  if (!document_client_ || view_id_ == -1)
    return;
  scale_pending_ = true;

  // immediately flush the container to scale without invalidating tiles
  InvalidatePluginContainer();
}

float OfficeWebPlugin::GetZoom() {
  return zoom_;
}

float OfficeWebPlugin::TwipToPx(float in) {
  return office::lok_callback::TwipToPixel(in, TotalScale());
}

float OfficeWebPlugin::TwipToCSSPx(float in) {
  return ceil(office::lok_callback::TwipToPixel(in, zoom_));
}

void OfficeWebPlugin::UpdateScroll(int y_position) {
  if (!document_client_ || stop_scrolling_)
    return;

  float view_height =
      plugin_rect_.height() / device_scale_ / (float)viewport_zoom_;
  float max_y = std::max(
      TwipToPx(document_client_->DocumentSizeTwips().height()) - view_height,
      0.0f);

  float scaled_y = base::clamp((float)y_position, 0.0f, max_y) * device_scale_;
  scroll_y_position_ = scaled_y;

  office::CancelFlag::CancelAndReset(paint_cancel_flag_);

  // TODO: paint PRIOR not ahead for scroll up
  office::TileRange range = tile_buffer_->NextScrollTileRange(
      scroll_y_position_, view_height * device_scale_);
  tile_buffer_->SetYPosition(scaled_y);
  // TODO: schedule paint _ahead_ / _prior_ to scroll position
  paint_manager_->SchedulePaint(document_, scroll_y_position_,
                                view_height * device_scale_, TotalScale(),
                                false, {range});
}

bool OfficeWebPlugin::RenderDocument(
    v8::Isolate* isolate,
    gin::Handle<office::DocumentClient> client) {
  if (client.IsEmpty()) {
    LOG(ERROR) << "invalid document client";
    return false;
  }
  base::WeakPtr<office::OfficeClient> office = client->GetOfficeClient();
  if (!office) {
    LOG(ERROR) << "invalid office client";
    return false;
  }

  // TODO: honestly, this is terrible, need to do this properly
  // already mounted
  bool needs_reset = view_id_ != -1 && document_ != client->GetDocument() &&
                     document_ != nullptr;
  if (needs_reset) {
    office->CloseDocument(document_client_->Path());
    document_client_->Unmount();
    document_ = nullptr;
    document_client_ = nullptr;
  }

  document_ = client->GetDocument();
  if (needs_reset) {
    tile_buffer_->InvalidateAllTiles();
  }

  document_client_ = client.get();
  rendered_client_.Reset(
      isolate, document_client_->GetWrapper(isolate).ToLocalChecked());
  view_id_ = client->Mount(isolate);
  auto size = document_client_->DocumentSizeTwips();
  scroll_y_position_ = 0;
  tile_buffer_->SetYPosition(0);
  tile_buffer_->Resize(size.width(), size.height(), TotalScale());

  if (needs_reset) {
    // this is an awful hack
    auto device_scale = device_scale_;
    device_scale_ = 0;
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&OfficeWebPlugin::OnViewportChanged,
                       base::Unretained(this), css_plugin_rect_, device_scale));
  }

  document_->setView(view_id_);
  document_->resetSelection();

  base::WeakPtr<office::EventBus> event_bus(client->GetEventBus());

  if (event_bus) {
    event_bus->Handle(
        LOK_CALLBACK_DOCUMENT_SIZE_CHANGED,
        base::BindRepeating(&OfficeWebPlugin::HandleDocumentSizeChanged,
                            base::Unretained(this)));
    event_bus->Handle(
        LOK_CALLBACK_INVALIDATE_TILES,
        base::BindRepeating(&OfficeWebPlugin::HandleInvalidateTiles,
                            base::Unretained(this)));
  }
  if (needs_reset) {
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&OfficeWebPlugin::OnGeometryChanged,
                       base::Unretained(this), viewport_zoom_, device_scale_));
  }
  return true;
}

void OfficeWebPlugin::ScheduleAvailableAreaPaint() {
  gfx::RectF offset_area(available_area_);
  offset_area.Offset(0, scroll_y_position_);
  offset_area.Scale(device_scale_);
  auto range = tile_buffer_->InvalidateTilesInRect(offset_area);
  auto limit =
      tile_buffer_->LimitIndex(scroll_y_position_, offset_area.height());

  // avoid scheduling out of bounds paints
  if (range.index_start > limit.index_end ||
      range.index_end < limit.index_start)
    return;
  range.index_start = std::max(range.index_start, limit.index_start);
  range.index_end = std::min(range.index_end, limit.index_end);
  paint_manager_->SchedulePaint(document_, scroll_y_position_,
                                offset_area.height(), TotalScale(), true,
                                {range});
}

void OfficeWebPlugin::TriggerFullRerender() {
  first_paint_ = true;
  if (document_client_ && !document_client_->DocumentSizeTwips().IsEmpty()) {
    tile_buffer_->InvalidateAllTiles();
    ScheduleAvailableAreaPaint();
  }
}

office::TileBuffer* OfficeWebPlugin::GetTileBuffer() {
  // TODO: maybe use a scoped ref pointer or shared pointer, but if the cancel
  // flag is invalid, shouldn't matter
  return tile_buffer_.get();
}

base::WeakPtr<office::PaintManager::Client> OfficeWebPlugin::GetWeakClient() {
  return GetWeakPtr();
}

base::WeakPtr<OfficeWebPlugin> OfficeWebPlugin::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

// } blink::WebPlugin

}  // namespace electron
