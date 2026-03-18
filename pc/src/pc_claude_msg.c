#include "pc_claude_msg.h"
#include "ac_npc.h"
#include "m_npc.h"
#include "m_msg.h"
#include "m_scene_table.h"
/* m_npc.h -> m_play.h -> m_submenu.h already brings in Submenu, mSM_open_submenu_new2,
   mSM_MODE_IDLE, mSM_OVL_EDITOR, and m_play.h provides the GAME_PLAY layout.
   We only need the mED_TYPE_HBOARD constant from m_editor_ovl.h — define it directly
   to avoid pulling in the heavy m_submenu_ovl.h transitive chain. */
#define mED_TYPE_HBOARD 1  /* editor sub-type: haniwa board (short single-field entry) */
/* Submenu sits at this byte offset within struct game_play_s */
#define GAME_PLAY_SUBMENU_OFFSET 0x1DEC

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <windows.h>

#define HISTORY_MAX_LINES  6    /* 3 exchanges × 2 lines each */
#define MEMORY_MAX_CHARS   300
#define SAVE_DIR           "save\\"

/* ---- Debug log ---- */

static void dbg(const char* fmt, ...) {
    FILE* f = fopen(SAVE_DIR "claude_debug.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

/* ---- API key ---- */

static char s_api_key[256] = {0};

static void ensure_api_key(void) {
    static int done = 0;
    if (done) return;
    done = 1;

    const char* env = getenv("ANTHROPIC_API_KEY");
    if (env && env[0]) {
        strncpy(s_api_key, env, sizeof(s_api_key) - 1);
        dbg("[claude_msg] API key loaded from ANTHROPIC_API_KEY env var\n");
        return;
    }

    FILE* f = fopen(SAVE_DIR "api_key.txt", "r");
    if (f) {
        char line[256] = {0};
        if (fgets(line, sizeof(line), f)) {
            int len = (int)strlen(line);
            while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                               line[len-1] == ' '  || line[len-1] == '\t'))
                line[--len] = '\0';
            if (len > 0) {
                strncpy(s_api_key, line, sizeof(s_api_key) - 1);
                dbg("[claude_msg] API key loaded from " SAVE_DIR "api_key.txt\n");
            }
        }
        fclose(f);
        if (s_api_key[0]) return;
    }

    dbg("[claude_msg] WARNING: no API key found.\n"
        "  Set ANTHROPIC_API_KEY env var, or put your sk-ant-api03-... key\n"
        "  on the first line of " SAVE_DIR "api_key.txt\n");
}

/* ---- JSON helpers ---- */

static void json_escape(const char* src, char* dst, int dst_size) {
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 3; i++) {
        unsigned char c = (unsigned char)src[i];
        if      (c == '"')  { dst[j++] = '\\'; dst[j++] = '"';  }
        else if (c == '\\') { dst[j++] = '\\'; dst[j++] = '\\'; }
        else if (c == '\n') { dst[j++] = '\\'; dst[j++] = 'n';  }
        else if (c == '\r') { dst[j++] = '\\'; dst[j++] = 'r';  }
        else if (c == '\t') { dst[j++] = '\\'; dst[j++] = 't';  }
        else if (c < 0x20)  { /* drop other control chars */ }
        else if (c >= 0x80) { /* drop non-ASCII */ }
        else                { dst[j++] = (char)c; }
    }
    dst[j] = '\0';
}

static int json_extract_string(const char* json, const char* key,
                               char* out, int out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    size_t slen = strlen(search);
    const char* p = json;

    while ((p = strstr(p, search)) != NULL) {
        p += slen;
        const char* q = p;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != ':') continue;
        q++;
        while (*q == ' ' || *q == '\t') q++;
        if (*q != '"') continue;
        q++;
        int i = 0;
        while (*q && i < out_size - 1) {
            if (*q == '\\') {
                q++;
                if (!*q) break;
                switch (*q) {
                    case '"':  out[i++] = '"';  break;
                    case '\\': out[i++] = '\\'; break;
                    case '/':  out[i++] = '/';  break;
                    case 'n':  out[i++] = '\n'; break;
                    case 'r':  out[i++] = '\r'; break;
                    case 't':  out[i++] = '\t'; break;
                    default:   out[i++] = *q;   break;
                }
            } else if (*q == '"') {
                break;
            } else {
                out[i++] = *q;
            }
            q++;
        }
        out[i] = '\0';
        return i;
    }
    return 0;
}

/* ---- Anthropic API call via curl + CreateProcess ---- */

/* Model to use for NPC dialogue generation.
   Update this to switch models — see https://docs.anthropic.com/en/docs/models-overview */
#define CLAUDE_MODEL "claude-haiku-4-5-20251001"

static int call_anthropic_api(const char* prompt, char* out, int out_size,
                               const char* base_path) {
    out[0] = '\0';
    if (!s_api_key[0]) { dbg("[claude_msg] no API key — skipping\n"); return 0; }

    char body_path[560], resp_path[560];
    snprintf(body_path, sizeof(body_path), "%s.apireq",  base_path);
    snprintf(resp_path, sizeof(resp_path), "%s.apiresp", base_path);

    char escaped[4096];
    json_escape(prompt, escaped, sizeof(escaped));

    FILE* f = fopen(body_path, "w");
    if (!f) { dbg("[claude_msg] failed to create request body file\n"); return 0; }
    fprintf(f,
        "{\"model\":\"" CLAUDE_MODEL "\","
        "\"max_tokens\":512,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
        escaped);
    fclose(f);

    char cmdline[1024];
    snprintf(cmdline, sizeof(cmdline),
        "curl.exe -s -X POST"
        " \"https://api.anthropic.com/v1/messages\""
        " -H \"x-api-key: %s\""
        " -H \"anthropic-version: 2023-06-01\""
        " -H \"content-type: application/json\""
        " -d \"@%s\""
        " -o \"%s\"",
        s_api_key, body_path, resp_path);
    dbg("[claude_msg] calling Anthropic API (key: %.8s...)\n", s_api_key);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};

    /* Use PATH to find curl — ships with Windows 10/11 and most package managers */
    BOOL ok = CreateProcessA(NULL, cmdline,
                             NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);

    if (!ok) {
        dbg("[claude_msg] curl not found in PATH — install curl or add it to PATH (%lu)\n", GetLastError());
        remove(body_path);
        return 0;
    }

    dbg("[claude_msg] curl PID=%lu, waiting...\n", pi.dwProcessId);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    remove(body_path);
    dbg("[claude_msg] curl exited with code %lu\n", exit_code);

    FILE* rf = fopen(resp_path, "r");
    if (!rf) {
        dbg("[claude_msg] no response file after curl\n");
        return 0;
    }
    char resp[16384] = {0};
    int total = (int)fread(resp, 1, sizeof(resp) - 1, rf);
    fclose(rf);
    remove(resp_path);
    resp[total] = '\0';

    dbg("[claude_msg] HTTP response (first 200): %.200s\n", resp);

    int result = 0;
    const char* content_section = strstr(resp, "\"content\"");
    if (content_section)
        result = json_extract_string(content_section, "text", out, out_size);

    if (result == 0) {
        char err_msg[256] = {0};
        const char* err_sec = strstr(resp, "\"error\"");
        if (err_sec) json_extract_string(err_sec, "message", err_msg, sizeof(err_msg));
        if (!err_msg[0]) json_extract_string(resp, "message", err_msg, sizeof(err_msg));
        if (err_msg[0])
            dbg("[claude_msg] API error: %s\n", err_msg);
        else if (total == 0)
            dbg("[claude_msg] empty response from curl\n");
        else
            dbg("[claude_msg] could not parse text from response\n");
    }
    return result;
}

/* ---- Character encoding ---- */

static unsigned char ascii_to_game(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == ' ' || c == '!' || c == '"' || c == '%' || c == '&' ||
        c == '\'' || c == '(' || c == ')' || c == ',' || c == '-' ||
        c == '.' || c == ':' || c == '?' || c == '@') {
        return c;
    }
    return ' ';
}

static void game_name_to_str(const u8* src, int src_len, char* dst, int dst_size) {
    int i, j = 0;
    for (i = 0; i < src_len && j < dst_size - 1; i++) {
        unsigned char b = src[i];
        if (b == 0) break;
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
            (b >= '0' && b <= '9') || b == '-' || b == '\'') {
            dst[j++] = (char)b;
        }
    }
    dst[j] = '\0';
}

static const char* personality_name(u8 looks) {
    switch (looks) {
        case 0: return "normal";
        case 1: return "peppy";
        case 2: return "lazy";
        case 3: return "jock";
        case 4: return "cranky";
        case 5: return "snooty";
        default: return "normal";
    }
}

/* ---- History helpers ---- */

static void history_path(const char* name, char* out, int out_size) {
    snprintf(out, out_size, "%sclaude_%s.txt", SAVE_DIR, name);
}

static void load_history(const char* path,
                         char* mem_out, int mem_size,
                         char* rec_out, int rec_size) {
    mem_out[0] = '\0';
    rec_out[0] = '\0';

    FILE* f = fopen(path, "r");
    if (!f) return;

    char line[512];
    int in_mem = 0, in_rec = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "[MEMORY]",  8) == 0)  { in_mem = 1; continue; }
        if (strncmp(line, "[/MEMORY]", 9) == 0)  { in_mem = 0; continue; }
        if (strncmp(line, "[RECENT]",  8) == 0)  { in_rec = 1; continue; }
        if (strncmp(line, "[/RECENT]", 9) == 0)  { in_rec = 0; continue; }
        if (in_mem) strncat(mem_out, line, mem_size - strlen(mem_out) - 1);
        if (in_rec) strncat(rec_out, line, rec_size - strlen(rec_out) - 1);
    }
    fclose(f);

    int ml = (int)strlen(mem_out);
    while (ml > 0 && (mem_out[ml-1] == '\n' || mem_out[ml-1] == '\r'))
        mem_out[--ml] = '\0';
}

static int count_lines(const char* s) {
    int n = 0;
    while (*s) { if (*s++ == '\n') n++; }
    return n;
}

static void drop_oldest_lines(char* s, int n) {
    int dropped = 0;
    char* p = s;
    while (*p && dropped < n) {
        if (*p++ == '\n') dropped++;
    }
    memmove(s, p, strlen(p) + 1);
}

static void write_history(const char* path, const char* memory, const char* recent) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "[MEMORY]\n%s\n[/MEMORY]\n[RECENT]\n%s[/RECENT]\n", memory, recent);
    fclose(f);
}

/* ---- Async memory extraction ---- */

typedef struct {
    char hist_path[512];
    char exchange[1024];
    char memory[MEMORY_MAX_CHARS + 16];
} MemExtractArgs;

static DWORD WINAPI memory_extract_thread(LPVOID param) {
    MemExtractArgs* args = (MemExtractArgs*)param;

    char prompt[1536];
    snprintf(prompt, sizeof(prompt),
        "Given this Animal Crossing conversation, extract one memorable fact "
        "about the player based ONLY on what the player said (lines starting with 'P:'). "
        "Ignore the NPC's lines entirely. Reply in 10 words or fewer. "
        "If the player said nothing meaningful, reply with exactly: nothing\n\n"
        "Conversation:\n%s",
        args->exchange);

    char mem_base[560];
    snprintf(mem_base, sizeof(mem_base), "%s.mem", args->hist_path);

    char fact[256] = {0};
    call_anthropic_api(prompt, fact, sizeof(fact), mem_base);

    int len = (int)strlen(fact);
    while (len > 0 && (fact[len-1] == '\n' || fact[len-1] == '\r' || fact[len-1] == ' '))
        fact[--len] = '\0';

    if (len == 0 || _stricmp(fact, "nothing") == 0) { free(args); return 0; }

    char new_memory[MEMORY_MAX_CHARS + 256];
    if (args->memory[0] != '\0')
        snprintf(new_memory, sizeof(new_memory), "%s %s", args->memory, fact);
    else
        snprintf(new_memory, sizeof(new_memory), "%s", fact);

    if ((int)strlen(new_memory) > MEMORY_MAX_CHARS) {
        new_memory[MEMORY_MAX_CHARS] = '\0';
        char* last_dot = strrchr(new_memory, '.');
        if (last_dot) *(last_dot + 1) = '\0';
    }

    char cur_mem[MEMORY_MAX_CHARS + 16] = {0};
    char cur_rec[4096] = {0};
    load_history(args->hist_path, cur_mem, sizeof(cur_mem), cur_rec, sizeof(cur_rec));
    write_history(args->hist_path, new_memory, cur_rec);

    free(args);
    return 0;
}

static void spawn_memory_extract(const char* path,
                                 const char* exchange,
                                 const char* memory) {
    MemExtractArgs* args = (MemExtractArgs*)malloc(sizeof(MemExtractArgs));
    if (!args) return;
    strncpy(args->hist_path, path,     sizeof(args->hist_path) - 1);
    strncpy(args->exchange,  exchange, sizeof(args->exchange)  - 1);
    strncpy(args->memory,    memory,   sizeof(args->memory)    - 1);
    HANDLE h = CreateThread(NULL, 0, memory_extract_thread, args, 0, NULL);
    if (h) CloseHandle(h);
    else   free(args);
}

/* ---- Message buffer builder ---- */

/* How paging works:
   All pages in one buffer (s_msg_buf), separated by BTN+MSGCLEAR:
     - BTN  {0x7F,0x04}: shows ▼ prompt, waits for A
     - MSGCLEAR {0x7F,0x02}: clears box, continues scanning same buffer
   Last page ends with BTN+MSGEND {0x7F,0x00}.                          */

#define WRAP_WIDTH      26
#define LINES_PER_PAGE   4
#define MSG_BUF_MAX   1536

static u8  s_msg_buf[MSG_BUF_MAX];
static int s_msg_len = 0;

/* ---- In-game keyboard input ---- */
/* Buffer layout: 32 chars/line × 4 lines = 128 bytes, matching mED_TYPE_HBOARD.
   Must be pre-filled with CHAR_SPACE (0x20) — the editor treats 0x20 as "empty",
   NOT 0x00, so memset(0) would make the editor think the buffer is full. */
#define KBD_LINE_WIDTH_MAX  192  /* mHB_LINE_WIDTH_MAX */
#define KBD_CHARS_PER_LINE   32
#define KBD_BUF_SIZE        (KBD_CHARS_PER_LINE * 4)  /* 128 bytes */
static u8   s_kbd_buf[KBD_BUF_SIZE]; /* game-encoding buffer written by keyboard */
static int  s_kbd_opened       = 0;   /* 1 while keyboard result is pending */
static char s_player_text[256] = {0}; /* ASCII result after keyboard closes */

/* ---- Conversation loop state ---- */
static ACTOR* s_active_speaker  = NULL; /* NPC actor for the current session */
static int    s_active_msg_no   = 0;    /* msg_no from the original approach */
static int    s_in_conversation = 0;    /* 1 while a Claude back-and-forth is active */

/* Used by mMsg_Check_main_hide() override to keep demo alive during keyboard phase. */
int pc_claude_is_conversation_active(void) {
    return s_in_conversation;
}

/* Convert game-encoding HBOARD buffer to ASCII.
   The buffer is CHAR_SPACE (0x20) padded, not null-terminated. */
static void game_str_to_ascii(const u8* src, int src_size, char* dst, int dst_size) {
    int j = 0;
    for (int i = 0; i < src_size && i < dst_size - 1; i++) {
        unsigned char b = src[i];
        if (b == ' ') { /* CHAR_SPACE — could be typed space or end-of-text padding */
            if (j > 0) dst[j++] = ' '; /* keep it for now; trailing spaces trimmed below */
        } else if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') ||
            (b >= '0' && b <= '9') ||
            b == '!' || b == '"' || b == '\'' ||
            b == ',' || b == '.' || b == '?' || b == '-' ||
            b == '(' || b == ')' || b == ':') {
            dst[j++] = (char)b;
        } else if (b == 0) {
            break; /* null terminator (safety) */
        } else if (j > 0 && dst[j-1] != ' ') {
            dst[j++] = ' '; /* collapse unknown bytes to single space */
        }
    }
    /* trim trailing spaces (which are end-of-text CHAR_SPACE padding) */
    while (j > 0 && dst[j-1] == ' ') j--;
    dst[j] = '\0';
}

static int parse_tag(const char* tag, u8 bytes[4], int* is_page_break) {
    *is_page_break = 0;
    if (strcmp(tag, "BTN") == 0 ||
        strcmp(tag, "MSGCONTINUE") == 0 ||
        strcmp(tag, "MSGEND") == 0) {
        *is_page_break = 1; return 0;
    }
    if (strncmp(tag, "PAUSE", 5) == 0) {
        int val = (tag[5] == ' ' || tag[5] == '_') ? atoi(tag + 6) : 0;
        bytes[0] = 0x7F; bytes[1] = 0x03; bytes[2] = (u8)(val & 0xFF); return 3;
    }
    if (strncmp(tag, "MSGCONTENTS_", 12) == 0) {
        const char* mood = tag + 12;
        u8 mb = 0x47;
        if      (strcmp(mood, "ANGRY")  == 0) mb = 0x48;
        else if (strcmp(mood, "SAD")    == 0) mb = 0x49;
        else if (strcmp(mood, "FUN")    == 0) mb = 0x4A;
        else if (strcmp(mood, "SLEEPY") == 0) mb = 0x4B;
        bytes[0] = 0x7F; bytes[1] = mb; return 2;
    }
    return 0;
}

static void emit_page_break(u8* buf, int* n, int* line_len,
                             int* lines_on_page, int* wrap_pos) {
    buf[(*n)++] = 0x7F; buf[(*n)++] = 0x04; /* BTN     */
    buf[(*n)++] = 0x7F; buf[(*n)++] = 0x02; /* MSGCLEAR */
    *line_len = 0; *lines_on_page = 1; *wrap_pos = -1;
}

static int build_msg_buf(const char* resp) {
    u8* buf = s_msg_buf;
    int n            = 0;
    int line_len     = 0;
    int lines_on_page = 1;
    int wrap_pos     = -1;
    int max          = MSG_BUF_MAX - 4;

    s_msg_len = 0;

    for (int i = 0; resp[i] && n < max; ) {
        unsigned char c = (unsigned char)resp[i];

        if (c == '\r') { i++; continue; }

        if (c == '<' && (unsigned char)resp[i+1] == '<') {
            const char* ts = resp + i + 2;
            const char* te = strstr(ts, ">>");
            if (te && (te - ts) < 64) {
                char tag[64];
                int  tlen = (int)(te - ts);
                memcpy(tag, ts, tlen);
                tag[tlen] = '\0';

                u8  bytes[4] = {0};
                int is_break = 0;
                int blen = parse_tag(tag, bytes, &is_break);

                for (int b = 0; b < blen && n < max; b++)
                    buf[n++] = bytes[b];

                if (is_break && n > 0 && n < max - 3)
                    emit_page_break(buf, &n, &line_len, &lines_on_page, &wrap_pos);

                i = (int)(te - resp) + 2;
                continue;
            }
        }

        if (c == '\n') {
            if (lines_on_page < LINES_PER_PAGE) {
                if (n < max) { buf[n++] = 0xCD; lines_on_page++; }
            } else if (n < max - 3) {
                emit_page_break(buf, &n, &line_len, &lines_on_page, &wrap_pos);
            }
            line_len = 0; wrap_pos = -1;
            i++; continue;
        }

        unsigned char gc = ascii_to_game(c);
        if (c == ' ') wrap_pos = n;
        if (n >= max) { i++; continue; }
        buf[n++] = gc;
        line_len++;
        i++;

        if (line_len >= WRAP_WIDTH && wrap_pos >= 0) {
            int saved_len = n - wrap_pos - 1;

            if (lines_on_page < LINES_PER_PAGE) {
                buf[wrap_pos] = 0xCD;
                lines_on_page++;
                line_len = saved_len;
                wrap_pos = -1;
            } else if (n < max - 3) {
                u8 saved[WRAP_WIDTH + 1];
                if (saved_len > 0)
                    memcpy(saved, buf + wrap_pos + 1, saved_len);
                n = wrap_pos;
                emit_page_break(buf, &n, &line_len, &lines_on_page, &wrap_pos);
                if (saved_len > 0 && n + saved_len < max) {
                    memcpy(buf + n, saved, saved_len);
                    n += saved_len;
                    line_len = saved_len;
                }
            }
        }
    }

    if (n > 0 && n < MSG_BUF_MAX - 3) {
        buf[n++] = 0x7F; buf[n++] = 0x04; /* BTN    */
        buf[n++] = 0x7F; buf[n++] = 0x00; /* MSGEND */
    }
    s_msg_len = n;
    return n;
}

/* ---- Async API call state ---- */

typedef struct {
    char prompt[2048];
    char base_path[560];
    char response[2048];
    int  resp_len;
} AsyncApiArgs;

static HANDLE       s_async_handle   = NULL; /* background thread handle  */
static AsyncApiArgs s_async_args;            /* written by thread, read after join */
static mMsg_Data_c* s_async_msg_data = NULL; /* buffer to populate when done */

/* Pending context for history update (read on main thread after async completes) */
static char s_pending_name[16];
static char s_pending_memory[MEMORY_MAX_CHARS + 16];
static char s_pending_recent[4096];
static char s_pending_hist_path[512];

/* Deferred history write — held until player submits keyboard input */
static char s_pending_response[2048] = {0};
static int  s_history_write_pending  = 0;

static DWORD WINAPI api_call_thread(LPVOID param) {
    AsyncApiArgs* a = (AsyncApiArgs*)param; /* points to s_async_args */
    int len = call_anthropic_api(a->prompt, a->response, sizeof(a->response),
                                 a->base_path);
    /* Trim trailing whitespace */
    while (len > 0 && (a->response[len-1] == '\n' || a->response[len-1] == '\r' ||
                        a->response[len-1] == ' '))
        a->response[--len] = '\0';
    a->resp_len = len;
    return 0;
}

/* ---- Deferred history write ---- */

static void flush_pending_history(void) {
    if (!s_history_write_pending) return;
    s_history_write_pending = 0;

    char player_line[280];
    if (s_player_text[0])
        snprintf(player_line, sizeof(player_line), "P: %s", s_player_text);
    else
        snprintf(player_line, sizeof(player_line), "P: ...");

    char new_recent[4096];
    snprintf(new_recent, sizeof(new_recent), "%s%s\nN: %s\n",
             s_pending_recent, player_line, s_pending_response);
    while (count_lines(new_recent) > HISTORY_MAX_LINES)
        drop_oldest_lines(new_recent, 2);
    write_history(s_pending_hist_path, s_pending_memory, new_recent);

    char exchange[1024];
    snprintf(exchange, sizeof(exchange), "%s\nN: %s",
             player_line, s_pending_response);
    spawn_memory_extract(s_pending_hist_path, exchange, s_pending_memory);
    dbg("[claude_msg] history flushed: %s\n", player_line);
}

/* ---- Public async API ---- */

int pc_claude_is_async_pending(void) {
    return s_async_handle != NULL;
}

/* Called each frame from mMsg_Main_Appear.
   Returns 1 when the thread has finished (response ready or failed).
   Returns 0 while still in flight. */
int pc_claude_poll_async(mMsg_Window_c* win) {
    if (!s_async_handle) return 0;
    if (WaitForSingleObject(s_async_handle, 0) != WAIT_OBJECT_0) return 0;

    /* Thread finished — read results and clean up */
    CloseHandle(s_async_handle);
    s_async_handle = NULL;

    if (s_async_args.resp_len > 0) {
        dbg("[claude_msg] %s: async response: %.120s\n",
            s_pending_name, s_async_args.response);
        int blen = build_msg_buf(s_async_args.response);
        dbg("[claude_msg] %s: built %d byte buffer\n", s_pending_name, blen);
        if (blen > 0 && s_async_msg_data != NULL) {
            memcpy(s_async_msg_data->text_buf.data, s_msg_buf, blen);
            /* msg_len is recounted by the caller (mMsg_Main_Appear) since
               mMsg_Count_MsgData is static inside the message subsystem. */
        }

        /* Store response — history is written after player provides keyboard input */
        strncpy(s_pending_response, s_async_args.response,
                sizeof(s_pending_response) - 1);
        s_history_write_pending = 1;
    } else {
        dbg("[claude_msg] %s: async empty/failed — ROM text fallback\n",
            s_pending_name);
        /* s_async_msg_data buffer still holds the original ROM text — leave it */
    }

    s_async_msg_data = NULL;
    return 1; /* allow CURSOL transition */
}

/* ---- Main intercept (called once per conversation from mMsg_MainSetup_Appear) ---- */

void pc_claude_intercept_message(mMsg_Data_c* msg_data, ACTOR* speaker,
                                  int msg_index) {
    ensure_api_key();

    /* Collect keyboard result from previous conversation */
    if (s_kbd_opened) {
        extern GAME* gamePT;
        Submenu* sm = (Submenu*)((char*)gamePT + GAME_PLAY_SUBMENU_OFFSET);
        if (sm->mode == mSM_MODE_IDLE) {
            game_str_to_ascii(s_kbd_buf, KBD_BUF_SIZE, s_player_text, sizeof(s_player_text));
            dbg("[claude_msg] keyboard result: \"%s\"\n", s_player_text);
        } else {
            s_player_text[0] = '\0'; /* keyboard still open or otherwise busy */
        }
        s_kbd_opened = 0;
    }

    /* Save player's current input before flush clears it */
    char cur_player_input[256] = {0};
    strncpy(cur_player_input, s_player_text, sizeof(cur_player_input) - 1);

    /* Flush pending history before building this conversation's prompt */
    if (s_history_write_pending)
        flush_pending_history();

    /* Guard: skip during player-select / save scenes */
    {
        s16 sid = speaker->scene_id;
        if (sid == SCENE_PLAYERSELECT      ||
            sid == SCENE_PLAYERSELECT_2    ||
            sid == SCENE_PLAYERSELECT_3    ||
            sid == SCENE_PLAYERSELECT_SAVE) {
            dbg("[claude_msg] skipping — player-select scene %d\n", (int)sid);
            return;
        }
    }

    /* Guard: NPC actors only */
    if (speaker->part != ACTOR_PART_NPC) {
        dbg("[claude_msg] skipping — not an NPC (part=%d)\n", (int)speaker->part);
        return;
    }

    /* Guard: don't overlap calls */
    if (s_async_handle != NULL) {
        dbg("[claude_msg] async already in progress — skipping\n");
        return;
    }

    s_msg_len = 0;

    NPC_ACTOR* npc    = (NPC_ACTOR*)speaker;
    Animal_c*  animal = npc->npc_info.animal;
    if (!animal) return;

    /* Resolve name */
    u8 name_buf[ANIMAL_NAME_LEN + 1];
    memset(name_buf, 0, sizeof(name_buf));
    mNpc_LoadNpcNameString(name_buf, animal->id.name_id);

    char name[16] = {0};
    game_name_to_str(name_buf, ANIMAL_NAME_LEN, name, sizeof(name));
    if (name[0] == '\0') return;

    const char* personality = personality_name(animal->id.looks);

    /* Save for conversation loop; re-set by on_dialogue_end if Claude responds */
    s_active_speaker  = speaker;
    s_active_msg_no   = msg_index;
    s_in_conversation = 0;

    /* Load history */
    char hist_path[512];
    history_path(name, hist_path, sizeof(hist_path));

    char memory[MEMORY_MAX_CHARS + 16] = {0};
    char recent[4096] = {0};
    load_history(hist_path, memory, sizeof(memory), recent, sizeof(recent));

    /* Build prompt */
    char prompt[2048];
    int plen = snprintf(prompt, sizeof(prompt),
        "You are %s, an Animal Crossing villager with a %s personality.\n"
        "Ground your conversations in village life — "
        "nature, neighbors, hobbies, items, and daily happenings. "
        "Follow the player's lead if they bring up other topics.\n",
        name, personality);
    if (memory[0] != '\0')
        plen += snprintf(prompt + plen, sizeof(prompt) - plen,
            "What you remember about this player: %s\n", memory);
    if (recent[0] != '\0')
        plen += snprintf(prompt + plen, sizeof(prompt) - plen,
            "Your recent conversations:\n%s\n", recent);
    if (cur_player_input[0] != '\0')
        plen += snprintf(prompt + plen, sizeof(prompt) - plen,
            "The player says to you: \"%s\". Respond to what they said. ",
            cur_player_input);
    else if (recent[0] != '\0')
        plen += snprintf(prompt + plen, sizeof(prompt) - plen,
            "The player approaches you again. ");
    else
        plen += snprintf(prompt + plen, sizeof(prompt) - plen,
            "The player approaches you for the first time. ");
    snprintf(prompt + plen, sizeof(prompt) - plen,
        "Reply in character with 3-5 complete sentences. "
        "Always finish your last sentence - never cut off mid-thought. "
        "Plain text only - no asterisks, no markdown.\n"
        "You may optionally use these dialogue tags for expression:\n"
        "  <<MSGCONTENTS_FUN>>    — cheerful, upbeat tone\n"
        "  <<MSGCONTENTS_ANGRY>>  — irritated, grumpy tone\n"
        "  <<MSGCONTENTS_SAD>>    — melancholy, somber tone\n"
        "  <<MSGCONTENTS_SLEEPY>> — drowsy, mumbling tone\n"
        "  <<MSGCONTENTS_NORMAL>> — reset to neutral tone\n"
        "  Use these anywhere; mood shifts from that point forward.\n"
        "  You may shift mood mid-response (e.g. start happy, turn sad partway through).\n"
        "  <<PAUSE 5>>  anywhere — brief beat (e.g. after a comma)\n"
        "  <<PAUSE 15>> anywhere — half-second pause (e.g. for drama)\n"
        "  <<PAUSE 30>> anywhere — one-second pause (e.g. before a reveal)\n"
        "Do NOT use <<BTN>>, <<MSGCONTINUE>>, or <<MSGEND>> — paging is automatic.");

    /* Store pending context for use after async completes */
    strncpy(s_pending_name,     name,      sizeof(s_pending_name)     - 1);
    strncpy(s_pending_memory,   memory,    sizeof(s_pending_memory)   - 1);
    strncpy(s_pending_recent,   recent,    sizeof(s_pending_recent)   - 1);
    strncpy(s_pending_hist_path, hist_path, sizeof(s_pending_hist_path) - 1);
    s_async_msg_data = msg_data;

    /* Fill async args — thread reads these after we return */
    strncpy(s_async_args.prompt,    prompt,    sizeof(s_async_args.prompt)    - 1);
    strncpy(s_async_args.base_path, hist_path, sizeof(s_async_args.base_path) - 1);
    s_async_args.resp_len  = 0;
    s_async_args.response[0] = '\0';

    dbg("[claude_msg] %s: starting async API call (msg_index=%d)...\n",
        name, msg_index);

    HANDLE h = CreateThread(NULL, 0, api_call_thread, &s_async_args, 0, NULL);
    if (!h) {
        dbg("[claude_msg] %s: CreateThread failed (%lu) — ROM text fallback\n",
            name, GetLastError());
        s_async_msg_data = NULL;
        return;
    }
    s_async_handle = h;
    /* Do NOT CloseHandle — we poll it via WaitForSingleObject in pc_claude_poll_async */
}

/* ---- In-game keyboard trigger ---- */

void pc_claude_on_dialogue_end(GAME* game) {
    if (!s_history_write_pending) return; /* not a Claude conversation */

    Submenu* sm = (Submenu*)((char*)game + GAME_PLAY_SUBMENU_OFFSET);

    /* Only open keyboard if submenu is currently free */
    if (sm->mode != mSM_MODE_IDLE) {
        dbg("[claude_msg] submenu busy (mode=%d) — skipping keyboard\n", sm->mode);
        return;
    }

    memset(s_kbd_buf, ' ', sizeof(s_kbd_buf)); /* fill with CHAR_SPACE (0x20) = "empty" */
    s_player_text[0] = '\0';
    mSM_open_submenu_new2(sm, mSM_OVL_EDITOR, mED_TYPE_HBOARD,
                          KBD_CHARS_PER_LINE, s_kbd_buf, KBD_LINE_WIDTH_MAX);
    s_kbd_opened = 1;
    s_in_conversation = 1;
    dbg("[claude_msg] in-game keyboard opened for player response\n");
}

/* Called every frame from mMsg_Main_Hide while the dialogue window is hidden.
   If the keyboard was closed with text, re-opens the dialogue for the next turn.
   Returns 1 if a re-open was triggered. */
int pc_claude_check_continue(mMsg_Window_c* msg_p, GAME* game) {
    if (!s_in_conversation || !s_kbd_opened) return 0;

    Submenu* sm = (Submenu*)((char*)game + GAME_PLAY_SUBMENU_OFFSET);
    if (sm->mode != mSM_MODE_IDLE) return 0; /* keyboard still open */

    s_kbd_opened = 0;
    game_str_to_ascii(s_kbd_buf, KBD_BUF_SIZE, s_player_text, sizeof(s_player_text));
    dbg("[claude_msg] continue input: \"%s\"\n", s_player_text);

    if (s_player_text[0] == '\0') {
        s_in_conversation = 0;
        dbg("[claude_msg] empty input -- conversation ended\n");
        return 0;
    }

    /* Re-open the dialogue window -- pc_claude_intercept_message fires on Appear */
    mMsg_request_main_appear(msg_p, s_active_speaker, TRUE,
                             &msg_p->request_data.request_main_appear.window_color,
                             s_active_msg_no, 5);
    return 1;
}
