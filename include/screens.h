#pragma once
#include <3ds.h>

typedef void (*WorkerFunc)(void *arg);

void draw_spinner(float cx, float cy);
void draw_message_screen_ex(const char *title, const char *body,
                            bool show_spinner);
void draw_message_screen(const char *title, const char *body);
void draw_loading_screen(const char *title, const char *body);
void draw_progress_screen(const char *title, const char *body,
                          int step, int total_steps);

void run_with_spinner(const char *title, const char *body,
                      int step, int total_steps,
                      WorkerFunc func, void *arg);
void run_loading_with_spinner(const char *title, const char *body,
                              WorkerFunc func, void *arg);
