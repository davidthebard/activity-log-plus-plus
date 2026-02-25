#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <3ds.h>

#include "export.h"
#include "pld.h"
#include "title_names.h"
#include "title_db.h"

static void csv_write_escaped(FILE *f, const char *s)
{
    bool needs_quote = false;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n') { needs_quote = true; break; }
    }
    if (!needs_quote) { fputs(s, f); return; }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

static void json_write_escaped(FILE *f, const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
}

Result export_data(const PldFile *pld, const PldSessionLog *sessions)
{
    mkdir("sdmc:/3ds/activity-log-pp", 0755);

    FILE *csv = fopen("sdmc:/3ds/activity-log-pp/export.csv", "w");
    FILE *json = fopen("sdmc:/3ds/activity-log-pp/export.json", "w");
    if (!csv || !json) {
        if (csv) fclose(csv);
        if (json) fclose(json);
        return -1;
    }

    fputs("title_id,name,playtime_secs,playtime,launches,sessions,avg_session_length_secs,avg_session_length,first_played,last_played\n", csv);

    fputs("{\n  \"titles\": [\n", json);

    bool first_json = true;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
        const PldSummary *s = &pld->summaries[i];
        if (pld_summary_is_empty(s)) continue;

        const char *name = title_name_lookup(s->title_id);
        if (!name) name = title_db_lookup(s->title_id);
        if (!name) name = "Unknown";

        char time_buf[32], first_buf[16], last_buf[16];
        pld_fmt_time(s->total_secs, time_buf, sizeof(time_buf));
        pld_fmt_date(s->first_played_days, first_buf, sizeof(first_buf));
        pld_fmt_date(s->last_played_days, last_buf, sizeof(last_buf));

        int sess_count = pld_count_sessions_for(sessions, s->title_id);
        u32 avg_secs = (s->launch_count > 0) ? (s->total_secs / s->launch_count) : 0;
        char avg_buf[20];
        pld_fmt_time(avg_secs, avg_buf, sizeof(avg_buf));

        fprintf(csv, "%016llX,", (unsigned long long)s->title_id);
        csv_write_escaped(csv, name);
        fprintf(csv, ",%lu,%s,%u,%d,%lu,%s,%s,%s\n",
                (unsigned long)s->total_secs, time_buf,
                s->launch_count, sess_count,
                (unsigned long)avg_secs, avg_buf,
                first_buf, last_buf);

        if (!first_json) fputs(",\n", json);
        first_json = false;
        fprintf(json, "    {\n");
        fprintf(json, "      \"title_id\": \"%016llX\",\n", (unsigned long long)s->title_id);
        fprintf(json, "      \"name\": \"");
        json_write_escaped(json, name);
        fprintf(json, "\",\n");
        fprintf(json, "      \"playtime_secs\": %lu,\n", (unsigned long)s->total_secs);
        fprintf(json, "      \"playtime\": \"%s\",\n", time_buf);
        fprintf(json, "      \"launches\": %u,\n", s->launch_count);
        fprintf(json, "      \"sessions\": %d,\n", sess_count);
        fprintf(json, "      \"avg_session_length_secs\": %lu,\n", (unsigned long)avg_secs);
        fprintf(json, "      \"avg_session_length\": \"%s\",\n", avg_buf);
        fprintf(json, "      \"first_played\": \"%s\",\n", first_buf);
        fprintf(json, "      \"last_played\": \"%s\"\n", last_buf);
        fprintf(json, "    }");
    }

    fputs("\n  ]\n}\n", json);

    fclose(csv);
    fclose(json);
    return 0;
}

void export_work(void *raw) {
    ExportArgs *a = (ExportArgs *)raw;
    a->rc = export_data(a->pld, a->sessions);
}
