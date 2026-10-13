#pragma once
#ifndef RELEASE
/* Initial absence is a baseline; initial presence counts as an appearance. */
typedef struct { int present; } toggle_file_edges;
static inline int toggle_file_edge(toggle_file_edges *s,int present)
{
    present=!!present;
    if (present==s->present) return -1;
    s->present=present; return present;
}
void toggle_file_start(void);
#endif
