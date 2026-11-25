#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <functional>
#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include <unordered_set>

namespace fs = std::filesystem;

// ============================================================================
//  Constantes
// ============================================================================

#define VIBRA_VERSION "1.0"

// Racine GitHub Pages : <base>/release.txt et <base>/releases/<version>/<arch>/vibra
static const char* UPDATE_BASE_URL = "https://inaddict1.github.io/VibraEditor";

// Séquences ANSI
#define ENTER_ALT_SCREEN "\033[?1049h"
#define EXIT_ALT_SCREEN  "\033[?1049l"
#define CLEAR_SCREEN     "\033[2J\033[H"
#define HIDE_CURSOR      "\033[?25l"
#define SHOW_CURSOR      "\033[?25h"
#define RESET_FORMAT     "\033[0m"

// Contrôle de l'auto-wrap (DECAWM)
#define DISABLE_AUTOWRAP "\033[?7l"
#define ENABLE_AUTOWRAP  "\033[?7h"

// Capture souris, tactile (SGR 1006) et Bracketed Paste
#define ENABLE_MOUSE     "\033[?1000h\033[?1002h\033[?1006h\033[?2004h"
#define DISABLE_MOUSE    "\033[?2004l\033[?1006l\033[?1002l\033[?1000l"

enum SyntaxMode {
    SYNTAX_NONE,
    SYNTAX_CPP,
    SYNTAX_PYTHON,
    SYNTAX_JS,
    SYNTAX_SHELL
};

// Styles de coloration (un style par octet de la ligne)
enum Style : uint8_t {
    S_NORMAL = 0,
    S_COMMENT,
    S_STRING,
    S_KEYWORD,
    S_FUNC,
    S_NUMBER,
    S_PREPROC,
    S_DECL,     // const / let / var
    S_BUILTIN,  // globals Node.js : require, module, process...
    S_VAR,      // $VAR (shell)
    S_COUNT
};

// Drapeaux de superposition (recherche / sélection)
enum { FL_SEARCH = 1, FL_SEL = 2 };

// État de coloration porté d'une ligne à la suivante
enum { LS_NONE = 0, LS_BLOCK = 1, LS_TEMPLATE = 2 };

struct LangDef {
    std::unordered_set<std::string> keywords;
    std::unordered_set<std::string> decls;
    std::unordered_set<std::string> builtins;
    std::string line_comment;
    std::string quotes = "\"'";
    bool block_comment = false;
    bool template_lit = false;
    bool preproc = false;
    bool shell_vars = false;
    bool dollar_ident = false;
    bool multiline() const { return block_comment || template_lit; }
};

// Structure représentant l'état du texte pour Undo/Redo
struct UndoSnapshot {
    std::vector<std::string> lines;
    int cursor_x;
    int cursor_y;
};

struct EditorState {
    std::vector<std::string> lines = {""};
    int cursor_x = 0; // Index d'octet dans la ligne
    int cursor_y = 0;
    int scroll_y = 0;
    int scroll_x = 0; // Défilement horizontal (en nombre de caractères)
    int rows = 24;
    int cols = 80;
    std::string filename;
    bool modified = false;
    bool is_termux = false;
    bool in_paste = false; // Indique si un collage de texte est en cours

    // Config / Flags
    bool create_dir = false;
    bool readonly = false;
    bool show_line_numbers = true;
    bool autocomplete = true;   // -a / --no-autocomplete pour désactiver
    bool update_check = true;   // --no-update-check pour désactiver
    SyntaxMode syntax_mode = SYNTAX_NONE;
    const LangDef* lang = nullptr;
    bool is_python = false;

    // Recherche
    std::string search_query = "";
    bool in_search = false;

    // Sélection (toujours normalisée : début <= fin)
    bool has_selection = false;
    int sel_sy = 0, sel_sx = 0;
    int sel_ey = 0, sel_ex = 0;

    // Autocomplétion
    std::vector<std::string> completions;
    std::string completion_prefix;
    int completion_idx = 0;
    bool completion_wanted = false; // vrai juste après une frappe de mot

    // Message temporaire dans la barre de statut
    std::string status_msg;
    std::chrono::steady_clock::time_point status_until;

    // Mise à jour
    bool update_available = false;
    std::string update_version;

    // Historique Undo / Redo
    std::vector<UndoSnapshot> undo_stack;
    std::vector<UndoSnapshot> redo_stack;
    const size_t max_undo_levels = 100;

    // Gestion du regroupement de frappe (Typing grouping)
    std::chrono::steady_clock::time_point last_edit_time;
    bool is_typing = false;
};

EditorState g_editor;
termios g_orig_termios;
const char* g_argv0 = "vibra";

void raw_write(const std::string& str) {
    ssize_t r = write(STDOUT_FILENO, str.c_str(), str.length());
    (void)r;
}

void set_status(const std::string& msg, int ms = 4000) {
    g_editor.status_msg = msg;
    g_editor.status_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

// ============================================================================
//  Processus externes (sans passer par un shell : pas d'injection possible)
// ============================================================================

static void set_cloexec(int fd) {
    int f = fcntl(fd, F_GETFD);
    if (f >= 0) fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

// Lance un programme. in_fd / out_fd (optionnels) reçoivent un tube vers
// son stdin / depuis son stdout. stderr part toujours vers /dev/null.
pid_t spawn_process(const std::vector<std::string>& args, int* in_fd, int* out_fd) {
    if (args.empty()) return -1;
    int pin[2] = {-1, -1}, pout[2] = {-1, -1};

    if (in_fd && pipe(pin) != 0) return -1;
    if (out_fd && pipe(pout) != 0) {
        if (in_fd) { close(pin[0]); close(pin[1]); }
        return -1;
    }
    for (int fd : {pin[0], pin[1], pout[0], pout[1]}) {
        if (fd >= 0) set_cloexec(fd);
    }

    pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {pin[0], pin[1], pout[0], pout[1]}) {
            if (fd >= 0) close(fd);
        }
        return -1;
    }

    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(in_fd ? pin[0] : nul, STDIN_FILENO);
        dup2(out_fd ? pout[1] : nul, STDOUT_FILENO);
        dup2(nul, STDERR_FILENO);
        if (nul > 2) close(nul);

        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    if (in_fd) { close(pin[0]); *in_fd = pin[1]; }
    if (out_fd) { close(pout[1]); *out_fd = pout[0]; }
    return pid;
}

// Exécute et capture stdout (avec délai maximum). Vrai si code de sortie 0.
bool run_capture(const std::vector<std::string>& args, std::string& out, int timeout_ms) {
    using namespace std::chrono;
    int fd = -1;
    pid_t pid = spawn_process(args, nullptr, &fd);
    if (pid < 0) return false;

    auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    bool timed_out = false;
    char buf[4096];

    while (true) {
        long left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
        if (left <= 0) { timed_out = true; break; }
        pollfd p{fd, POLLIN, 0};
        int r = poll(&p, 1, static_cast<int>(left));
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) { timed_out = true; break; }
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (out.size() < (1u << 20)) out.append(buf, static_cast<size_t>(n));
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }
    close(fd);
    if (timed_out) kill(pid, SIGKILL);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return !timed_out && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// Exécute en lui envoyant `input` sur stdin. Vrai si code de sortie 0.
bool run_with_input(const std::vector<std::string>& args, const std::string& input, int timeout_ms) {
    using namespace std::chrono;
    int fd = -1;
    pid_t pid = spawn_process(args, &fd, nullptr);
    if (pid < 0) return false;

    auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    bool failed = false;
    size_t off = 0;

    while (off < input.size()) {
        long left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
        if (left <= 0) { failed = true; break; }
        pollfd p{fd, POLLOUT, 0};
        int r = poll(&p, 1, static_cast<int>(left));
        if (r < 0) { if (errno == EINTR) continue; failed = true; break; }
        if (r == 0) { failed = true; break; }
        ssize_t n = write(fd, input.data() + off, input.size() - off);
        if (n > 0) off += static_cast<size_t>(n);
        else if (n < 0 && errno != EINTR) { failed = true; break; }
    }
    close(fd);

    int st = 0;
    while (true) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) { failed = true; break; }
        if (steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
            return false;
        }
        usleep(5000);
    }
    return !failed && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

bool command_exists(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        std::string dir = p.substr(start, end - start);
        if (dir.empty()) dir = ".";
        std::string full = dir + "/" + name;
        struct stat sb;
        if (stat(full.c_str(), &sb) == 0 && S_ISREG(sb.st_mode) && access(full.c_str(), X_OK) == 0) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

// --- processor architecture detector ---
std::string getLinuxArch() {
    struct utsname buffer;
    if (uname(&buffer) == 0) {
        return buffer.machine; // Renvoie "x86_64", "aarch64", etc.
    }
    return "Unknown";
}

// ============================================================================
//  Base64 (OSC 52)
// ============================================================================

std::string base64_encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ============================================================================
//  GESTION DE L'HISTORIQUE (UNDO / REDO)
// ============================================================================

void clear_selection() {
    g_editor.has_selection = false;
}

void save_snapshot() {
    UndoSnapshot snap;
    snap.lines = g_editor.lines;
    snap.cursor_x = g_editor.cursor_x;
    snap.cursor_y = g_editor.cursor_y;

    g_editor.undo_stack.push_back(snap);
    if (g_editor.undo_stack.size() > g_editor.max_undo_levels) {
        g_editor.undo_stack.erase(g_editor.undo_stack.begin());
    }
    g_editor.redo_stack.clear();
}

void finalize_typing_session() {
    g_editor.is_typing = false;
}

void prepare_typing_snapshot() {
    if (g_editor.in_paste) return;

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_editor.last_edit_time).count();

    if (!g_editor.is_typing || elapsed > 500) {
        save_snapshot();
        g_editor.is_typing = true;
    }
    g_editor.last_edit_time = now;
}

void undo() {
    finalize_typing_session();
    clear_selection();
    if (g_editor.undo_stack.empty()) return;

    UndoSnapshot current;
    current.lines = g_editor.lines;
    current.cursor_x = g_editor.cursor_x;
    current.cursor_y = g_editor.cursor_y;
    g_editor.redo_stack.push_back(current);

    UndoSnapshot previous = g_editor.undo_stack.back();
    g_editor.undo_stack.pop_back();

    g_editor.lines = previous.lines;
    g_editor.cursor_x = previous.cursor_x;
    g_editor.cursor_y = previous.cursor_y;
    g_editor.modified = true;
}

void redo() {
    finalize_typing_session();
    clear_selection();
    if (g_editor.redo_stack.empty()) return;

    UndoSnapshot current;
    current.lines = g_editor.lines;
    current.cursor_x = g_editor.cursor_x;
    current.cursor_y = g_editor.cursor_y;
    g_editor.undo_stack.push_back(current);

    UndoSnapshot next = g_editor.redo_stack.back();
    g_editor.redo_stack.pop_back();

    g_editor.lines = next.lines;
    g_editor.cursor_x = next.cursor_x;
    g_editor.cursor_y = next.cursor_y;
    g_editor.modified = true;
}

// --- DÉTECTION D'ENTRÉE PENDING ---

bool has_pending_input() {
    int bytes = 0;
    if (ioctl(STDIN_FILENO, FIONREAD, &bytes) == 0) {
        return bytes > 0;
    }
    return false;
}

// ============================================================================
//  FONCTIONS UTF-8
// ============================================================================

size_t utf8_length(const std::string& s) {
    size_t count = 0;
    for (size_t i = 0; i < s.length(); ) {
        unsigned char c = s[i];
        if (c < 0x80) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        count++;
    }
    return count;
}

std::string read_utf8_sequence(char first_byte) {
    std::string seq;
    seq += first_byte;
    unsigned char c = static_cast<unsigned char>(first_byte);
    int extra = 0;
    if ((c & 0xE0) == 0xC0) extra = 1;
    else if ((c & 0xF0) == 0xE0) extra = 2;
    else if ((c & 0xF8) == 0xF0) extra = 3;

    for (int i = 0; i < extra; ++i) {
        char next = 0;
        if (read(STDIN_FILENO, &next, 1) > 0) {
            seq += next;
        } else {
            break;
        }
    }
    return seq;
}

size_t utf8_prev_index(const std::string& s, size_t index) {
    if (index == 0) return 0;
    size_t i = index - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        i--;
    }
    return i;
}

size_t utf8_next_index(const std::string& s, size_t index) {
    if (index >= s.length()) return s.length();
    unsigned char c = static_cast<unsigned char>(s[index]);
    if (c < 0x80) return index + 1;
    if ((c & 0xE0) == 0xC0) return std::min(s.length(), index + 2);
    if ((c & 0xF0) == 0xE0) return std::min(s.length(), index + 3);
    if ((c & 0xF8) == 0xF0) return std::min(s.length(), index + 4);
    return index + 1;
}

std::string utf8_substr(const std::string& s, size_t start_char, size_t char_count) {
    size_t start_byte = 0;
    for (size_t i = 0; i < start_char && start_byte < s.length(); ++i) {
        start_byte = utf8_next_index(s, start_byte);
    }
    size_t end_byte = start_byte;
    for (size_t i = 0; i < char_count && end_byte < s.length(); ++i) {
        end_byte = utf8_next_index(s, end_byte);
    }
    return s.substr(start_byte, end_byte - start_byte);
}

// ============================================================================
//  AUTO-INDENTATION PAR LANGAGE
// ============================================================================

std::string compute_auto_indent(const std::string& line, int cursor_x) {
    std::string indent = "";

    for (int i = 0; i < cursor_x && i < static_cast<int>(line.length()); ++i) {
        if (line[i] == ' ' || line[i] == '\t') {
            indent += line[i];
        } else {
            break;
        }
    }

    if (cursor_x > 0) {
        int idx = cursor_x - 1;
        while (idx >= 0 && (line[idx] == ' ' || line[idx] == '\t')) {
            idx--;
        }
        if (idx >= 0) {
            char last = line[idx];
            bool should_indent = (last == '{' || last == '(' || last == '[');

            if (g_editor.is_python && last == ':') {
                should_indent = true;
            }

            if (should_indent) {
                indent += "    ";
            }
        }
    }

    return indent;
}

// ============================================================================
//  Aide / version
// ============================================================================

void print_version() {
    std::cout << "vibraEditor (vibra) v" VIBRA_VERSION " - " << getLinuxArch() << " / Linux\n";
    std::cout << "Copyright (c) 2026 - Tous droits réservés.\n";
}

void print_help() {
    print_version();
    std::cout << "\nUsage : vibra [OPTIONS] [FICHIER]\n\n"
              << "Options :\n"
              << "  -d, --create-dir       Crée automatiquement le dossier parent si inexistant.\n"
              << "  -r, --readonly         Ouvre le fichier en mode lecture seule.\n"
              << "  -n, --no-numbers       Masque la numérotation des lignes.\n"
              << "  -a, --no-autocomplete  Désactive l'autocomplétion.\n"
              << "      --no-update-check  Ne vérifie pas les mises à jour au démarrage.\n"
              << "  -u, --update           Installe la dernière version puis quitte.\n"
              << "  -h, --help             Affiche cette aide et quitte.\n"
              << "  -v, --version          Affiche la version et quitte.\n\n"
              << "Raccourcis dans l'éditeur :\n"
              << "  Ctrl+S              Sauvegarder le fichier\n"
              << "  Ctrl+Z              Annuler l'action (Undo)\n"
              << "  Ctrl+Y              Refaire l'action (Redo)\n"
              << "  Ctrl+A              Tout sélectionner\n"
              << "  Ctrl+C              Copier la sélection (puis désélectionner)\n"
              << "  Ctrl+F              Rechercher du texte (modal)\n"
              << "  Ctrl+R              Chercher et remplacer dans tout le fichier\n"
              << "  Ctrl+K              Supprimer la ligne courante\n"
              << "  Ctrl+U              Installer la mise à jour disponible\n"
              << "  Ctrl+Q              Quitter l'éditeur (avec confirmation si modifié)\n"
              << "  Tab                 Accepter la suggestion (sinon insère 4 espaces)\n"
              << "  Haut / Bas          Naviguer dans les suggestions quand elles sont affichées\n"
              << "  Home / End          Début / Fin de la ligne\n"
              << "  Ctrl+Home/Ctrl+End  Début / Fin du texte\n"
              << "  Flèches / Clic      Déplacer le curseur / Naviguer dans les résultats\n"
              << "  Échap               Fermer le mode recherche / sélection / suggestions\n"
              << "  Molette / Swipe     Faire défiler le texte (Scroll)\n";
}

bool detect_termux() {
    const char* prefix = std::getenv("PREFIX");
    const char* home = std::getenv("HOME");

    if (prefix && std::string(prefix).find("/data/data/com.termux") != std::string::npos) return true;
    if (home && std::string(home).find("/data/data/com.termux") != std::string::npos) return true;

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        if (std::string(cwd).find("/data/data/com.termux") != std::string::npos) return true;
    }

    return false;
}

// ============================================================================
//  Définition des langages
// ============================================================================

static LangDef make_cpp() {
    LangDef d;
    d.keywords = {
        "int", "char", "bool", "void", "float", "double", "auto", "const",
        "struct", "class", "namespace", "using", "return", "if", "else",
        "while", "for", "do", "switch", "case", "break", "continue",
        "true", "false", "nullptr", "public", "private", "protected",
        "template", "typename", "virtual", "override", "explicit", "static",
        "inline", "constexpr", "size_t", "std", "vector", "string", "include",
        "long", "short", "unsigned", "signed", "enum", "union", "typedef",
        "try", "catch", "throw", "noexcept", "this", "new", "delete", "default",
        "goto", "extern", "volatile", "mutable", "friend", "operator", "sizeof",
        "decltype", "final", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "int8_t", "int16_t", "int32_t", "int64_t"
    };
    d.line_comment = "//";
    d.block_comment = true;
    d.preproc = true;
    return d;
}

static LangDef make_python() {
    LangDef d;
    d.keywords = {
        "def", "class", "import", "from", "return", "if", "elif", "else",
        "while", "for", "in", "is", "not", "and", "or", "True", "False",
        "None", "try", "except", "finally", "raise", "with", "as", "pass",
        "break", "continue", "lambda", "async", "await", "yield", "self", "global",
        "nonlocal", "del", "assert"
    };
    d.line_comment = "#";
    return d;
}

// JavaScript / TypeScript, avec les globals de Node.js (CommonJS : require, module, exports...)
static LangDef make_js() {
    LangDef d;
    d.keywords = {
        "function", "return", "if", "else", "for", "while", "do", "switch",
        "case", "break", "continue", "import", "export", "from", "default",
        "class", "extends", "new", "this", "async", "await", "try", "catch",
        "finally", "throw", "typeof", "instanceof", "true", "false", "null",
        "undefined", "of", "in", "delete", "void", "yield", "static", "super",
        "with", "debugger"
    };
    d.decls = {"const", "let", "var"};
    d.builtins = {
        "require", "module", "exports", "__dirname", "__filename",
        "process", "console", "Buffer", "global", "globalThis",
        "setTimeout", "setInterval", "setImmediate",
        "clearTimeout", "clearInterval", "clearImmediate",
        "queueMicrotask", "structuredClone", "fetch",
        "Promise", "JSON", "Math", "Object", "Array", "String", "Number",
        "Boolean", "Symbol", "BigInt", "Map", "Set", "WeakMap", "WeakSet",
        "Date", "RegExp", "Error", "TypeError", "RangeError", "SyntaxError",
        "URL", "URLSearchParams", "TextEncoder", "TextDecoder", "AbortController",
        "parseInt", "parseFloat", "isNaN", "NaN", "Infinity"
    };
    d.line_comment = "//";
    d.quotes = "\"'";
    d.block_comment = true;
    d.template_lit = true;
    d.dollar_ident = true;
    return d;
}

static LangDef make_shell() {
    LangDef d;
    d.keywords = {
        "if", "then", "elif", "else", "fi", "for", "in", "do", "done",
        "while", "until", "case", "esac", "function", "return", "exit",
        "local", "export", "echo", "cd", "pwd", "set", "unset"
    };
    d.line_comment = "#";
    d.shell_vars = true;
    return d;
}

const LangDef* get_lang(SyntaxMode m) {
    static const LangDef cpp = make_cpp();
    static const LangDef py = make_python();
    static const LangDef js = make_js();
    static const LangDef sh = make_shell();
    switch (m) {
        case SYNTAX_CPP:    return &cpp;
        case SYNTAX_PYTHON: return &py;
        case SYNTAX_JS:     return &js;
        case SYNTAX_SHELL:  return &sh;
        default:            return nullptr;
    }
}

void apply_syntax(SyntaxMode m) {
    g_editor.syntax_mode = m;
    g_editor.lang = get_lang(m);
    g_editor.is_python = (m == SYNTAX_PYTHON);
}

void check_syntax_mode() {
    std::string ext = fs::path(g_editor.filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".cpp" || ext == ".hpp" || ext == ".c" || ext == ".h" || ext == ".cc" || ext == ".cxx") {
        apply_syntax(SYNTAX_CPP);
    } else if (ext == ".py" || ext == ".pyw") {
        apply_syntax(SYNTAX_PYTHON);
    } else if (ext == ".js" || ext == ".cjs" || ext == ".mjs" || ext == ".ts" || ext == ".jsx" || ext == ".tsx" || ext == ".json") {
        apply_syntax(SYNTAX_JS);
    } else if (ext == ".sh" || ext == ".bash" || ext == ".zsh") {
        apply_syntax(SYNTAX_SHELL);
    } else {
        apply_syntax(SYNTAX_NONE);
    }
}

// Fichier sans extension connue : on regarde le shebang (#!/usr/bin/env node ...)
void detect_syntax_from_shebang() {
    if (g_editor.syntax_mode != SYNTAX_NONE || g_editor.lines.empty()) return;
    const std::string& l = g_editor.lines[0];
    if (l.compare(0, 2, "#!") != 0) return;
    if (l.find("node") != std::string::npos) apply_syntax(SYNTAX_JS);
    else if (l.find("python") != std::string::npos) apply_syntax(SYNTAX_PYTHON);
    else if (l.find("sh") != std::string::npos) apply_syntax(SYNTAX_SHELL);
}

// ============================================================================
//  COLORATION SYNTAXIQUE (un style par octet, pas de séquences ANSI ici)
// ============================================================================

static inline bool ch_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
static inline bool ch_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
static inline bool ch_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
static inline bool ch_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

// Parcourt une chaîne à partir de `from` (juste après le guillemet ouvrant).
// Retourne l'index suivant le guillemet fermant, ou la fin de ligne.
static size_t scan_quoted(const std::string& line, size_t from, char quote, bool& closed) {
    closed = false;
    size_t n = line.size();
    size_t j = from;
    while (j < n) {
        if (line[j] == '\\') { j += 2; continue; }
        if (line[j] == quote) { closed = true; return j + 1; }
        j++;
    }
    return n;
}

void highlight_line(const std::string& line, int& state, std::vector<uint8_t>& st, int idx) {
    const size_t n = line.size();
    st.assign(n, S_NORMAL);
    const LangDef* L = g_editor.lang;
    if (!L || n == 0) return;

    auto paint = [&](size_t a, size_t b, uint8_t s) {
        for (size_t k = a; k < b && k < n; ++k) st[k] = s;
    };

    // Shebang
    if (idx == 0 && n >= 2 && line[0] == '#' && line[1] == '!') {
        paint(0, n, S_COMMENT);
        return;
    }

    size_t i = 0;

    // Reprise d'un état multi-lignes
    if (state == LS_BLOCK) {
        size_t e = line.find("*/");
        if (e == std::string::npos) { paint(0, n, S_COMMENT); return; }
        paint(0, e + 2, S_COMMENT);
        i = e + 2;
        state = LS_NONE;
    } else if (state == LS_TEMPLATE) {
        bool closed = false;
        size_t e = scan_quoted(line, 0, '`', closed);
        paint(0, e, S_STRING);
        i = e;
        if (!closed) return;
        state = LS_NONE;
    }

    size_t first_ns = line.find_first_not_of(" \t");
    bool is_preproc = L->preproc && first_ns != std::string::npos && line[first_ns] == '#';
    bool is_include = false;
    const size_t lc_len = L->line_comment.size();

    while (i < n) {
        char c = line[i];

        // Directive de préprocesseur (C/C++)
        if (is_preproc && i == first_ns) {
            size_t j = i + 1;
            while (j < n && ch_space(line[j])) j++;
            size_t name_start = j;
            while (j < n && ch_alpha(line[j])) j++;
            std::string d = line.substr(name_start, j - name_start);
            paint(i, j, S_PREPROC);
            is_include = (d == "include" || d == "import");
            i = j;
            continue;
        }

        if (is_include && c == '<') {
            size_t close_pos = line.find('>', i);
            if (close_pos != std::string::npos) {
                paint(i, close_pos + 1, S_STRING);
                i = close_pos + 1;
                continue;
            }
        }

        // Commentaire de ligne
        if (lc_len > 0 && line.compare(i, lc_len, L->line_comment) == 0) {
            paint(i, n, S_COMMENT);
            return;
        }

        // Commentaire de bloc
        if (L->block_comment && c == '/' && i + 1 < n && line[i + 1] == '*') {
            size_t e = line.find("*/", i + 2);
            if (e == std::string::npos) {
                paint(i, n, S_COMMENT);
                state = LS_BLOCK;
                return;
            }
            paint(i, e + 2, S_COMMENT);
            i = e + 2;
            continue;
        }

        // Template literal JS (peut s'étendre sur plusieurs lignes)
        if (L->template_lit && c == '`') {
            bool closed = false;
            size_t e = scan_quoted(line, i + 1, '`', closed);
            paint(i, e, S_STRING);
            i = e;
            if (!closed) { state = LS_TEMPLATE; return; }
            continue;
        }

        // Chaînes
        if (c != 0 && L->quotes.find(c) != std::string::npos) {
            bool closed = false;
            size_t e = scan_quoted(line, i + 1, c, closed);
            paint(i, e, S_STRING);
            i = e;
            continue;
        }

        // Variables shell
        if (L->shell_vars && c == '$') {
            size_t j = i + 1;
            if (j < n && line[j] == '{') {
                size_t e = line.find('}', j);
                j = (e == std::string::npos) ? n : e + 1;
            } else if (j < n && (ch_alnum(line[j]) || line[j] == '_')) {
                while (j < n && (ch_alnum(line[j]) || line[j] == '_')) j++;
            } else if (j < n && std::strchr("#?!@*$-", line[j])) {
                j++;
            }
            paint(i, j, S_VAR);
            i = j;
            continue;
        }

        // Identifiants / mots-clés
        if (ch_alpha(c) || c == '_' || (L->dollar_ident && c == '$')) {
            size_t j = i;
            while (j < n && (ch_alnum(line[j]) || line[j] == '_' || (L->dollar_ident && line[j] == '$'))) j++;
            std::string word = line.substr(i, j - i);
            bool member = (i > 0 && line[i - 1] == '.');

            uint8_t s = S_NORMAL;
            if (!member) {
                if (L->decls.count(word)) s = S_DECL;
                else if (L->keywords.count(word)) s = S_KEYWORD;
                else if (L->builtins.count(word)) s = S_BUILTIN;
            }
            if (s == S_NORMAL) {
                size_t p = j;
                while (p < n && ch_space(line[p])) p++;
                if (p < n && line[p] == '(') s = S_FUNC;
            }
            paint(i, j, s);
            i = j;
            continue;
        }

        // Nombres (0x1F, 1_000, 3.14...)
        if (ch_digit(c)) {
            size_t j = i;
            while (j < n && (ch_alnum(line[j]) || line[j] == '_' ||
                             (line[j] == '.' && j + 1 < n && ch_digit(line[j + 1])))) j++;
            paint(i, j, S_NUMBER);
            i = j;
            continue;
        }

        i++;
    }
}

// ============================================================================
//  SÉLECTION
// ============================================================================

void select_all() {
    auto& e = g_editor;
    e.sel_sy = 0;
    e.sel_sx = 0;
    e.sel_ey = static_cast<int>(e.lines.size()) - 1;
    e.sel_ex = static_cast<int>(e.lines[e.sel_ey].length());
    e.has_selection = true;
    e.cursor_y = e.sel_ey;
    e.cursor_x = e.sel_ex;
}

bool selection_range(int& y0, int& x0, int& y1, int& x1) {
    auto& e = g_editor;
    if (!e.has_selection) return false;
    int last = static_cast<int>(e.lines.size()) - 1;
    y0 = std::clamp(e.sel_sy, 0, last);
    y1 = std::clamp(e.sel_ey, 0, last);
    x0 = std::clamp(e.sel_sx, 0, static_cast<int>(e.lines[y0].size()));
    x1 = std::clamp(e.sel_ex, 0, static_cast<int>(e.lines[y1].size()));
    return true;
}

std::string get_selection_text() {
    int y0, x0, y1, x1;
    if (!selection_range(y0, x0, y1, x1)) return "";
    auto& L = g_editor.lines;
    if (y0 == y1) return L[y0].substr(x0, x1 - x0);

    std::string text = L[y0].substr(x0) + "\n";
    for (int y = y0 + 1; y < y1; ++y) text += L[y] + "\n";
    text += L[y1].substr(0, x1);
    return text;
}

void delete_selection() {
    int y0, x0, y1, x1;
    if (!selection_range(y0, x0, y1, x1)) return;
    auto& e = g_editor;

    if (y0 == y1) {
        e.lines[y0].erase(x0, x1 - x0);
    } else {
        e.lines[y0] = e.lines[y0].substr(0, x0) + e.lines[y1].substr(x1);
        e.lines.erase(e.lines.begin() + y0 + 1, e.lines.begin() + y1 + 1);
    }
    e.cursor_y = y0;
    e.cursor_x = x0;
    e.has_selection = false;
    e.modified = true;
}

// À appeler avant toute modification : gère l'undo et remplace la sélection.
void begin_edit() {
    auto& e = g_editor;
    if (e.has_selection) {
        if (!e.in_paste) {
            finalize_typing_session();
            save_snapshot();
        }
        delete_selection();
        e.is_typing = true;
        e.last_edit_time = std::chrono::steady_clock::now();
    } else {
        prepare_typing_snapshot();
    }
}

// ============================================================================
//  PRESSE-PAPIERS
//    Termux      : termux-clipboard-set (paquet termux-api + appli Termux:API)
//    Linux       : wl-copy (Wayland), xclip / xsel (X11)
//    Repli       : OSC 52 (fonctionne dans la plupart des terminaux, même en SSH)
// ============================================================================

std::string copy_to_clipboard(const std::string& text) {
    auto& e = g_editor;
    std::string note;

    if (e.is_termux) {
        if (command_exists("termux-clipboard-set")) {
            if (run_with_input({"termux-clipboard-set"}, text, 5000)) return "termux-clipboard-set";
            note = "termux-clipboard-set a échoué (appli Termux:API installée ?), repli OSC 52";
        } else {
            note = "termux-api absent (pkg install termux-api), repli OSC 52";
        }
    } else {
        std::vector<std::string> cmd;
        if (std::getenv("WAYLAND_DISPLAY") && command_exists("wl-copy")) {
            cmd = {"wl-copy"};
        } else if (std::getenv("DISPLAY") && command_exists("xclip")) {
            cmd = {"xclip", "-selection", "clipboard"};
        } else if (std::getenv("DISPLAY") && command_exists("xsel")) {
            cmd = {"xsel", "--clipboard", "--input"};
        }
        if (!cmd.empty()) {
            if (run_with_input(cmd, text, 3000)) return cmd[0];
            note = cmd[0] + " a échoué, repli OSC 52";
        } else {
            note = "OSC 52 (selon le terminal)";
        }
    }

    raw_write("\033]52;c;" + base64_encode(text) + "\a");
    return note;
}

void copy_selection() {
    auto& e = g_editor;
    if (!e.has_selection) {
        set_status("Rien à copier (Ctrl+A pour tout sélectionner)");
        return;
    }
    std::string text = get_selection_text();
    std::string how = copy_to_clipboard(text);
    size_t nlines = static_cast<size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    e.has_selection = false;
    set_status("Copié : " + std::to_string(nlines) + (nlines > 1 ? " lignes" : " ligne") + " via " + how, 5000);
}

// ============================================================================
//  AUTOCOMPLÉTION
// ============================================================================

static inline bool is_word_char(char c) {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || c == '_' || c == '$' || u >= 0x80;
}

// Calcule les suggestions pour le mot situé juste avant le curseur
// (mots-clés du langage d'abord, puis mots présents dans le fichier).
void compute_completions(std::vector<std::string>& out, std::string& prefix) {
    auto& e = g_editor;
    out.clear();
    prefix.clear();
    if (e.cursor_y < 0 || e.cursor_y >= static_cast<int>(e.lines.size())) return;

    const std::string& line = e.lines[e.cursor_y];
    size_t end = std::min<size_t>(static_cast<size_t>(e.cursor_x), line.size());
    size_t start = end;
    while (start > 0 && is_word_char(line[start - 1])) start--;

    if (end - start < 2) return;
    if (end < line.size() && is_word_char(line[end])) return; // curseur au milieu d'un mot
    if (ch_digit(line[start])) return;

    std::string pre = line.substr(start, end - start);

    std::set<std::string> lang_words, buf_words;
    if (e.lang) {
        for (const auto* set : {&e.lang->keywords, &e.lang->decls, &e.lang->builtins}) {
            for (const auto& w : *set) {
                if (w.size() > pre.size() && w.compare(0, pre.size(), pre) == 0) lang_words.insert(w);
            }
        }
    }

    for (size_t y = 0; y < e.lines.size(); ++y) {
        const std::string& l = e.lines[y];
        size_t i = 0;
        while (i < l.size()) {
            if (!is_word_char(l[i])) { i++; continue; }
            size_t j = i;
            while (j < l.size() && is_word_char(l[j])) j++;
            bool is_current = (static_cast<int>(y) == e.cursor_y && i == start);
            if (!is_current && j - i > pre.size() && !ch_digit(l[i]) && l.compare(i, pre.size(), pre) == 0) {
                buf_words.insert(l.substr(i, j - i));
            }
            i = j;
        }
    }

    const size_t max_items = 6;
    for (const auto& w : lang_words) {
        if (out.size() >= max_items) break;
        out.push_back(w);
    }
    for (const auto& w : buf_words) {
        if (out.size() >= max_items) break;
        if (!lang_words.count(w)) out.push_back(w);
    }
    if (!out.empty()) prefix = pre;
}

void update_completions() {
    auto& e = g_editor;
    if (!e.autocomplete || e.readonly || !e.completion_wanted || e.has_selection || e.in_paste) {
        e.completions.clear();
        e.completion_prefix.clear();
        return;
    }
    std::vector<std::string> c;
    std::string p;
    compute_completions(c, p);
    if (c.empty()) {
        e.completions.clear();
        e.completion_prefix.clear();
        return;
    }
    if (p != e.completion_prefix) e.completion_idx = 0;
    if (e.completion_idx >= static_cast<int>(c.size())) e.completion_idx = 0;
    e.completions = c;
    e.completion_prefix = p;
}

void close_completion() {
    g_editor.completions.clear();
    g_editor.completion_prefix.clear();
    g_editor.completion_wanted = false;
}

bool accept_completion() {
    auto& e = g_editor;
    std::vector<std::string> cands;
    std::string prefix;
    compute_completions(cands, prefix);
    if (cands.empty()) return false;

    int idx = std::min(e.completion_idx, static_cast<int>(cands.size()) - 1);
    std::string suffix = cands[idx].substr(prefix.size());

    finalize_typing_session();
    save_snapshot();
    e.lines[e.cursor_y].insert(e.cursor_x, suffix);
    e.cursor_x += static_cast<int>(suffix.size());
    e.modified = true;
    close_completion();
    return true;
}

// ============================================================================
//  MISE À JOUR (GitHub Pages)
//    <base>/release.txt                          -> dernière version (1re ligne : ex. 2.4)
//    <base>/releases/<version>/<arch>/vibra      -> binaire précompilé
// ============================================================================

static bool valid_version(const std::string& v) {
    if (v.empty() || v.size() > 32) return false;
    bool last_dot = true; // interdit un point en début
    for (char c : v) {
        if (c == '.') {
            if (last_dot) return false;
            last_dot = true;
        } else if (ch_digit(c)) {
            last_dot = false;
        } else {
            return false;
        }
    }
    return !last_dot;
}

static std::vector<int> split_version(const std::string& v) {
    std::vector<int> parts;
    size_t i = 0;
    while (i <= v.size()) {
        size_t j = v.find('.', i);
        if (j == std::string::npos) j = v.size();
        parts.push_back(std::atoi(v.substr(i, j - i).c_str()));
        i = j + 1;
    }
    return parts;
}

int version_compare(const std::string& a, const std::string& b) {
    std::vector<int> pa = split_version(a), pb = split_version(b);
    size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0);
    pb.resize(n, 0);
    for (size_t i = 0; i < n; ++i) {
        if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

// Extrait la version de release.txt : première ligne utile (ignore vides et # commentaires).
bool parse_release_txt(const std::string& text, std::string& version) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string l = text.substr(pos, nl - pos);
        pos = nl + 1;

        size_t a = l.find_first_not_of(" \t\r");
        if (a == std::string::npos) continue;
        size_t b = l.find_last_not_of(" \t\r");
        l = l.substr(a, b - a + 1);
        if (l[0] == '#') continue;
        if (l[0] == 'v' || l[0] == 'V') l.erase(0, 1);
        if (!valid_version(l)) return false;
        version = l;
        return true;
    }
    return false;
}

// Commande de téléchargement (curl, sinon wget). Vide si aucun des deux n'est installé.
std::vector<std::string> http_get_args(const std::string& url, const std::string& outfile, int timeout_s) {
    std::string t = std::to_string(timeout_s);
    if (command_exists("curl")) {
        std::vector<std::string> a = {"curl", "-fsSL", "--connect-timeout", "5", "--max-time", t};
        if (!outfile.empty()) { a.push_back("-o"); a.push_back(outfile); }
        a.push_back(url);
        return a;
    }
    if (command_exists("wget")) {
        std::vector<std::string> a = {"wget", "-q", "-T", t, "-O", outfile.empty() ? "-" : outfile, url};
        return a;
    }
    return {};
}

static std::string release_url() {
    // Paramètre bidon pour contourner le cache du CDN de GitHub Pages
    return std::string(UPDATE_BASE_URL) + "/release.txt?t=" +
           std::to_string(static_cast<long long>(time(nullptr)));
}

// --- Vérification asynchrone au démarrage (non bloquante) ---

struct Updater {
    pid_t pid = -1;
    int fd = -1;
    std::string buf;
};
Updater g_upd;

void updater_start() {
    if (!g_editor.update_check) return;
    auto args = http_get_args(release_url(), "", 8);
    if (args.empty()) return;
    g_upd.pid = spawn_process(args, nullptr, &g_upd.fd);
    if (g_upd.pid < 0) g_upd.fd = -1;
}

void updater_pump() {
    if (g_upd.fd < 0) return;
    char tmp[1024];
    ssize_t n = read(g_upd.fd, tmp, sizeof(tmp));
    if (n > 0) {
        if (g_upd.buf.size() < 8192) g_upd.buf.append(tmp, static_cast<size_t>(n));
        return;
    }
    if (n < 0 && (errno == EINTR || errno == EAGAIN)) return;

    // Fin du flux : on termine le processus et on analyse
    close(g_upd.fd);
    g_upd.fd = -1;
    int st = 0;
    while (waitpid(g_upd.pid, &st, 0) < 0 && errno == EINTR) {}
    g_upd.pid = -1;

    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return; // hors-ligne / erreur : silence

    std::string v;
    if (parse_release_txt(g_upd.buf, v) && version_compare(v, VIBRA_VERSION) > 0) {
        g_editor.update_available = true;
        g_editor.update_version = v;
        set_status("Mise à jour v" + v + " disponible : Ctrl+U pour l'installer", 10000);
    }
}

std::string self_exe_path() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        std::string p(buf, static_cast<size_t>(n));
        const std::string del = " (deleted)";
        if (p.size() > del.size() && p.compare(p.size() - del.size(), del.size(), del) == 0) {
            p.resize(p.size() - del.size());
        }
        return p;
    }
    std::string a0 = g_argv0;
    char real[4096];
    if (a0.find('/') != std::string::npos) {
        if (realpath(a0.c_str(), real)) return real;
    } else if (const char* path = std::getenv("PATH")) {
        std::string p(path);
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == std::string::npos) end = p.size();
            std::string cand = p.substr(start, end - start) + "/" + a0;
            if (access(cand.c_str(), X_OK) == 0 && realpath(cand.c_str(), real)) return real;
            start = end + 1;
        }
    }
    return "";
}

enum UpdateOutcome { UPD_OK, UPD_UP_TO_DATE, UPD_ERROR };

// Télécharge et installe la dernière version. `target` vide = on interroge release.txt.
UpdateOutcome perform_update(std::string target, std::string& msg,
                             const std::function<void(const std::string&)>& progress) {
    if (target.empty()) {
        progress("Recherche de la dernière version...");
        auto args = http_get_args(release_url(), "", 8);
        if (args.empty()) { msg = "Ni curl ni wget n'est installé (Termux : pkg install curl)."; return UPD_ERROR; }
        std::string out;
        if (!run_capture(args, out, 12000) || !parse_release_txt(out, target)) {
            msg = "Pas d'internet.";
            return UPD_ERROR;
        }
    }

    if (!valid_version(target)) { msg = "Version distante invalide."; return UPD_ERROR; }
    if (version_compare(target, VIBRA_VERSION) <= 0) {
        msg = "Déjà à jour (v" VIBRA_VERSION ").";
        return UPD_UP_TO_DATE;
    }

    std::string arch = getLinuxArch();
    for (char c : arch) {
        if (!ch_alnum(c) && c != '_' && c != '-') { msg = "Architecture non supportée : " + arch; return UPD_ERROR; }
    }

    std::string exe = self_exe_path();
    if (exe.empty()) { msg = "Impossible de localiser l'exécutable courant."; return UPD_ERROR; }

    std::string url = std::string(UPDATE_BASE_URL) + "/releases/" + target + "/" + arch + "/vibra";
    std::string tmp = exe + ".new";

    progress("Téléchargement de la v" + target + " (" + arch + ")...");
    auto dl = http_get_args(url, tmp, 120);
    if (dl.empty()) { msg = "Ni curl ni wget n'est installé."; return UPD_ERROR; }

    std::string ignored;
    if (!run_capture(dl, ignored, 130000)) {
        unlink(tmp.c_str());
        msg = "Téléchargement impossible : releases/" + target + "/" + arch + "/vibra";
        return UPD_ERROR;
    }

    if (chmod(tmp.c_str(), 0755) != 0) {
        unlink(tmp.c_str());
        msg = "chmod impossible sur le fichier téléchargé.";
        return UPD_ERROR;
    }

    // Vérification : le binaire doit s'exécuter et annoncer la bonne version
    progress("Vérification du binaire...");
    std::string ver_out;
    if (!run_capture({tmp, "--version"}, ver_out, 5000) ||
        ver_out.find("vibraEditor") == std::string::npos ||
        ver_out.find("v" + target) == std::string::npos) {
        unlink(tmp.c_str());
        msg = "Binaire téléchargé invalide (mauvaise architecture ou fichier corrompu).";
        return UPD_ERROR;
    }

    if (rename(tmp.c_str(), exe.c_str()) != 0) {
        std::string err = std::strerror(errno);
        unlink(tmp.c_str());
        msg = "Installation impossible dans " + exe + " : " + err;
        return UPD_ERROR;
    }

    msg = "v" + target + " installée : relancez vibra pour l'utiliser.";
    return UPD_OK;
}

int cli_update() {
    std::cout << "vibraEditor v" VIBRA_VERSION " (" << getLinuxArch() << ")\n";
    std::string msg;
    UpdateOutcome r = perform_update("", msg, [](const std::string& s) { std::cout << s << "\n"; });
    (r == UPD_ERROR ? std::cerr : std::cout) << msg << "\n";
    return r == UPD_ERROR ? 1 : 0;
}

// ============================================================================
//  Terminal
// ============================================================================

void update_terminal_size() {
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        g_editor.rows = ws.ws_row;
        g_editor.cols = ws.ws_col;
    }
}

void disable_raw_mode() {
    // 1. Désactiver explicitement le tracking de la souris et les modes d'écran
    raw_write(DISABLE_MOUSE RESET_FORMAT SHOW_CURSOR ENABLE_AUTOWRAP EXIT_ALT_SCREEN);
    tcdrain(STDOUT_FILENO);

    // 2. Pause pour s'assurer que le terminal traite la désactivation de la souris
    usleep(50000); // 50ms

    // 3. Purge complète non-bloquante de l'entrée stdin avant de réactiver le mode canonique
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char dummy[512];
    while (read(STDIN_FILENO, dummy, sizeof(dummy)) > 0) {}

    fcntl(STDIN_FILENO, F_SETFL, flags);
    tcflush(STDIN_FILENO, TCIFLUSH);

    // 4. Restauration finale du mode original
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);

    // 5. Deuxième purge de sécurité après restauration termios
    tcflush(STDIN_FILENO, TCIFLUSH);
}

void handle_signal(int sig) {
    if (sig == SIGWINCH) {
        update_terminal_size();
    } else if (sig == SIGINT || sig == SIGTERM) {
        exit(0);
    }
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    atexit(disable_raw_mode);

    termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);

    raw_write(ENTER_ALT_SCREEN CLEAR_SCREEN ENABLE_MOUSE DISABLE_AUTOWRAP);
}

char read_char() {
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) <= 0) return 0;
    return c;
}

// Largeur de la gouttière (numéros de ligne) : identique pour toutes les lignes
int get_line_prefix_width(int /*line_num*/) {
    if (!g_editor.show_line_numbers) return 0;
    return static_cast<int>(std::to_string(g_editor.lines.size()).length()) + 1;
}

// ============================================================================
//  RENDU
// ============================================================================

static std::string sgr_for(int key) {
    static const char* codes[S_COUNT] = {
        "", ";90", ";33", ";1;36", ";34", ";32", ";35", ";1;32", ";38;5;214", ";35"
    };
    std::string s = "\033[0";
    s += codes[key & 0xFF];
    if (key & (FL_SEL << 8)) s += ";48;5;24";
    if (key & (FL_SEARCH << 8)) s += ";43;30";
    s += "m";
    return s;
}

// Produit la partie visible (défilement horizontal inclus) d'une ligne, colorée.
// `state` est l'état multi-lignes en entrée et reste valable pour la ligne suivante.
static std::string render_line(int idx, int& state, int prefix_width) {
    auto& e = g_editor;
    const std::string& line = e.lines[idx];

    std::vector<uint8_t> st;
    highlight_line(line, state, st, idx);

    std::vector<uint8_t> fl(line.size(), 0);

    if (!e.search_query.empty()) {
        size_t pos = 0, found;
        while ((found = line.find(e.search_query, pos)) != std::string::npos) {
            for (size_t k = found; k < found + e.search_query.size() && k < fl.size(); ++k) fl[k] |= FL_SEARCH;
            pos = found + e.search_query.size();
        }
    }

    bool sel_nl = false;
    int y0, x0, y1, x1;
    if (selection_range(y0, x0, y1, x1) && idx >= y0 && idx <= y1) {
        size_t sa = (idx == y0) ? static_cast<size_t>(x0) : 0;
        size_t sb = (idx == y1) ? static_cast<size_t>(x1) : line.size();
        for (size_t k = sa; k < sb && k < fl.size(); ++k) fl[k] |= FL_SEL;
        sel_nl = (idx < y1);
    }

    int max_text_len = std::max(0, e.cols - prefix_width);

    // Saute les caractères masqués par le défilement horizontal
    size_t b = 0;
    for (int c = 0; c < e.scroll_x && b < line.size(); ++c) b = utf8_next_index(line, b);

    std::string out;
    int key_cur = -1;
    int drawn = 0;
    while (b < line.size() && drawn < max_text_len) {
        size_t nb = utf8_next_index(line, b);
        int key = st[b] | (fl[b] << 8);
        if (key != key_cur) {
            out += sgr_for(key);
            key_cur = key;
        }
        out.append(line, b, nb - b);
        b = nb;
        drawn++;
    }

    // Retour à la ligne sélectionné : on matérialise par un espace surligné
    if (sel_nl && drawn < max_text_len && b >= line.size()) {
        out += sgr_for(FL_SEL << 8);
        out += ' ';
    }

    out += RESET_FORMAT;
    return out;
}

void render() {
    auto& e = g_editor;
    std::string buffer;
    buffer.reserve(e.rows * e.cols * 2);

    buffer += "\033[H" HIDE_CURSOR;

    int edit_rows = e.rows - 1;

    // Ajustement du Scroll Vertical
    if (e.cursor_y < e.scroll_y) {
        e.scroll_y = e.cursor_y;
    }
    if (e.cursor_y >= e.scroll_y + edit_rows) {
        e.scroll_y = e.cursor_y - edit_rows + 1;
    }

    // Ajustement du Scroll Horizontal
    int prefix_width = get_line_prefix_width(0);
    int max_text_cols = std::max(1, e.cols - prefix_width);
    int cursor_char_pos = utf8_length(e.lines[e.cursor_y].substr(0, e.cursor_x));

    if (cursor_char_pos < e.scroll_x) {
        e.scroll_x = cursor_char_pos;
    }
    if (cursor_char_pos >= e.scroll_x + max_text_cols) {
        e.scroll_x = cursor_char_pos - max_text_cols + 1;
    }

    // État multi-lignes (commentaires /* */, template literals) au début de l'écran
    int state = LS_NONE;
    if (e.lang && e.lang->multiline()) {
        std::vector<uint8_t> scratch;
        for (int k = 0; k < e.scroll_y && k < static_cast<int>(e.lines.size()); ++k) {
            highlight_line(e.lines[k], state, scratch, k);
        }
    }

    for (int i = 0; i < edit_rows; ++i) {
        int line_idx = e.scroll_y + i;
        buffer += "\033[K";

        if (line_idx < static_cast<int>(e.lines.size())) {
            if (e.show_line_numbers) {
                std::string num = std::to_string(line_idx + 1);
                std::string pad(std::max(0, prefix_width - 1 - static_cast<int>(num.size())), ' ');
                buffer += "\033[90m" + pad + num + " \033[0m";
            }
            buffer += render_line(line_idx, state, prefix_width);
        } else {
            buffer += "\033[90m•\033[0m";
        }
        buffer += "\r\n";
    }

    // Barre de statut
    buffer += "\033[7m";
    std::string mode_str = e.readonly ? " [RO]" : "";
    std::string syntax_str = "";
    switch (e.syntax_mode) {
        case SYNTAX_CPP:    syntax_str = " [C++]"; break;
        case SYNTAX_PYTHON: syntax_str = " [Python]"; break;
        case SYNTAX_JS:     syntax_str = " [JS/TS]"; break;
        case SYNTAX_SHELL:  syntax_str = " [Shell]"; break;
        default: break;
    }

    std::string search_str = !e.search_query.empty() ? " [RECHERCHE]" : "";
    std::string env_label = e.is_termux ? "[vibraEditor-Termux]" : "[vibraEditor]";

    std::string status = " " + env_label + mode_str + syntax_str + search_str + " "
                       + (e.filename.empty() ? "[Nouveau]" : e.filename)
                       + (e.modified ? " *" : "")
                       + " | L: " + std::to_string(e.cursor_y + 1) + "/" + std::to_string(e.lines.size())
                       + " C: " + std::to_string(utf8_length(e.lines[e.cursor_y].substr(0, e.cursor_x)) + 1);

    bool msg_active = !e.status_msg.empty() && std::chrono::steady_clock::now() < e.status_until;
    if (msg_active) {
        status += " | " + e.status_msg;
    } else {
        if (e.update_available) status += " | MAJ v" + e.update_version + " (^U)";
        status += " | ^S: Sauver | ^Z: Annuler | ^Y: Refaire | ^F: Chercher | ^A/^C: Copier | ^Q: Quitter";
    }

    if (static_cast<int>(utf8_length(status)) < e.cols) {
        int pad = e.cols - utf8_length(status);
        status.append(pad, ' ');
    } else {
        status = utf8_substr(status, 0, e.cols);
    }
    buffer += status + "\033[0m";

    // Curseur
    int screen_y = (e.cursor_y - e.scroll_y) + 1;
    int char_offset = utf8_length(e.lines[e.cursor_y].substr(0, e.cursor_x)) - e.scroll_x;
    int screen_x = 1 + prefix_width + char_offset;

    // Fenêtre de suggestions (autocomplétion)
    if (!e.completions.empty()) {
        int n = static_cast<int>(e.completions.size());
        int plen = static_cast<int>(utf8_length(e.completion_prefix));
        int w = 0;
        for (const auto& c : e.completions) w = std::max(w, static_cast<int>(utf8_length(c)));
        w += 2;
        w = std::min(w, std::max(4, e.cols));

        int col = screen_x - plen - 1;
        if (col + w - 1 > e.cols) col = e.cols - w + 1;
        if (col < 1) col = 1;

        int below = edit_rows - screen_y;
        int top, shown;
        if (below >= n) {
            top = screen_y + 1; shown = n;
        } else if (screen_y - 1 >= n) {
            top = screen_y - n; shown = n;
        } else if (below > 0) {
            top = screen_y + 1; shown = below;
        } else {
            top = std::max(1, screen_y - n); shown = std::min(n, screen_y - 1);
        }

        for (int i = 0; i < shown; ++i) {
            std::string label = utf8_substr(e.completions[i], 0, w - 2);
            int pad = w - 2 - static_cast<int>(utf8_length(label));
            buffer += "\033[" + std::to_string(top + i) + ";" + std::to_string(col) + "H";
            buffer += (i == e.completion_idx) ? "\033[1;30;47m" : "\033[97;48;5;60m";
            buffer += " " + label + std::string(std::max(0, pad), ' ') + " ";
            buffer += RESET_FORMAT;
        }
    }

    buffer += "\033[" + std::to_string(screen_y) + ";" + std::to_string(screen_x) + "H";
    buffer += SHOW_CURSOR;

    raw_write(buffer);
}

void handle_mouse_sgr() {
    finalize_typing_session();
    std::string seq;
    char c;
    while ((c = read_char())) {
        if (c == 'M' || c == 'm') break;
        seq += c;
    }

    int button = 0, x = 0, y = 0;
    if (sscanf(seq.c_str(), "%d;%d;%d", &button, &x, &y) != 3) return;

    int edit_rows = g_editor.rows - 1;
    int max_scroll = std::max(0, static_cast<int>(g_editor.lines.size()) - edit_rows);

    if (button == 64) {
        if (g_editor.scroll_y > 0) {
            g_editor.scroll_y--;
            if (g_editor.cursor_y >= g_editor.scroll_y + edit_rows) {
                g_editor.cursor_y = g_editor.scroll_y + edit_rows - 1;
            }
        }
    } else if (button == 65) {
        if (g_editor.scroll_y < max_scroll) {
            g_editor.scroll_y++;
            if (g_editor.cursor_y < g_editor.scroll_y) {
                g_editor.cursor_y = g_editor.scroll_y;
            }
        }
    } else if (button == 0 || button == 32) {
        clear_selection();
        if (y >= 1 && y <= edit_rows) {
            int target_line = g_editor.scroll_y + (y - 1);
            if (target_line < static_cast<int>(g_editor.lines.size())) {
                g_editor.cursor_y = target_line;

                int prefix_width = get_line_prefix_width(g_editor.cursor_y + 1);
                int target_char_col = (x - 1 - prefix_width) + g_editor.scroll_x;
                if (target_char_col < 0) target_char_col = 0;

                const std::string& line = g_editor.lines[g_editor.cursor_y];
                size_t byte_idx = 0;
                for (int k = 0; k < target_char_col && byte_idx < line.length(); ++k) {
                    byte_idx = utf8_next_index(line, byte_idx);
                }
                g_editor.cursor_x = byte_idx;
            }
        }
    }
}

// ============================================================================
//  Modales
// ============================================================================

bool confirm_exit_modal() {
    finalize_typing_session();
    close_completion();
    int focused = 1;
    const int box_w = 48;
    const int box_h = 9;

    while (true) {
        update_terminal_size();
        int top_r = std::max(1, (g_editor.rows - box_h) / 2);
        int left_c = std::max(1, (g_editor.cols - box_w) / 2);

        int exit_col = left_c + 8;
        int exit_w = 12;
        int cancel_col = left_c + 26;
        int cancel_w = 14;
        int btn_row_top = top_r + 5;
        int btn_row_mid = top_r + 6;
        int btn_row_bot = top_r + 7;

        std::string bg = "\033[48;5;60m";
        std::string fg_white = "\033[1;97m";
        std::string frame_norm = "\033[38;5;110m";
        std::string frame_foc = "\033[1;97m";
        std::string text_exit = "\033[1;31m";
        std::string text_cancel = "\033[38;5;117m";
        std::string reset = "\033[0m";

        render();

        std::string buf = HIDE_CURSOR;

        buf += "\033[" + std::to_string(top_r) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "┌──────────────────────────────────────────────┐" + reset;

        buf += "\033[" + std::to_string(top_r + 1) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                                              │" + reset;

        buf += "\033[" + std::to_string(top_r + 2) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│        Modifications non sauvegardées.       │" + reset;

        buf += "\033[" + std::to_string(top_r + 3) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│        Voulez-vous vraiment quitter ?        │" + reset;

        buf += "\033[" + std::to_string(top_r + 4) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                                              │" + reset;

        std::string exit_frame = (focused == 0) ? frame_foc : frame_norm;
        std::string cancel_frame = (focused == 1) ? frame_foc : frame_norm;

        buf += "\033[" + std::to_string(btn_row_top) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│       "
            + exit_frame + "┌──────────┐" + bg + fg_white + "      "
            + cancel_frame + "┌────────────┐" + bg + fg_white + "       │" + reset;

        buf += "\033[" + std::to_string(btn_row_mid) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│       "
            + exit_frame + "│  " + text_exit + "EXIT" + bg + exit_frame + "    │" + bg + fg_white + "      "
            + cancel_frame + "│   " + text_cancel + "CANCEL" + bg + cancel_frame + "   │" + bg + fg_white + "       │" + reset;

        buf += "\033[" + std::to_string(btn_row_bot) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│       "
            + exit_frame + "└──────────┘" + bg + fg_white + "      "
            + cancel_frame + "└────────────┘" + bg + fg_white + "       │" + reset;

        buf += "\033[" + std::to_string(top_r + 8) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "└──────────────────────────────────────────────┘" + reset;

        raw_write(buf);

        char c = read_char();
        if (c == 0) continue;

        if (c == '\t') {
            focused = 1 - focused;
        } else if (c == 'o' || c == 'O' || c == 'y' || c == 'Y') {
            return true;
        } else if (c == 'n' || c == 'N') {
            return false;
        } else if (c == '\r' || c == '\n' || c == ' ') {
            return (focused == 0);
        } else if (c == '\033') {
            termios current_termios, raw_short;
            tcgetattr(STDIN_FILENO, &current_termios);
            raw_short = current_termios;
            raw_short.c_cc[VMIN] = 0;
            raw_short.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw_short);

            char next = 0;
            if (read(STDIN_FILENO, &next, 1) > 0) {
                if (next == '[') {
                    char sub = 0;
                    if (read(STDIN_FILENO, &sub, 1) > 0) {
                        if (sub == '<') {
                            std::string seq;
                            char sc = 0;
                            while (read(STDIN_FILENO, &sc, 1) > 0) {
                                if (sc == 'M' || sc == 'm') break;
                                seq += sc;
                            }
                            int button = 0, mx = 0, my = 0;
                            if (sscanf(seq.c_str(), "%d;%d;%d", &button, &mx, &my) == 3) {
                                if (button == 0 || button == 32) {
                                    if (my >= btn_row_top && my <= btn_row_bot) {
                                        if (mx >= exit_col && mx < exit_col + exit_w) {
                                            focused = 0;
                                            if (sc == 'm') {
                                                tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
                                                return true;
                                            }
                                        }
                                        if (mx >= cancel_col && mx < cancel_col + cancel_w) {
                                            focused = 1;
                                            if (sc == 'm') {
                                                tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
                                                return false;
                                            }
                                        }
                                    }
                                }
                            }
                        } else if (sub == 'C' || sub == 'D') {
                            focused = 1 - focused;
                        }
                    }
                }
            } else {
                tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
                return false;
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
        }
    }
}

void find_next_match(bool forward = true) {
    if (g_editor.search_query.empty()) return;

    int total_lines = static_cast<int>(g_editor.lines.size());
    int curr_y = g_editor.cursor_y;
    int curr_x = g_editor.cursor_x;

    for (int i = 0; i < total_lines; ++i) {
        int idx = forward ? (curr_y + i) % total_lines
                          : (curr_y - i + total_lines) % total_lines;
        const std::string& line = g_editor.lines[idx];

        size_t pos = std::string::npos;
        if (idx == curr_y) {
            if (forward) {
                pos = line.find(g_editor.search_query, curr_x + 1);
            } else {
                if (curr_x > 0) {
                    pos = line.rfind(g_editor.search_query, curr_x - 1);
                }
            }
        } else {
            pos = forward ? line.find(g_editor.search_query)
                          : line.rfind(g_editor.search_query);
        }

        if (pos != std::string::npos) {
            g_editor.cursor_y = idx;
            g_editor.cursor_x = static_cast<int>(pos);
            return;
        }
    }
}

void search_prompt() {
    finalize_typing_session();
    close_completion();
    clear_selection();
    int orig_y = g_editor.cursor_y;
    int orig_x = g_editor.cursor_x;
    g_editor.search_query.clear();
    g_editor.in_search = true;

    const int box_w = 50;
    const int box_h = 9;

    while (g_editor.in_search) {
        update_terminal_size();
        render();

        int top_r = std::max(1, (g_editor.rows - box_h) / 2);
        int left_c = std::max(1, (g_editor.cols - box_w) / 2);

        std::string bg = "\033[48;5;60m";
        std::string fg_white = "\033[1;97m";
        std::string frame_norm = "\033[38;5;110m";
        std::string frame_foc = "\033[1;97m";
        std::string text_cyan = "\033[38;5;117m";
        std::string reset = "\033[0m";

        std::string buf = HIDE_CURSOR;

        buf += "\033[" + std::to_string(top_r) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "┌────────────────────────────────────────────────┐" + reset;

        buf += "\033[" + std::to_string(top_r + 1) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                   RECHERCHER                   │" + reset;

        buf += "\033[" + std::to_string(top_r + 2) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                                                │" + reset;

        std::string display_q = g_editor.search_query;
        size_t q_len = utf8_length(display_q);
        if (q_len > 35) {
            size_t idx = display_q.length();
            for (size_t k = 0; k < 35; ++k) {
                idx = utf8_prev_index(display_q, idx);
            }
            display_q = display_q.substr(idx);
        }
        int pad_len = std::max(0, 35 - static_cast<int>(utf8_length(display_q)));
        std::string pad(pad_len, ' ');

        buf += "\033[" + std::to_string(top_r + 3) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│   "
            + frame_foc + "┌────────────────────────────────────────┐" + bg + fg_white + "   │" + reset;

        buf += "\033[" + std::to_string(top_r + 4) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│   "
            + frame_foc + "│ " + text_cyan + display_q + "█" + bg + frame_foc + pad + "   │" + bg + fg_white + "   │" + reset;

        buf += "\033[" + std::to_string(top_r + 5) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│   "
            + frame_foc + "└────────────────────────────────────────┘" + bg + fg_white + "   │" + reset;

        buf += "\033[" + std::to_string(top_r + 6) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                                                │" + reset;

        buf += "\033[" + std::to_string(top_r + 7) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│ " + frame_norm + "[Entrée]" + fg_white + " Valider  "
            + frame_norm + "[↑/↓]" + fg_white + " Nav.   "
            + frame_norm + "[Échap]" + fg_white + " Annuler │" + reset;

        buf += "\033[" + std::to_string(top_r + 8) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "└────────────────────────────────────────────────┘" + reset;

        raw_write(buf);

        char c = read_char();
        if (c == 0) continue;

        if (c == 27) {
            termios current_termios, raw_short;
            tcgetattr(STDIN_FILENO, &current_termios);
            raw_short = current_termios;
            raw_short.c_cc[VMIN] = 0;
            raw_short.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw_short);

            char next = 0;
            if (read(STDIN_FILENO, &next, 1) > 0) {
                if (next == '[') {
                    char sub = 0;
                    if (read(STDIN_FILENO, &sub, 1) > 0) {
                        if (sub == 'A') {
                            find_next_match(false);
                        } else if (sub == 'B') {
                            find_next_match(true);
                        } else if (sub == '<') {
                            handle_mouse_sgr();
                        } else {
                            char dummy;
                            while (read(STDIN_FILENO, &dummy, 1) > 0) {
                                if (std::isalpha(dummy) || dummy == '~') break;
                            }
                        }
                    }
                }
            } else {
                g_editor.search_query.clear();
                g_editor.cursor_y = orig_y;
                g_editor.cursor_x = orig_x;
                g_editor.in_search = false;
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
        } else if (c == '\r' || c == '\n') {
            if (!g_editor.search_query.empty()) {
                find_next_match(true);
            }
            g_editor.in_search = false;
        } else if (c == 127 || c == 8) {
            if (!g_editor.search_query.empty()) {
                size_t prev_idx = utf8_prev_index(g_editor.search_query, g_editor.search_query.length());
                g_editor.search_query.erase(prev_idx);
                find_next_match(true);
            }
        } else {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 32 && uc != 127) {
                std::string utf8_char = read_utf8_sequence(c);
                g_editor.search_query += utf8_char;
                find_next_match(true);
            }
        }
    }
}

std::string prompt_input(const std::string& title) {
    finalize_typing_session();
    close_completion();
    std::string input = "";
    const int box_w = 50;
    const int box_h = 9;

    while (true) {
        update_terminal_size();
        render();

        int top_r = std::max(1, (g_editor.rows - box_h) / 2);
        int left_c = std::max(1, (g_editor.cols - box_w) / 2);

        std::string bg = "\033[48;5;60m";
        std::string fg_white = "\033[1;97m";
        std::string frame_norm = "\033[38;5;110m";
        std::string frame_foc = "\033[1;97m";
        std::string text_cyan = "\033[38;5;117m";
        std::string reset = "\033[0m";

        std::string buf = HIDE_CURSOR;

        buf += "\033[" + std::to_string(top_r) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "┌────────────────────────────────────────────────┐" + reset;

        buf += "\033[" + std::to_string(top_r + 1) + ";" + std::to_string(left_c) + "H";

        int title_pad = std::max(0, 48 - static_cast<int>(utf8_length(title)));
        std::string t_left(title_pad / 2, ' ');
        std::string t_right(title_pad - title_pad / 2, ' ');
        buf += bg + fg_white + "│" + t_left + title + t_right + "│" + reset;

        buf += "\033[" + std::to_string(top_r + 2) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                                                │" + reset;

        std::string display_in = input;
        if (utf8_length(display_in) > 35) {
            size_t idx = display_in.length();
            for (size_t k = 0; k < 35; ++k) idx = utf8_prev_index(display_in, idx);
            display_in = display_in.substr(idx);
        }
        int pad_len = std::max(0, 35 - static_cast<int>(utf8_length(display_in)));
        std::string pad(pad_len, ' ');

        buf += "\033[" + std::to_string(top_r + 3) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│   " + frame_foc + "┌────────────────────────────────────────┐" + bg + fg_white + "   │" + reset;

        buf += "\033[" + std::to_string(top_r + 4) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│   " + frame_foc + "│ " + text_cyan + display_in + "█" + bg + frame_foc + pad + "   │" + bg + fg_white + "   │" + reset;

        buf += "\033[" + std::to_string(top_r + 5) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│   " + frame_foc + "└────────────────────────────────────────┘" + bg + fg_white + "   │" + reset;

        buf += "\033[" + std::to_string(top_r + 6) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│                                                │" + reset;

        buf += "\033[" + std::to_string(top_r + 7) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "│       " + frame_norm + "[Entrée]" + fg_white + " Valider  " + frame_norm + "[Échap]" + fg_white + " Annuler        │" + reset;

        buf += "\033[" + std::to_string(top_r + 8) + ";" + std::to_string(left_c) + "H";
        buf += bg + fg_white + "└────────────────────────────────────────────────┘" + reset;

        raw_write(buf);

        char c = read_char();
        if (c == 0) continue;

        if (c == 27) {
            termios current_termios, raw_short;
            tcgetattr(STDIN_FILENO, &current_termios);
            raw_short = current_termios;
            raw_short.c_cc[VMIN] = 0;
            raw_short.c_cc[VTIME] = 1;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw_short);

            char next = 0;
            if (read(STDIN_FILENO, &next, 1) > 0) {
                if (next == '[') {
                    char sub = 0;
                    if (read(STDIN_FILENO, &sub, 1) > 0) {
                        if (sub == '<') {
                            tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
                            handle_mouse_sgr();
                            continue;
                        } else {
                            char dummy;
                            while (read(STDIN_FILENO, &dummy, 1) > 0) {
                                if (std::isalpha(dummy) || dummy == '~') break;
                            }
                        }
                    }
                }
            } else {
                tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
                return "";
            }
            tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
        } else if (c == '\r' || c == '\n') {
            return input;
        } else if (c == 127 || c == 8) {
            if (!input.empty()) {
                size_t prev_idx = utf8_prev_index(input, input.length());
                input.erase(prev_idx);
            }
        } else {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc >= 32 && uc != 127) {
                std::string utf8_char = read_utf8_sequence(c);
                input += utf8_char;
            }
        }
    }
}

void replace_prompt() {
    clear_selection();
    std::string target = prompt_input("RECHERCHER CE TEXTE");
    if (target.empty()) return;

    std::string replacement = prompt_input("REMPLACER PAR");

    save_snapshot();

    bool changed = false;
    for (auto& line : g_editor.lines) {
        size_t pos = 0;
        while ((pos = line.find(target, pos)) != std::string::npos) {
            line.replace(pos, target.length(), replacement);
            pos += replacement.length();
            changed = true;
        }
    }

    if (changed) {
        g_editor.modified = true;
    } else {
        g_editor.undo_stack.pop_back();
    }
}

void save_file() {
    finalize_typing_session();
    if (g_editor.readonly) { set_status("Fichier en lecture seule"); return; }
    if (g_editor.filename.empty()) { set_status("Aucun nom de fichier : lancez vibra <fichier>"); return; }

    fs::path path(g_editor.filename);
    if (g_editor.create_dir && path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(g_editor.filename);
    if (!file.is_open()) { set_status("Échec de l'ouverture en écriture : " + g_editor.filename); return; }
    for (const auto& line : g_editor.lines) {
        file << line << "\n";
    }
    file.flush();
    if (!file) { set_status("Erreur d'écriture : " + g_editor.filename); return; }
    g_editor.modified = false;
    set_status("Sauvegardé");
}

// Ctrl+U : installe la mise à jour (bloquant le temps du téléchargement)
void ui_install_update() {
    close_completion();
    std::string msg;
    auto progress = [](const std::string& s) {
        set_status(s, 120000);
        render();
    };
    UpdateOutcome r = perform_update(g_editor.update_available ? g_editor.update_version : "", msg, progress);
    if (r == UPD_OK) g_editor.update_available = false;
    set_status(msg, 12000);
}

// ============================================================================
//  Entrées clavier
// ============================================================================

void process_keypress() {
    auto& e = g_editor;
    char c = read_char();
    if (c == 0) return;

    e.completion_wanted = false; // réactivé par les frappes de mot

    if (c == 1) { // Ctrl+A
        finalize_typing_session();
        select_all();
    } else if (c == 3) { // Ctrl+C
        finalize_typing_session();
        copy_selection();
    } else if (c == 21) { // Ctrl+U
        ui_install_update();
    } else if (c == 6) { // Ctrl+F
        search_prompt();
    } else if (!e.readonly && c == 26) { // Ctrl+Z
        undo();
    } else if (!e.readonly && c == 25) { // Ctrl+Y
        redo();
    } else if (!e.readonly && c == 11) { // Ctrl+K
        finalize_typing_session();
        clear_selection();
        save_snapshot();
        if (e.lines.size() == 1) {
            e.lines[0] = "";
            e.cursor_x = 0;
        } else {
            e.lines.erase(e.lines.begin() + e.cursor_y);
            if (e.cursor_y >= static_cast<int>(e.lines.size())) {
                e.cursor_y = static_cast<int>(e.lines.size()) - 1;
            }
            e.cursor_x = std::min(e.cursor_x, static_cast<int>(e.lines[e.cursor_y].length()));
        }
        e.modified = true;
    } else if (c == 17) { // Ctrl+Q
        if (!e.modified) {
            exit(0);
        } else {
            if (confirm_exit_modal()) {
                exit(0);
            }
        }
    } else if (!e.readonly && c == 18) { // Ctrl+R
        replace_prompt();
    } else if (c == 19) { // Ctrl+S
        save_file();
    } else if (!e.readonly && (c == 127 || c == 8)) {
        if (e.has_selection) {
            begin_edit(); // supprime la sélection
        } else if (e.cursor_x > 0 || e.cursor_y > 0) {
            prepare_typing_snapshot();
            if (e.cursor_x > 0) {
                size_t prev_x = utf8_prev_index(e.lines[e.cursor_y], e.cursor_x);
                size_t bytes_to_erase = e.cursor_x - prev_x;
                e.lines[e.cursor_y].erase(prev_x, bytes_to_erase);
                e.cursor_x = prev_x;
            } else if (e.cursor_y > 0) {
                e.cursor_x = e.lines[e.cursor_y - 1].length();
                e.lines[e.cursor_y - 1] += e.lines[e.cursor_y];
                e.lines.erase(e.lines.begin() + e.cursor_y);
                e.cursor_y--;
            }
            e.modified = true;
            e.completion_wanted = true;
        }
    } else if (!e.readonly && (c == '\r' || c == '\n')) {
        begin_edit();

        const std::string& current_line = e.lines[e.cursor_y];
        std::string auto_indent = e.in_paste ? "" : compute_auto_indent(current_line, e.cursor_x);

        std::string rest = current_line.substr(e.cursor_x);
        e.lines[e.cursor_y] = current_line.substr(0, e.cursor_x);
        e.lines.insert(e.lines.begin() + e.cursor_y + 1, auto_indent + rest);
        e.cursor_y++;
        e.cursor_x = static_cast<int>(auto_indent.length());
        e.modified = true;
    } else if (!e.readonly && c == '\t') {
        // Tab : accepte la suggestion affichée, sinon insère 4 espaces
        if (!e.in_paste && !e.completions.empty() && accept_completion()) {
            // fait
        } else {
            begin_edit();
            e.lines[e.cursor_y].insert(e.cursor_x, "    ");
            e.cursor_x += 4;
            e.modified = true;
        }
    } else if (c == '\033') {
        finalize_typing_session();
        termios current_termios, raw_short;
        tcgetattr(STDIN_FILENO, &current_termios);
        raw_short = current_termios;
        raw_short.c_cc[VMIN] = 0;
        raw_short.c_cc[VTIME] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_short);

        char next = 0;
        if (read(STDIN_FILENO, &next, 1) > 0) {
            if (next == '[') {
                char sub = 0;
                if (read(STDIN_FILENO, &sub, 1) > 0) {
                    if (sub == '<') {
                        tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
                        handle_mouse_sgr();
                        return;
                    }

                    std::string seq(1, sub);
                    if (!std::isalpha(sub) && sub != '~') {
                        char ch = 0;
                        while (read(STDIN_FILENO, &ch, 1) > 0) {
                            seq += ch;
                            if (std::isalpha(ch) || ch == '~') break;
                        }
                    }
                    tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);

                    if (seq == "200~") {
                        close_completion();
                        if (!e.readonly) {
                            save_snapshot();
                            if (e.has_selection) delete_selection();
                        }
                        e.in_paste = true;
                    } else if (seq == "201~") {
                        e.in_paste = false;
                        finalize_typing_session();
                        close_completion();
                    } else if ((seq == "A" || seq == "B") && !e.completions.empty()) {
                        // Navigation dans la fenêtre de suggestions
                        int n = static_cast<int>(e.completions.size());
                        e.completion_idx = (e.completion_idx + (seq == "B" ? 1 : n - 1)) % n;
                        e.completion_wanted = true;
                    } else {
                        clear_selection();
                        if (seq == "A") {
                            if (!e.search_query.empty()) {
                                find_next_match(false);
                            } else if (e.cursor_y > 0) {
                                e.cursor_y--;
                            }
                        } else if (seq == "B") {
                            if (!e.search_query.empty()) {
                                find_next_match(true);
                            } else if (e.cursor_y < static_cast<int>(e.lines.size()) - 1) {
                                e.cursor_y++;
                            }
                        } else if (seq == "C") {
                            e.cursor_x = utf8_next_index(e.lines[e.cursor_y], e.cursor_x);
                        } else if (seq == "D") {
                            e.cursor_x = utf8_prev_index(e.lines[e.cursor_y], e.cursor_x);
                        }
                        else if (seq == "H" || seq == "1~" || seq == "7~") {
                            e.cursor_x = 0;
                        } else if (seq == "F" || seq == "4~" || seq == "8~") {
                            e.cursor_x = static_cast<int>(e.lines[e.cursor_y].length());
                        } else if (seq == "1;5H" || seq == "1;5~") {
                            e.cursor_y = 0;
                            e.cursor_x = 0;
                        } else if (seq == "1;5F" || seq == "4;5~") {
                            e.cursor_y = static_cast<int>(e.lines.size()) - 1;
                            e.cursor_x = static_cast<int>(e.lines[e.cursor_y].length());
                        }
                    }

                    e.cursor_y = std::clamp(e.cursor_y, 0, static_cast<int>(e.lines.size()) - 1);
                    e.cursor_x = std::min(e.cursor_x, static_cast<int>(e.lines[e.cursor_y].length()));
                }
            } else if (next == 'O') {
                clear_selection();
                char sub = 0;
                if (read(STDIN_FILENO, &sub, 1) > 0) {
                    if (sub == 'H') e.cursor_x = 0;
                    else if (sub == 'F') e.cursor_x = static_cast<int>(e.lines[e.cursor_y].length());
                }
                tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
            }
        } else {
            // Échap seul : ferme recherche, sélection et suggestions
            if (!e.search_query.empty()) {
                e.search_query.clear();
            }
            clear_selection();
            tcsetattr(STDIN_FILENO, TCSANOW, &current_termios);
        }
    } else if (!e.readonly) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc != 127) {
            begin_edit();
            std::string utf8_char = read_utf8_sequence(c);
            e.lines[e.cursor_y].insert(e.cursor_x, utf8_char);
            e.cursor_x += utf8_char.length();
            e.modified = true;
            e.completion_wanted = is_word_char(c);
        }
    }
}

// Garde-fou : curseur toujours dans les bornes (molette, undo, etc.)
static void clamp_cursor() {
    auto& e = g_editor;
    e.cursor_y = std::clamp(e.cursor_y, 0, static_cast<int>(e.lines.size()) - 1);
    e.cursor_x = std::clamp(e.cursor_x, 0, static_cast<int>(e.lines[e.cursor_y].length()));
}

int main(int argc, char* argv[]) {
    g_argv0 = argv[0];
    signal(SIGPIPE, SIG_IGN);
    g_editor.is_termux = detect_termux();

    bool do_update = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        } else if (arg == "-d" || arg == "--create-dir") {
            g_editor.create_dir = true;
        } else if (arg == "-r" || arg == "--readonly") {
            g_editor.readonly = true;
        } else if (arg == "-n" || arg == "--no-numbers") {
            g_editor.show_line_numbers = false;
        } else if (arg == "-a" || arg == "--no-autocomplete") {
            g_editor.autocomplete = false;
        } else if (arg == "--no-update-check") {
            g_editor.update_check = false;
        } else if (arg == "-u" || arg == "--update") {
            do_update = true;
        } else if (arg[0] != '-') {
            g_editor.filename = arg;
        } else {
            std::cerr << "Option inconnue : " << arg << "\nUtilisez -h pour afficher l'aide.\n";
            return 1;
        }
    }

    if (do_update) return cli_update();

    if (!g_editor.filename.empty()) {
        check_syntax_mode();
        fs::path path(g_editor.filename);

        if (g_editor.create_dir && path.has_parent_path()) {
            fs::create_directories(path.parent_path());
        }

        std::ifstream file(g_editor.filename);
        if (file.is_open()) {
            g_editor.lines.clear();
            std::string line;
            while (std::getline(file, line)) {
                g_editor.lines.push_back(line);
            }
            if (g_editor.lines.empty()) g_editor.lines.push_back("");
            detect_syntax_from_shebang();
        }
    }

    signal(SIGWINCH, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    enable_raw_mode();
    update_terminal_size();

    updater_start(); // vérification de mise à jour en arrière-plan

    while (true) {
        clamp_cursor();
        update_completions();
        if (!has_pending_input()) {
            render();
        }

        // Attente : clavier, résultat de la vérification de MAJ, ou fin du message de statut
        pollfd fds[2];
        nfds_t nf = 1;
        fds[0] = {STDIN_FILENO, POLLIN, 0};
        if (g_upd.fd >= 0) {
            fds[1] = {g_upd.fd, POLLIN, 0};
            nf = 2;
        }

        int timeout = -1;
        if (!g_editor.status_msg.empty()) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            g_editor.status_until - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                g_editor.status_msg.clear();
                continue; // re-rendu sans message
            }
            timeout = static_cast<int>(left) + 10;
        }

        int r = poll(fds, nf, timeout);
        if (r < 0) continue; // EINTR (SIGWINCH...) : on redessine

        if (nf == 2 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            updater_pump();
        }

        if (fds[0].revents & POLLIN) {
            process_keypress();
            while (has_pending_input()) {
                process_keypress();
            }
        } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            exit(0); // terminal fermé
        }
    }

    return 0;
}
