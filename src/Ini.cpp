#include "Ini.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <sys/stat.h>

namespace {

constexpr const char* kPath = "Data/SKSE/Plugins/PPB.ini";

std::mutex g_mx;
std::unordered_map<std::string, std::string> g_kv;   // "section|key" (lowercased) -> raw value
long long g_mtime    = -1;
double    g_lastPoll = -1e9;

double NowS()
{
    // steady, cheap; matches the polling style used across PPB
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

void Lower(char* s) { for (; *s; ++s) if (*s >= 'A' && *s <= 'Z') *s += 32; }

void Trim(char* s)
{
    char* b = s;
    while (*b == ' ' || *b == '\t') ++b;
    std::size_t n = std::strlen(b);
    while (n && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\r' || b[n - 1] == '\n')) --n;
    std::memmove(s, b, n);
    s[n] = '\0';
}

void Reparse()
{
    g_kv.clear();
    FILE* f = nullptr;
    if (fopen_s(&f, kPath, "r") != 0 || !f) return;
    char line[512];
    char section[96] = "";
    while (fgets(line, sizeof line, f)) {
        // strip comments (';' or '#'), then whitespace
        if (char* c = std::strpbrk(line, ";#")) *c = '\0';
        Trim(line);
        if (!line[0]) continue;
        if (line[0] == '[') {
            if (char* e = std::strchr(line, ']')) {
                *e = '\0';
                std::snprintf(section, sizeof section, "%s", line + 1);
                Trim(section);
                Lower(section);
            }
            continue;
        }
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char key[160];
        std::snprintf(key, sizeof key, "%s", line);
        Trim(key);
        Lower(key);
        char val[256];
        std::snprintf(val, sizeof val, "%s", eq + 1);
        Trim(val);
        if (!key[0]) continue;
        g_kv[std::string(section) + "|" + key] = val;
    }
    fclose(f);
}

// Reparse when the file changed; rate-limited to ~1 Hz so hot reads stay cheap.
void PollLocked()
{
    const double now = NowS();
    if (now - g_lastPoll < 1.0) return;
    g_lastPoll = now;
    struct _stat64 st{};
    const long long mt = (_stat64(kPath, &st) == 0) ? (long long)st.st_mtime : -2;
    if (mt == g_mtime) return;
    g_mtime = mt;
    Reparse();
}

const std::string* Find(const char* section, const char* key)
{
    char sec[96], k[160];
    std::snprintf(sec, sizeof sec, "%s", section ? section : "");
    std::snprintf(k, sizeof k, "%s", key ? key : "");
    Lower(sec);
    Lower(k);
    auto it = g_kv.find(std::string(sec) + "|" + k);
    return it == g_kv.end() ? nullptr : &it->second;
}

}   // namespace

namespace Ini {

bool GetBool(const char* section, const char* key, bool def)
{
    std::scoped_lock lk(g_mx);
    PollLocked();
    const std::string* v = Find(section, key);
    if (!v) return def;
    const char* s = v->c_str();
    if (_stricmp(s, "1") == 0 || _stricmp(s, "true") == 0 || _stricmp(s, "on") == 0)  return true;
    if (_stricmp(s, "0") == 0 || _stricmp(s, "false") == 0 || _stricmp(s, "off") == 0) return false;
    return def;
}

float GetFloat(const char* section, const char* key, float def)
{
    std::scoped_lock lk(g_mx);
    PollLocked();
    const std::string* v = Find(section, key);
    if (!v) return def;
    char* end = nullptr;
    const float f = std::strtof(v->c_str(), &end);
    return (end && end != v->c_str()) ? f : def;
}

bool GetString(const char* section, const char* key, const char* def, char* out, std::size_t cap)
{
    std::scoped_lock lk(g_mx);
    PollLocked();
    const std::string* v = Find(section, key);
    std::snprintf(out, cap, "%s", v ? v->c_str() : (def ? def : ""));
    return v != nullptr;
}

// ══ [Features] — the FOMOD feature switches, cached (2026-09-11) ═══════════════════════════
// Read from the per-actor pre-drive hook, so the raw GetBool path (mutex + two std::string
// builds + a hash lookup, EVERY call) must not be on it. One atomic load per query; the real
// read happens at most ~1x/second, on whichever main-thread caller trips the staleness check.
// Defaults TRUE so a missing file or a missing [Features] section = today's behaviour exactly.
namespace {
std::atomic<bool>      g_featPush { true };
std::atomic<bool>      g_featEquip{ true };
std::atomic<bool>      g_featFeet { true };
std::atomic<bool>      g_featWalk { true };   // 2.2.0 push sub-switches
std::atomic<bool>      g_featStumble{ true };
std::atomic<bool>      g_featRagdoll{ true };
std::atomic<long long> g_featStamp{ -1 };     // clock() ticks at the last real read; -1 = never

void RefreshFeaturesIfStale()
{
    const long long now  = (long long)std::clock();
    const long long last = g_featStamp.load(std::memory_order_relaxed);
    if (last >= 0 && (now - last) < (long long)CLOCKS_PER_SEC) return;
    g_featStamp.store(now, std::memory_order_relaxed);
    // GetBool takes g_mx itself — we hold no lock here, so this is not re-entrant.
    g_featPush .store(GetBool("Features", "bPushShove",     true), std::memory_order_relaxed);
    g_featEquip.store(GetBool("Features", "bEquipGestures", true), std::memory_order_relaxed);
    g_featFeet .store(GetBool("Features", "bFeetLift",      true), std::memory_order_relaxed);
    g_featWalk   .store(GetBool("Features", "bPushWalk",     true), std::memory_order_relaxed);
    g_featStumble.store(GetBool("Features", "bPushStumble",  true), std::memory_order_relaxed);
    g_featRagdoll.store(GetBool("Features", "bShoveRagdoll", true), std::memory_order_relaxed);
}
}   // namespace

bool FeaturePushShove()     { RefreshFeaturesIfStale(); return g_featPush .load(std::memory_order_relaxed); }
bool FeatureEquipGestures() { RefreshFeaturesIfStale(); return g_featEquip.load(std::memory_order_relaxed); }
bool FeatureFeetLift()      { RefreshFeaturesIfStale(); return g_featFeet .load(std::memory_order_relaxed); }
bool FeaturePushWalk()      { RefreshFeaturesIfStale(); return g_featWalk   .load(std::memory_order_relaxed); }
bool FeaturePushStumble()   { RefreshFeaturesIfStale(); return g_featStumble.load(std::memory_order_relaxed); }
bool FeatureShoveRagdoll()  { RefreshFeaturesIfStale(); return g_featRagdoll.load(std::memory_order_relaxed); }

}   // namespace Ini
