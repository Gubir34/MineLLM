/* MineLLM: a Minecraft player driven by a language model, on the native
 * MineMCP core. */
#include "window.h"

static void on_activate(GApplication *app, gpointer ud)
{
    (void)ud;
    minellm_window_activate(ADW_APPLICATION(app));
}

int main(int argc, char **argv)
{
    g_set_application_name("MineLLM");
    AdwApplication *app = adw_application_new("io.github.gubir34.MineLLM", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
