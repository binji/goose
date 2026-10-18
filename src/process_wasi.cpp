// Goose compiler — other processes, where there are none: the Wasm build
// (docs/design/interpreter.md §8). WASI has no processes to start, no PATH,
// no shared libraries to load and no cross-process locks, so every
// function of process.h answers that the thing is not available, and the
// verbs that need one (build, fetch, a package built now) say so. The two
// functions that are only text are the same as process.cpp's.
#include "includes.h"
#include "utils.h"
#include "process.h"

namespace goose {

static const char *const unavailable = "not available in the browser build: no processes can be run";

ProcessResult RunProcess(const vector<string> &argv, const string &, bool) {
    ProcessResult r;
    r.output = cat(unavailable, " (", argv.empty() ? string("?") : argv[0], ")");
    return r;
}

string FindOnPath(const string &, const char *) { return ""; }

void SetEnvVar(const string &name, const string &value) {
    setenv(name.c_str(), value.c_str(), 1);
}

FileLock::FileLock(const string &, const string &) {}
FileLock::~FileLock() {}

bool LoadSharedSymbols(const string &path, vector<pair<string, const void *>> &, string &err) {
    err = cat(unavailable, ": ", path);
    return false;
}

string MacSdkPath() { return ""; }

string CommandLineText(const vector<string> &argv) {
    string s;
    for (auto &a : argv) {
        if (!s.empty()) s += ' ';
        if (!a.empty() && a.find_first_of(" \t\"'&|<>^();$`\\") == string::npos) {
            s += a;
            continue;
        }
        s += '"';
        for (auto c : a) {
            if (c == '"') s += '\\';
            s += c;
        }
        s += '"';
    }
    return s;
}

}  // namespace goose
