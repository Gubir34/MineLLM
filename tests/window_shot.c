/* Developer tool: opens the main window and saves a PNG of it.
 *   window_shot OUT.png [preferences | live SECONDS "request"]
 * live: presses Connect, sends the request, and shoots after SECONDS.
 * Run it on a headless backend: GDK_BACKEND=broadway with gtk4-broadwayd. */
#include "../src/window.h"

#include <stdlib.h>

static const char *out, *live_request;
static gboolean prefs;
static int live_seconds;

/* The first widget under w matching the test. */
static GtkWidget *find(GtkWidget *w, gboolean (*test)(GtkWidget *))
{
    if (test(w))
        return w;
    for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *r = find(c, test);
        if (r)
            return r;
    }
    return NULL;
}

static gboolean is_connect(GtkWidget *w)
{
    return GTK_IS_BUTTON(w) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)), "Connect");
}

static gboolean is_prompt(GtkWidget *w)
{
    return GTK_IS_ENTRY(w) && g_str_has_prefix(gtk_entry_get_placeholder_text(GTK_ENTRY(w)) ?: "", "Tell the bot");
}

static gboolean send_request(gpointer ud)
{
    GtkWidget *e = find(GTK_WIDGET(gtk_application_get_active_window(ud)), is_prompt);
    gtk_editable_set_text(GTK_EDITABLE(e), live_request);
    g_signal_emit_by_name(e, "activate");
    return G_SOURCE_REMOVE;
}

static gboolean press_connect(gpointer ud)
{
    GtkWidget *b = find(GTK_WIDGET(gtk_application_get_active_window(ud)), is_connect);
    g_signal_emit_by_name(b, "clicked");
    return G_SOURCE_REMOVE;
}

static gboolean shoot(gpointer ud)
{
    GtkApplication *app = ud;
    GtkWindow *win = gtk_application_get_active_window(app);
    GList *wins = gtk_window_list_toplevels();
    for (GList *l = wins; l; l = l->next) /* a dialog is its own toplevel only on some backends */
        if (gtk_widget_get_mapped(l->data) && GTK_WINDOW(l->data) != win && prefs)
            win = l->data;
    g_list_free(wins);
    int w = gtk_widget_get_width(GTK_WIDGET(win)), h = gtk_widget_get_height(GTK_WIDGET(win));
    GdkPaintable *p = gtk_widget_paintable_new(GTK_WIDGET(win));
    GtkSnapshot *s = gtk_snapshot_new();
    gdk_paintable_snapshot(p, s, w, h);
    GskRenderNode *node = gtk_snapshot_free_to_node(s);
    GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(win));
    GdkTexture *t = gsk_renderer_render_texture(r, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
    gdk_texture_save_to_png(t, out);
    g_print("saved %dx%d to %s\n", w, h, out);
    g_object_unref(t);
    gsk_render_node_unref(node);
    g_object_unref(p);
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

static gboolean open_prefs(gpointer ud)
{
    g_action_group_activate_action(G_ACTION_GROUP(ud), "preferences", NULL);
    return G_SOURCE_REMOVE;
}

static void on_activate(GApplication *app, gpointer ud)
{
    (void)ud;
    /* no frame clock ticks without a browser on broadway: no animations */
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    minellm_window_activate(ADW_APPLICATION(app));
    if (prefs)
        g_timeout_add(800, open_prefs, app);
    if (live_request) {
        g_timeout_add(500, press_connect, app);
        g_timeout_add(8000, send_request, app);
        g_timeout_add_seconds(live_seconds, shoot, app);
        return;
    }
    g_timeout_add(2500, shoot, app);
}

int main(int argc, char **argv)
{
    out = argc > 1 ? argv[1] : "minellm.png";
    prefs = argc > 2 && !g_strcmp0(argv[2], "preferences");
    if (argc > 4 && !g_strcmp0(argv[2], "live")) {
        live_seconds = atoi(argv[3]);
        live_request = argv[4];
    }
    AdwApplication *app = adw_application_new("io.github.gubir34.MineLLM.Shot", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    char *args[] = {argv[0], NULL};
    int st = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    return st;
}
