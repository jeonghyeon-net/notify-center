/*
 * notify-center — 알림 서버 + 팝업 + 패널 종 버튼 알림 창 (dunst 대체, 상주형)
 *
 * - org.freedesktop.Notifications 를 직접 처리. 팝업이 사라져도 알림은 "살아 있는" 채로 목록에 남음
 *   (앱에 닫힘 신호를 보내지 않음 → 목록에서 눌러도 Slack 스레드 이동 등 원래 동작이 그대로 됨)
 * - 팝업: 오른쪽 아래, 최대 3개 (넘치면 오래된 팝업만 내려감, 알림은 목록에 남음). 시간 지나면 팝업만 사라짐
 *   왼쪽 클릭 = 알림 동작 실행 + 해결 / 오른쪽 클릭 = 팝업 모두 내리기 (목록엔 남음)
 * - 알림 창: Toggle 로 열고 닫음. 카드 클릭 = 동작 실행 + 해결 / ✕ = 해결 / 모두 지우기
 *   열려 있는 동안 새 알림은 팝업 없이 목록에 바로 추가
 * - 해결 안 된 개수가 바뀌면 패널 genmon 배지 갱신 신호를 보냄
 * - 일시적 알림(transient, 볼륨 표시 등)은 목록에 남기지 않음
 *
 * 빌드: make  (설치: make install — README.md 참고)
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

#define PANEL_EVENT  "--plugin-event=genmon:refresh:bool:true" /* 모든 genmon 플러그인 새로 고침 */
#define CENTER_W     380
#define ICON_C       24                                    /* 알림 창 아이콘 */
#define TEXT_W_C     (CENTER_W - 12 - 14 - ICON_C - 20 - 16) /* 목록 여백, 카드 여백, 아이콘, ✕, 간격 */
#define MAX_HEIGHT   620
#define POPUP_W      360
#define ICON_P       32                                    /* 팝업 아이콘 */
#define TEXT_W_P     (POPUP_W - 2 - 24 - ICON_P - 10)       /* 테두리, 안쪽 여백, 아이콘, 간격 */
#define MAX_POPUPS   3
#define POPUP_MARGIN 8
#define POPUP_GAP    6
#define DEFAULT_TIMEOUT_MS 10000

enum { REASON_EXPIRED = 1, REASON_DISMISSED = 2, REASON_CLOSED = 3 };

static const char *CSS =
    "#center { background-color: @theme_bg_color; border: 1px solid rgba(255,255,255,0.12); }"
    "#header { padding: 4px 4px 2px 10px; }"
    "#header button { padding: 2px 6px; min-height: 0; font-size: 9pt; }"
    "#title { font-weight: 700; }"
    "#count, .meta { color: rgba(255,255,255,0.55); }"
    ".meta { font-size: 9pt; }"
    ".card { background-color: rgba(255,255,255,0.05); border-radius: 6px; padding: 7px 4px 7px 10px; }"
    ".card.hover { background-color: rgba(255,255,255,0.10); }"
    ".summary { font-weight: 700; }"
    ".body { color: rgba(255,255,255,0.8); }"
    ".dismiss { min-width: 20px; min-height: 20px; padding: 0; border-radius: 10px; font-size: 8pt; }"
    "#empty { color: rgba(255,255,255,0.5); padding: 16px 0 18px 0; }"
    "window.popup { background-color: @theme_bg_color; border: 1px solid #4c4c4c; }"
    "window.popup.critical { border-color: #F28B82; }"
    ".popup-body { padding: 10px 12px; }";

static const char *INTROSPECTION =
    "<node>"
    " <interface name='org.freedesktop.Notifications'>"
    "  <method name='GetCapabilities'><arg direction='out' type='as'/></method>"
    "  <method name='Notify'>"
    "   <arg direction='in' type='s'/><arg direction='in' type='u'/><arg direction='in' type='s'/>"
    "   <arg direction='in' type='s'/><arg direction='in' type='s'/><arg direction='in' type='as'/>"
    "   <arg direction='in' type='a{sv}'/><arg direction='in' type='i'/><arg direction='out' type='u'/>"
    "  </method>"
    "  <method name='CloseNotification'><arg direction='in' type='u'/></method>"
    "  <method name='GetServerInformation'>"
    "   <arg direction='out' type='s'/><arg direction='out' type='s'/>"
    "   <arg direction='out' type='s'/><arg direction='out' type='s'/>"
    "  </method>"
    "  <signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>"
    "  <signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>"
    " </interface>"
    " <interface name='local.notify.Center'>"
    "  <method name='Toggle'/>"
    "  <method name='Count'><arg direction='out' type='u'/></method>"
    "  <method name='Quit'/>"
    " </interface>"
    "</node>";

typedef struct {
    guint32 id;
    char *app_name, *summary, *body_markup, *desktop_entry, *stack_tag, *sender;
    gboolean has_default, transient, resident;
    guint8 urgency;
    gint32 timeout;
    gint64 created;
    GdkPixbuf *icon; /* 최대 48px */
    GtkWidget *popup;
    guint timer;
} Notif;

static GDBusConnection *bus;
static GList *notifs; /* 오래된 것 → 최신 */
static GList *popups; /* 표시 중인 팝업 (오래된 것 → 최신) */
static guint32 next_id = 1;
static guint refresh_source;
static GtkWidget *win, *scroll, *list_box, *count_label, *clear_btn, *empty_label;
static int n_cards;
static gint64 hidden_by_focus_at; /* 포커스를 잃어 닫힌 시각: 그 원인이 종 버튼 클릭이면 다시 열지 않음 */

static void center_populate(void);
static void relayout_popups(void);

/* ---------- 공통 ---------- */

static Notif *notif_find(guint32 id)
{
    for (GList *l = notifs; l; l = l->next)
        if (((Notif *)l->data)->id == id)
            return l->data;
    return NULL;
}

static guint count_unresolved(void)
{
    guint c = 0;
    for (GList *l = notifs; l; l = l->next)
        c += !((Notif *)l->data)->transient;
    return c;
}

static gboolean center_visible(void)
{
    return win && gtk_widget_get_visible(win);
}

static gboolean do_refresh(gpointer data)
{
    const char *argv[] = { "xfce4-panel", PANEL_EVENT, NULL };
    refresh_source = 0;
    g_spawn_async(NULL, (char **)argv, NULL,
                  G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    return G_SOURCE_REMOVE;
}

static void count_changed(void)
{
    if (!refresh_source) /* 알림이 몰려와도 배지 갱신은 한 번 */
        refresh_source = g_timeout_add(80, do_refresh, NULL);
}

static char *ago(gint64 ts)
{
    gint64 s = (g_get_monotonic_time() - ts) / G_USEC_PER_SEC;
    if (s < 60)
        return g_strdup("방금");
    if (s < 3600)
        return g_strdup_printf("%d분 전", (int)(s / 60));
    if (s < 86400)
        return g_strdup_printf("%d시간 전", (int)(s / 3600));
    return g_strdup_printf("%d일 전", (int)(s / 86400));
}

static char *sanitize_markup(const char *s)
{
    /* 본문 마크업: 표시할 수 없는 <a>, <img> 태그는 빼고, 그래도 깨져 있으면 글자 그대로 */
    static GRegex *re;
    if (!re)
        re = g_regex_new("<\\s*/?\\s*(a|img)\\b[^>]*>", G_REGEX_CASELESS, 0, NULL);
    char *t = g_regex_replace_literal(re, s ? s : "", -1, 0, "", 0, NULL);
    if (t && pango_parse_markup(t, -1, 0, NULL, NULL, NULL, NULL))
        return t;
    g_free(t);
    return g_markup_escape_text(s ? s : "", -1);
}

/* ---------- 아이콘 ---------- */

static GdkPixbuf *limit_size(GdkPixbuf *pb)
{
    if (!pb)
        return NULL;
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), m = MAX(w, h);
    if (m <= 48)
        return pb;
    GdkPixbuf *s = gdk_pixbuf_scale_simple(pb, MAX(1, w * 48 / m), MAX(1, h * 48 / m), GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    return s;
}

static GdkPixbuf *pixbuf_from_image_data(GVariant *v)
{
    if (!g_variant_is_of_type(v, G_VARIANT_TYPE("(iiibiiay)")))
        return NULL;
    gint32 w, h, rs, bps, ch;
    gboolean alpha;
    GVariant *data;
    g_variant_get(v, "(iiibii@ay)", &w, &h, &rs, &alpha, &bps, &ch, &data);
    GdkPixbuf *pb = NULL;
    gsize len = g_variant_get_size(data);
    if (w > 0 && h > 0 && bps == 8 && (ch == 3 || ch == 4) && (ch == 4) == !!alpha && rs >= w * ch &&
        (gsize)rs * (h - 1) + (gsize)w * ch <= len) {
        GBytes *b = g_variant_get_data_as_bytes(data);
        pb = gdk_pixbuf_new_from_bytes(b, GDK_COLORSPACE_RGB, alpha, 8, w, h, rs);
        g_bytes_unref(b);
    }
    g_variant_unref(data);
    return limit_size(pb);
}

static GdkPixbuf *pixbuf_from_name(const char *s)
{
    if (!s || !*s)
        return NULL;
    GdkPixbuf *pb = NULL;
    char *path = g_str_has_prefix(s, "file://") ? g_filename_from_uri(s, NULL, NULL) : g_strdup(s);
    if (path && path[0] == '/')
        pb = gdk_pixbuf_new_from_file_at_scale(path, 48, 48, TRUE, NULL);
    else
        pb = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), s, 48, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    g_free(path);
    return pb;
}

static GtkWidget *icon_widget(Notif *n, int size)
{
    GdkPixbuf *pb = n->icon ? g_object_ref(n->icon) : NULL;
    if (!pb) {
        char *lower = g_ascii_strdown(n->desktop_entry && *n->desktop_entry ? n->desktop_entry : n->app_name, -1);
        pb = pixbuf_from_name(lower);
        g_free(lower);
    }
    if (!pb)
        pb = pixbuf_from_name("dialog-information");
    if (!pb)
        return gtk_image_new();
    int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), m = MAX(w, h);
    GdkPixbuf *s = gdk_pixbuf_scale_simple(pb, MAX(1, w * size / m), MAX(1, h * size / m), GDK_INTERP_BILINEAR);
    GtkWidget *img = gtk_image_new_from_pixbuf(s);
    g_object_unref(s);
    g_object_unref(pb);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_widget_set_margin_top(img, 2);
    return img;
}

/* ---------- 앱 창으로 이동 (알림에 동작이 없거나 앱이 창을 못 띄울 때) ---------- */

static gboolean class_matches(Display *dpy, Window w, const char *want)
{
    XClassHint ch = { NULL, NULL };
    gboolean ok = FALSE;
    if (w && XGetClassHint(dpy, w, &ch)) {
        ok = (ch.res_name && g_ascii_strcasecmp(ch.res_name, want) == 0) ||
             (ch.res_class && g_ascii_strcasecmp(ch.res_class, want) == 0);
        if (ch.res_name)
            XFree(ch.res_name);
        if (ch.res_class)
            XFree(ch.res_class);
    }
    return ok;
}

static char *wm_name_for(Notif *n)
{
    /* "Google Chrome" → "google-chrome" 처럼 WM_CLASS 형태로 */
    char *s = g_ascii_strdown(n->desktop_entry && *n->desktop_entry ? n->desktop_entry : n->app_name, -1);
    g_strdelimit(s, " ", '-');
    return s;
}

static Window find_app_window(Display *dpy, const char *want)
{
    Atom type;
    int fmt;
    unsigned long cnt = 0, after;
    unsigned char *data = NULL;
    Window found = None;
    if (*want && XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_NET_CLIENT_LIST_STACKING", False),
                                    0, 4096, False, XA_WINDOW, &type, &fmt, &cnt, &after, &data) == Success && data) {
        Window *wins = (Window *)data;
        for (long i = (long)cnt - 1; i >= 0 && found == None; i--) /* 가장 위의 창부터 */
            if (class_matches(dpy, wins[i], want))
                found = wins[i];
        XFree(data);
    }
    return found;
}

static Window active_window(Display *dpy)
{
    Atom type;
    int fmt;
    unsigned long cnt = 0, after;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False), 0, 1, False,
                           XA_WINDOW, &type, &fmt, &cnt, &after, &data) == Success && data) {
        if (cnt)
            w = *(Window *)data;
        XFree(data);
    }
    return w;
}

static gboolean bring_app_front(gpointer data)
{
    char *want = data;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (!class_matches(dpy, active_window(dpy), want)) {
        Window w = find_app_window(dpy, want);
        if (w != None) {
            XEvent e = { 0 };
            e.xclient.type = ClientMessage;
            e.xclient.window = w;
            e.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
            e.xclient.format = 32;
            e.xclient.data.l[0] = 2; /* 사용자 요청으로 취급 → 다른 작업공간·최소화 상태여도 가져옴 */
            e.xclient.data.l[1] = gdk_x11_get_server_time(gtk_widget_get_window(win));
            XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
            XFlush(dpy);
        }
    }
    g_free(want);
    return G_SOURCE_REMOVE;
}

/* ---------- 알림 수명 ---------- */

static void emit(Notif *n, const char *signal, GVariant *params)
{
    g_dbus_connection_emit_signal(bus, n->sender, "/org/freedesktop/Notifications", "org.freedesktop.Notifications",
                                  signal, params, NULL);
}

static void popup_hide(Notif *n)
{
    if (n->timer) {
        g_source_remove(n->timer);
        n->timer = 0;
    }
    if (n->popup) {
        gtk_widget_destroy(n->popup);
        n->popup = NULL;
        popups = g_list_remove(popups, n);
        relayout_popups();
    }
}

static void notif_free(Notif *n)
{
    popup_hide(n);
    g_free(n->app_name);
    g_free(n->summary);
    g_free(n->body_markup);
    g_free(n->desktop_entry);
    g_free(n->stack_tag);
    g_free(n->sender);
    g_clear_object(&n->icon);
    g_free(n);
}

static void notif_close(Notif *n, guint reason)
{
    gboolean listed = !n->transient;
    emit(n, "NotificationClosed", g_variant_new("(uu)", n->id, reason));
    notifs = g_list_remove(notifs, n);
    notif_free(n);
    if (listed) {
        count_changed();
        if (center_visible())
            center_populate();
    }
}

static void notif_activate(Notif *n)
{
    /* 앱에 동작 전달 (Slack 스레드 이동 등). 앱이 창을 못 띄우면 잠시 뒤 앱 창을 직접 앞으로 */
    g_timeout_add(n->has_default ? 400 : 0, bring_app_front, wm_name_for(n));
    if (n->has_default)
        emit(n, "ActionInvoked", g_variant_new("(us)", n->id, "default"));
    if (n->resident)
        popup_hide(n);
    else
        notif_close(n, REASON_DISMISSED);
}

/* ---------- 위젯 ---------- */

static GtkWidget *wrapped_label(const char *css_class, int width)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    /* 폭 고정(최소=자연 폭) → 창 높이가 정확히 계산됨 */
    gtk_widget_set_size_request(l, width, -1);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 1);
    gtk_style_context_add_class(gtk_widget_get_style_context(l), css_class);
    return l;
}

static GtkWidget *content_row(Notif *n, int icon, int text_w, gboolean with_time, int body_lines)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, icon == ICON_P ? 10 : 8);
    gtk_box_pack_start(GTK_BOX(row), icon_widget(n, icon), FALSE, FALSE, 0);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *meta = wrapped_label("meta", text_w);
    const char *app = *n->app_name ? n->app_name : "알 수 없음";
    if (with_time) {
        char *when = ago(n->created), *s = g_strdup_printf("%s · %s", app, when);
        gtk_label_set_text(GTK_LABEL(meta), s);
        g_free(s);
        g_free(when);
    } else {
        gtk_label_set_text(GTK_LABEL(meta), app);
    }
    gtk_box_pack_start(GTK_BOX(text), meta, FALSE, FALSE, 0);

    if (*n->summary) {
        GtkWidget *sum = wrapped_label("summary", text_w);
        gtk_label_set_text(GTK_LABEL(sum), n->summary);
        gtk_box_pack_start(GTK_BOX(text), sum, FALSE, FALSE, 0);
    }
    if (*n->body_markup) {
        GtkWidget *b = wrapped_label("body", text_w);
        gtk_label_set_lines(GTK_LABEL(b), body_lines);
        gtk_label_set_ellipsize(GTK_LABEL(b), PANGO_ELLIPSIZE_END);
        gtk_label_set_markup(GTK_LABEL(b), n->body_markup);
        gtk_box_pack_start(GTK_BOX(text), b, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    return row;
}

static void set_pointer_cursor(GtkWidget *w, gpointer data)
{
    GdkCursor *c = gdk_cursor_new_from_name(gdk_display_get_default(), "pointer");
    gdk_window_set_cursor(gtk_widget_get_window(w), c);
    g_object_unref(c);
}

/* ---------- 팝업 ---------- */

static void relayout_popups(void)
{
    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m)
        m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle wa;
    gdk_monitor_get_workarea(m, &wa);
    int y = wa.y + wa.height - POPUP_MARGIN;
    for (GList *l = g_list_last(popups); l; l = l->prev) { /* 최신이 맨 아래 */
        GtkWidget *p = ((Notif *)l->data)->popup;
        int h = 0;
        gtk_widget_get_preferred_height_for_width(p, POPUP_W, NULL, &h);
        y -= h;
        gtk_window_move(GTK_WINDOW(p), wa.x + wa.width - POPUP_W - POPUP_MARGIN, y);
        y -= POPUP_GAP;
    }
}

static gboolean on_popup_expire(gpointer data)
{
    Notif *n = notif_find(GPOINTER_TO_UINT(data));
    if (!n)
        return G_SOURCE_REMOVE;
    n->timer = 0;
    if (n->transient)
        notif_close(n, REASON_EXPIRED);
    else
        popup_hide(n); /* 팝업만 내림. 알림은 살아 있는 채로 목록에 남음 */
    return G_SOURCE_REMOVE;
}

static gboolean on_popup_click(GtkWidget *w, GdkEventButton *e, gpointer data)
{
    Notif *n = notif_find(GPOINTER_TO_UINT(data));
    if (!n)
        return TRUE;
    if (e->button == 1) {
        notif_activate(n);
    } else if (e->button == 3) { /* 팝업 모두 내리기 (목록엔 남음) */
        GList *copy = g_list_copy(popups);
        for (GList *l = copy; l; l = l->next) {
            Notif *p = l->data;
            if (p->transient)
                notif_close(p, REASON_DISMISSED);
            else
                popup_hide(p);
        }
        g_list_free(copy);
    }
    return TRUE;
}

static void popup_show(Notif *n)
{
    popup_hide(n);
    GtkWidget *p = gtk_window_new(GTK_WINDOW_POPUP);
    GtkStyleContext *sc = gtk_widget_get_style_context(p);
    gtk_style_context_add_class(sc, "popup");
    if (n->urgency == 2)
        gtk_style_context_add_class(sc, "critical");
    gtk_window_set_default_size(GTK_WINDOW(p), POPUP_W, -1);
    gtk_widget_add_events(p, GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(p, "button-release-event", G_CALLBACK(on_popup_click), GUINT_TO_POINTER(n->id));
    g_signal_connect(p, "realize", G_CALLBACK(set_pointer_cursor), NULL);

    GtkWidget *body = content_row(n, ICON_P, TEXT_W_P, FALSE, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(body), "popup-body");
    gtk_container_add(GTK_CONTAINER(p), body);
    gtk_widget_show_all(body);
    n->popup = p;
    popups = g_list_append(popups, n);

    while (g_list_length(popups) > MAX_POPUPS) { /* 넘치면 가장 오래된 팝업만 내림 */
        Notif *old = popups->data;
        if (old->transient)
            notif_close(old, REASON_EXPIRED);
        else
            popup_hide(old);
    }
    relayout_popups();
    gtk_widget_show(p);

    gint32 t = n->timeout < 0 ? (n->urgency == 2 ? 0 : DEFAULT_TIMEOUT_MS) : n->timeout;
    if (t > 0)
        n->timer = g_timeout_add(t, on_popup_expire, GUINT_TO_POINTER(n->id));
}

/* ---------- 알림 창 ---------- */

static void fit_height(void)
{
    /* 창 폭 기준으로 목록 높이를 직접 계산 (본문 줄 제한까지 반영), MAX_HEIGHT 넘으면 스크롤 */
    int nat = 0;
    gtk_widget_get_preferred_height_for_width(list_box, CENTER_W, NULL, &nat);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), MIN(nat, MAX_HEIGHT));
}

static void update_header(void)
{
    char *t = n_cards ? g_strdup_printf("%d개", n_cards) : g_strdup("");
    gtk_label_set_text(GTK_LABEL(count_label), t);
    g_free(t);
    gtk_widget_set_sensitive(clear_btn, n_cards > 0);
    if (n_cards == 0 && !empty_label) {
        empty_label = gtk_label_new("새 알림이 없습니다");
        gtk_widget_set_name(empty_label, "empty");
        gtk_box_pack_start(GTK_BOX(list_box), empty_label, FALSE, FALSE, 0);
        gtk_widget_show(empty_label);
    }
    fit_height();
}

static void on_dismiss(GtkButton *b, gpointer data)
{
    Notif *n = notif_find(GPOINTER_TO_UINT(data));
    if (n)
        notif_close(n, REASON_DISMISSED); /* 목록은 notif_close 에서 다시 그림 */
}

static gboolean on_card_click(GtkWidget *ev, GdkEventButton *e, gpointer data)
{
    GtkWidget *x = g_object_get_data(G_OBJECT(ev), "x");
    GtkWidget *src = gtk_get_event_widget((GdkEvent *)e);
    if (e->button != 1 || (src && (src == x || gtk_widget_is_ancestor(src, x))))
        return FALSE; /* ✕ 버튼 클릭은 따로 처리 */
    Notif *n = notif_find(GPOINTER_TO_UINT(data));
    gtk_widget_hide(win);
    if (n)
        notif_activate(n);
    return TRUE;
}

static gboolean on_card_hover(GtkWidget *ev, GdkEventCrossing *e, gpointer card)
{
    if (e->detail == GDK_NOTIFY_INFERIOR) /* ✕ 버튼 위로 옮겨간 것은 카드를 벗어난 게 아님 */
        return FALSE;
    GtkStyleContext *sc = gtk_widget_get_style_context(GTK_WIDGET(card));
    if (e->type == GDK_ENTER_NOTIFY)
        gtk_style_context_add_class(sc, "hover");
    else
        gtk_style_context_remove_class(sc, "hover");
    return FALSE;
}

static GtkWidget *make_card(Notif *n)
{
    GtkWidget *card = content_row(n, ICON_C, TEXT_W_C, TRUE, 3);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");

    GtkWidget *x = gtk_button_new_with_label("✕");
    gtk_button_set_relief(GTK_BUTTON(x), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(x, FALSE); /* 키보드 입력으로 눌리지 않게 */
    gtk_widget_set_valign(x, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(x, "해결 (목록에서 지우기)");
    gtk_style_context_add_class(gtk_widget_get_style_context(x), "dismiss");
    g_signal_connect(x, "clicked", G_CALLBACK(on_dismiss), GUINT_TO_POINTER(n->id));
    gtk_box_pack_end(GTK_BOX(card), x, FALSE, FALSE, 0);

    GtkWidget *ev = gtk_event_box_new(); /* 카드 전체를 클릭 가능하게 */
    gtk_widget_add_events(ev, GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_container_add(GTK_CONTAINER(ev), card);
    gtk_widget_set_tooltip_text(ev, "눌러서 이동 (알림 해결)");
    g_object_set_data(G_OBJECT(ev), "x", x);
    g_signal_connect(ev, "button-release-event", G_CALLBACK(on_card_click), GUINT_TO_POINTER(n->id));
    g_signal_connect(ev, "enter-notify-event", G_CALLBACK(on_card_hover), card);
    g_signal_connect(ev, "leave-notify-event", G_CALLBACK(on_card_hover), card);
    g_signal_connect(ev, "realize", G_CALLBACK(set_pointer_cursor), NULL);
    gtk_widget_show_all(ev); /* 보이는 상태여야 높이 계산에 포함됨 */
    return ev;
}

static void destroy_child(GtkWidget *w, gpointer data)
{
    gtk_widget_destroy(w);
}

static void center_populate(void)
{
    gtk_container_foreach(GTK_CONTAINER(list_box), destroy_child, NULL);
    empty_label = NULL;
    n_cards = 0;
    for (GList *l = g_list_last(notifs); l; l = l->prev) { /* 최신이 위 */
        Notif *n = l->data;
        if (n->transient)
            continue;
        gtk_box_pack_start(GTK_BOX(list_box), make_card(n), FALSE, FALSE, 0);
        n_cards++;
    }
    update_header();
}

static void place_center(int w, int h)
{
    GdkDisplay *dpy = gdk_display_get_default();
    GdkMonitor *m = gdk_display_get_primary_monitor(dpy);
    if (!m)
        m = gdk_display_get_monitor(dpy, 0);
    GdkRectangle wa;
    gdk_monitor_get_workarea(m, &wa);
    int x = wa.x + wa.width - w, y = wa.y + wa.height - h, cx, cy; /* 패널 위, 오른쪽 끝에 딱 붙임 */
    gtk_window_get_position(GTK_WINDOW(win), &cx, &cy);
    if (cx != x || cy != y)
        gtk_window_move(GTK_WINDOW(win), x, y);
}

static void on_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer data)
{
    place_center(a->width, a->height); /* 항목을 지워 창이 줄어도 붙어 있게 */
}

static void center_show(void)
{
    GList *copy = g_list_copy(popups); /* 떠 있는 팝업은 내림 (목록에 다 있음) */
    for (GList *l = copy; l; l = l->next)
        if (!((Notif *)l->data)->transient)
            popup_hide(l->data);
    g_list_free(copy);

    center_populate();
    GtkRequisition nat;
    gtk_widget_show(win);
    gtk_widget_get_preferred_size(win, NULL, &nat);
    place_center(nat.width, nat.height);
    gtk_window_present_with_time(GTK_WINDOW(win), gdk_x11_get_server_time(gtk_widget_get_window(win)));
}

static void center_toggle(void)
{
    if (center_visible()) {
        gtk_widget_hide(win);
    } else if (hidden_by_focus_at && g_get_monotonic_time() - hidden_by_focus_at < 300000) {
        hidden_by_focus_at = 0; /* 열린 상태에서 종 버튼을 눌러 닫힌 경우: 다시 열지 않음 */
    } else {
        center_show();
    }
}

static gboolean on_focus_out(GtkWidget *w, GdkEvent *e, gpointer data)
{
    if (center_visible()) {
        hidden_by_focus_at = g_get_monotonic_time();
        gtk_widget_hide(win);
    }
    return FALSE;
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer data)
{
    if (e->keyval == GDK_KEY_Escape) {
        gtk_widget_hide(win);
        return TRUE;
    }
    return FALSE;
}

static void on_clear_all(GtkButton *b, gpointer data)
{
    GList *copy = g_list_copy(notifs);
    for (GList *l = copy; l; l = l->next)
        if (!((Notif *)l->data)->transient)
            notif_close(l->data, REASON_DISMISSED);
    g_list_free(copy);
    gtk_widget_hide(win);
}

static void build_center(void)
{
    win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(win, "center");
    gtk_window_set_title(GTK_WINDOW(win), "알림");
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
    g_signal_connect(win, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(on_focus_out), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(win, "size-allocate", G_CALLBACK(on_size_allocate), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(outer, CENTER_W, -1);
    gtk_container_add(GTK_CONTAINER(win), outer);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_name(header, "header");
    GtkWidget *title = gtk_label_new("알림");
    gtk_widget_set_name(title, "title");
    count_label = gtk_label_new(NULL);
    gtk_widget_set_name(count_label, "count");
    clear_btn = gtk_button_new_with_label("모두 지우기");
    gtk_button_set_relief(GTK_BUTTON(clear_btn), GTK_RELIEF_NONE);
    gtk_widget_set_can_focus(clear_btn, FALSE); /* 키보드 입력으로 눌리지 않게 */
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_all), NULL);
    gtk_box_pack_start(GTK_BOX(header), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), count_label, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(header), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), MAX_HEIGHT);
    list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(list_box, 6);
    gtk_widget_set_margin_end(list_box, 6);
    gtk_widget_set_margin_bottom(list_box, 6);
    gtk_container_add(GTK_CONTAINER(scroll), list_box);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(outer); /* 창은 숨긴 채로, 내용은 표시 상태로 둬야 높이 계산이 됨 */
    gtk_widget_realize(win);    /* 서버 시각 조회용 */
}

/* ---------- D-Bus ---------- */

static const char *hint_str(GVariant *hints, const char *a, const char *b)
{
    const char *s = NULL;
    if (!g_variant_lookup(hints, a, "&s", &s) && b)
        g_variant_lookup(hints, b, "&s", &s);
    return s;
}

static guint32 handle_notify(GVariant *params, const char *sender)
{
    const char *app_name, *app_icon, *summary, *body;
    guint32 replaces_id;
    gint32 timeout;
    GVariant *actions, *hints;
    g_variant_get(params, "(&su&s&s&s@as@a{sv}i)", &app_name, &replaces_id, &app_icon, &summary, &body, &actions,
                  &hints, &timeout);

    const char *tag = hint_str(hints, "x-canonical-private-synchronous", "x-dunst-stack-tag");
    if (!tag)
        tag = hint_str(hints, "synchronous", "private-synchronous");
    gboolean transient = FALSE, resident = FALSE;
    g_variant_lookup(hints, "transient", "b", &transient);
    g_variant_lookup(hints, "resident", "b", &resident);
    GVariant *gauge = g_variant_lookup_value(hints, "value", NULL);
    if (tag || gauge)
        transient = TRUE; /* 볼륨·밝기 표시처럼 계속 갱신되는 알림은 목록에 남기지 않음 */

    if (gauge)
        g_variant_unref(gauge);

    Notif *n = replaces_id ? notif_find(replaces_id) : NULL;
    if (!n && tag)
        for (GList *l = notifs; l && !n; l = l->next) {
            Notif *o = l->data;
            if (o->stack_tag && strcmp(o->stack_tag, tag) == 0 && strcmp(o->app_name, app_name) == 0)
                n = o;
        }
    if (n) { /* 기존 알림 갱신: 최신으로 올림 */
        notifs = g_list_remove(notifs, n);
        popup_hide(n);
        g_free(n->app_name);
        g_free(n->summary);
        g_free(n->body_markup);
        g_free(n->desktop_entry);
        g_free(n->stack_tag);
        g_free(n->sender);
        g_clear_object(&n->icon);
    } else {
        n = g_new0(Notif, 1);
        n->id = next_id++;
        if (next_id == 0)
            next_id = 1;
    }
    n->app_name = g_strdup(app_name);
    n->summary = g_strdup(summary);
    n->body_markup = sanitize_markup(body);
    n->desktop_entry = g_strdup(hint_str(hints, "desktop-entry", NULL));
    n->stack_tag = g_strdup(tag);
    n->sender = g_strdup(sender);
    n->transient = transient;
    n->resident = resident;
    n->timeout = timeout;
    n->created = g_get_monotonic_time();
    n->urgency = 1;
    g_variant_lookup(hints, "urgency", "y", &n->urgency);

    gsize na = 0;
    const char **acts = g_variant_get_strv(actions, &na);
    n->has_default = FALSE;
    for (gsize i = 0; i < na; i += 2) /* 동작은 (키, 이름) 쌍 */
        if (strcmp(acts[i], "default") == 0)
            n->has_default = TRUE;
    g_free(acts);

    /* 아이콘 우선순위: image-data > image-path > app_icon > icon_data */
    GVariant *v;
    if ((v = g_variant_lookup_value(hints, "image-data", NULL)) || (v = g_variant_lookup_value(hints, "image_data", NULL))) {
        n->icon = pixbuf_from_image_data(v);
        g_variant_unref(v);
    }
    if (!n->icon)
        n->icon = limit_size(pixbuf_from_name(hint_str(hints, "image-path", "image_path")));
    if (!n->icon)
        n->icon = limit_size(pixbuf_from_name(app_icon));
    if (!n->icon && (v = g_variant_lookup_value(hints, "icon_data", NULL))) {
        n->icon = pixbuf_from_image_data(v);
        g_variant_unref(v);
    }
    g_variant_unref(actions);
    g_variant_unref(hints);

    notifs = g_list_append(notifs, n);
    if (center_visible() && !n->transient)
        center_populate(); /* 창이 열려 있으면 팝업 없이 목록에 바로 */
    else
        popup_show(n);
    if (!n->transient)
        count_changed();
    return n->id;
}

static void on_method(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data)
{
    if (strcmp(method, "Notify") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", handle_notify(params, sender)));
    } else if (strcmp(method, "CloseNotification") == 0) {
        guint32 id;
        g_variant_get(params, "(u)", &id);
        Notif *n = notif_find(id);
        if (n)
            notif_close(n, REASON_CLOSED); /* 앱이 거둬간 알림 (예: 다른 기기에서 읽음) */
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (strcmp(method, "GetCapabilities") == 0) {
        const char *caps[] = { "actions", "body", "body-markup", "icon-static", "persistence", NULL };
        g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", caps));
    } else if (strcmp(method, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "notify-center", "local", "1.0", "1.2"));
    } else if (strcmp(method, "Toggle") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
        center_toggle();
    } else if (strcmp(method, "Count") == 0) {
        g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", count_unresolved()));
    } else if (strcmp(method, "Quit") == 0) { /* notify-center-setup --remove 에서 사용 */
        g_dbus_method_invocation_return_value(inv, NULL);
        gtk_main_quit();
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
    }
}

static const GDBusInterfaceVTable vtable = { on_method, NULL, NULL, { 0 } };

static void on_bus_acquired(GDBusConnection *c, const char *name, gpointer data)
{
    bus = c;
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(INTROSPECTION, NULL);
    for (int i = 0; info->interfaces[i]; i++)
        g_dbus_connection_register_object(c, "/org/freedesktop/Notifications", info->interfaces[i], &vtable,
                                          NULL, NULL, NULL);
    g_dbus_node_info_unref(info);
}

static void on_name_acquired(GDBusConnection *c, const char *name, gpointer data)
{
    count_changed();
}

static void on_name_lost(GDBusConnection *c, const char *name, gpointer data)
{
    g_printerr("notify-center: org.freedesktop.Notifications 이름을 얻지 못함 (다른 알림 서버가 실행 중)\n");
    exit(1);
}

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    build_center();
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.Notifications", G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                   on_bus_acquired, on_name_acquired, on_name_lost, NULL, NULL);
    gtk_main();
    return 0;
}
