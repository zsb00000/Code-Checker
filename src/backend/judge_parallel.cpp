#include "../../include/ThreadPool.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <direct.h>
#include <fstream>
#include <io.h>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

using namespace std;

const vector<string> SUPPORTED_STDS = {"c++98", "c++11", "c++14", "c++17",
                                       "c++20"};

const int DEFAULT_UNKNOWN_TIME_LIMIT_MS = 2000;
const int DEFAULT_UNKNOWN_MEMORY_LIMIT_MB = 512;
const int ANS_TIME_LIMIT_MS = 60000;
const int ANS_MEMORY_LIMIT_MB = 4096;
const int MAKE_TIME_LIMIT_MS = 5000;

enum ErrorType
{
    AC = 0,
    WA = 1,
    RE = 2,
    TLE = 3,
    MLE = 4,
    CE = 5,
    UKE = 6
};

struct JudgeResult
{
    int id;
    string result;
    string message;
    string std_version;
    ErrorType error_type;
    string input_data;
    string ans_output;
    string unk_output;
    bool files_saved;
    string saved_path;
};

class Logger
{
  private:
    ofstream log_file;
    int task_id;
    string log_path;
    mutex log_mutex;

  public:
    Logger(const string &dir, int id) : task_id(id)
    {
        log_path = dir + "\\task_" + to_string(id) + "_log.txt";
        log_file.open(log_path, ios::app);
        log_file << "=== Task " << id << " Start ===" << endl;
    }
    ~Logger()
    {
        if (log_file.is_open())
        {
            log_file << "=== End ===" << endl;
            log_file.close();
        }
    }
    void log(const string &msg)
    {
        lock_guard<mutex> lock(log_mutex);
        if (log_file.is_open())
            log_file << msg << endl;
        cerr << "[Task " << task_id << "] " << msg << endl;
    }
    string get_log_path() const { return log_path; }
};

string json_escape(const string &s)
{
    string o;
    for (char c : s)
    {
        switch (c)
        {
        case '"':
            o += "\\\"";
            break;
        case '\\':
            o += "\\\\";
            break;
        case '\n':
            o += "\\n";
            break;
        case '\r':
            o += "\\r";
            break;
        case '\t':
            o += "\\t";
            break;
        default:
            if (c >= 0x20 && c <= 0x7E)
                o += c;
            else
                o += ' ';
        }
    }
    return o;
}

string random_string(int len)
{
    const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    string s;
    static thread_local mt19937 gen(
        chrono::steady_clock::now().time_since_epoch().count());
    uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    for (int i = 0; i < len; ++i)
        s += chars[dist(gen)];
    return s;
}

string create_task_dir(int task_id)
{
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    string dir = string(temp_path) + "judge_" + to_string(task_id) + "_" +
                 to_string(GetCurrentProcessId()) + "_" + random_string(8);
    _mkdir(dir.c_str());
    if (_access(dir.c_str(), 0) == -1)
    {
        cerr << "Failed to create: " << dir << endl;
        exit(1);
    }
    return dir;
}

bool copy_file_safe(const string &src, const string &dst)
{
    for (int i = 0; i < 3; ++i)
    {
        ifstream in(src, ios::binary);
        ofstream out(dst, ios::binary | ios::trunc);
        if (in && out)
        {
            out << in.rdbuf();
            out.close();
            ifstream check(dst, ios::binary | ios::ate);
            if (check && check.tellg() > 0)
                return true;
        }
        Sleep(50);
    }
    return false;
}

void remove_directory(const string &dir)
{
    string cmd = "rd /S /Q \"" + dir + "\" 2>nul";
    system(cmd.c_str());
}

void ensure_dir(const string &dir)
{
    size_t pos = 0;
    while ((pos = dir.find_first_of("\\/", pos)) != string::npos)
    {
        string sub = dir.substr(0, pos);
        if (!sub.empty())
            _mkdir(sub.c_str());
        ++pos;
    }
    _mkdir(dir.c_str());
}

bool compile(const string &dir, const string &src, const string &exe,
             const string &std_v, string &err, Logger &log)
{
    string cmd = "g++ -O2 -std=" + std_v + " -o \"" + dir + "\\" + exe +
                 ".exe\" \"" + dir + "\\" + src + "\" 2>\"" + dir + "\\" + exe +
                 "_err.txt\"";
    log.log("Compile cmd: " + cmd);

    int ret = system(cmd.c_str());
    if (ret != 0)
    {
        ifstream f(dir + "\\" + exe + "_err.txt");
        stringstream b;
        b << f.rdbuf();
        err = b.str();
        log.log("Compile failed: " + err.substr(0, 100));
        return false;
    }
    log.log("Compile OK");
    return true;
}

struct RunResult
{
    int exit_code;
    string err_msg;
};

RunResult run_program(const string &dir, const string &prog,
                      const string &in_file, const string &out_file,
                      int time_ms, int mem_mb, Logger &log)
{
    RunResult res = {0, ""};

    string exe = dir + "\\" + prog + ".exe";
    string input = in_file.empty() ? "" : (dir + "\\" + in_file);
    string output = dir + "\\" + out_file;
    string err_file = dir + "\\" + prog + "_err.txt";

    log.log("Run: " + prog + " timeout=" + to_string(time_ms));

    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};

    HANDLE hOut = CreateFileA(output.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut == INVALID_HANDLE_VALUE)
    {
        log.log("Failed to create output file");
        return {-1, "Cannot create output"};
    }

    HANDLE hIn = INVALID_HANDLE_VALUE;
    if (!input.empty())
    {
        hIn = CreateFileA(input.c_str(), GENERIC_READ, FILE_SHARE_READ, &sa,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hIn == INVALID_HANDLE_VALUE)
        {
            CloseHandle(hOut);
            log.log("Failed to open input: " + input);
            return {-1, "Cannot open input"};
        }
    }

    HANDLE hErr = CreateFileA(err_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hIn;
    si.hStdOutput = hOut;
    si.hStdError = (hErr != INVALID_HANDLE_VALUE) ? hErr : hOut;

    PROCESS_INFORMATION pi = {0};
    string cmd = "\"" + exe + "\"";

    log.log("CreateProcess: " + cmd);
    BOOL created = CreateProcessA(
        NULL, const_cast<char *>(cmd.c_str()), NULL, NULL, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, dir.c_str(), &si, &pi);

    if (!created)
    {
        CloseHandle(hOut);
        if (hIn != INVALID_HANDLE_VALUE)
            CloseHandle(hIn);
        if (hErr != INVALID_HANDLE_VALUE)
            CloseHandle(hErr);
        log.log("CreateProcess failed: " + to_string(GetLastError()));
        return {-1, "Failed to start process"};
    }

    HANDLE hJob = NULL;
    if (mem_mb > 0)
    {
        hJob = CreateJobObject(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        jeli.ProcessMemoryLimit = (SIZE_T)mem_mb * 1024 * 1024;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli,
                                sizeof(jeli));
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    DWORD wait = WaitForSingleObject(pi.hProcess, time_ms);

    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 1);
        res.exit_code = -2;
        res.err_msg = "Time Limit Exceeded";
        log.log("TLE");
    }
    else
    {
        DWORD code;
        GetExitCodeProcess(pi.hProcess, &code);
        if (code == 0)
        {
            res.exit_code = 0;
            log.log("Exit 0");
        }
        else if (code == 0xC0000017 || code == 0xC0000005)
        {
            res.exit_code = -3;
            res.err_msg = "Memory Limit";
            log.log("MLE/RE code=" + to_string(code));
        }
        else
        {
            res.exit_code = -1;
            res.err_msg = "Runtime Error code=" + to_string(code);
            log.log("RE code=" + to_string(code));
        }
    }

    CloseHandle(pi.hProcess);
    CloseHandle(hOut);
    if (hIn != INVALID_HANDLE_VALUE)
        CloseHandle(hIn);
    if (hErr != INVALID_HANDLE_VALUE)
        CloseHandle(hErr);
    if (hJob)
        CloseHandle(hJob);

    Sleep(50);

    return res;
}

string read_file(const string &f, int max = 100000)
{
    ifstream file(f, ios::binary | ios::ate);
    if (!file)
        return "";
    auto size = min((streamsize)max, (streamsize)file.tellg());
    file.seekg(0);
    string s(size, '\0');
    file.read(&s[0], size);
    if (file.tellg() > 10000)
        s = s.substr(0, 10000) + "\n...(truncated)";
    return s;
}

string trim(const string &s)
{
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == string::npos) ? "" : s.substr(0, end + 1);
}

bool compare_files(const string &f1, const string &f2)
{
    ifstream a(f1), b(f2);
    if (!a || !b)
        return false;

    string l1, l2;
    while (getline(a, l1) && getline(b, l2))
    {
        if (trim(l1) != trim(l2))
            return false;
    }
    if (getline(a, l1) || getline(b, l2))
    {
        do
        {
            if (trim(l1) != "")
                return false;
        } while (getline(a, l1));
        do
        {
            if (trim(l2) != "")
                return false;
        } while (getline(b, l2));
    }
    return true;
}

bool save_files(const string &work, const string &save, int id,
                const JudgeResult &res, Logger &log)
{
    if (save.empty())
        return false;

    string d = save + "\\task_" + to_string(id);
    ensure_dir(d);

    ofstream(d + "\\input.txt") << res.input_data;
    ofstream(d + "\\expected.txt") << res.ans_output;
    ofstream(d + "\\output.txt") << res.unk_output;
    copy_file_safe(log.get_log_path(), d + "\\log.txt");

    ofstream sum(d + "\\summary.txt");
    sum << "Task: " << id << "\nResult: " << res.result
        << "\nMsg: " << res.message;

    log.log("Saved to " + d);
    return true;
}

JudgeResult judge(int id, const string &make, const string &ans,
                  const string &unk, const string &std_v, int time_ms,
                  int mem_mb, const string &save)
{
    JudgeResult res;
    res.id = id;
    res.std_version = std_v;
    res.files_saved = false;

    string dir = create_task_dir(id);
    Logger log(dir, id);

    log.log("Judge start in " + dir);

    try
    {
        if (!copy_file_safe(make, dir + "\\make.cpp") ||
            !copy_file_safe(ans, dir + "\\ans.cpp") ||
            !copy_file_safe(unk, dir + "\\unknown.cpp"))
        {
            throw runtime_error("Copy failed");
        }

        string err;
        if (!compile(dir, "make.cpp", "make", std_v, err, log))
        {
            res = {id,    "CE", "make.cpp compile error", std_v, CE, "", "", "",
                   false, ""};
            remove_directory(dir);
            return res;
        }
        if (!compile(dir, "ans.cpp", "ans", std_v, err, log))
        {
            res = {id,    "CE", "ans.cpp compile error", std_v, CE, "", "", "",
                   false, ""};
            remove_directory(dir);
            return res;
        }
        if (!compile(dir, "unknown.cpp", "unknown", std_v, err, log))
        {
            res = {
                id,    "CE", "unknown.cpp compile error", std_v, CE, "", "", "",
                false, ""};
            remove_directory(dir);
            return res;
        }

        auto r1 =
            run_program(dir, "make", "", "data.in", MAKE_TIME_LIMIT_MS, 0, log);
        if (r1.exit_code != 0)
        {
            res = {id,    "UKE", "make failed: " + r1.err_msg,
                   std_v, UKE,   "",
                   "",    "",    false,
                   ""};
            remove_directory(dir);
            return res;
        }

        string data_in = read_file(dir + "\\data.in");

        auto r2 = run_program(dir, "ans", "data.in", "data.ans",
                              ANS_TIME_LIMIT_MS, ANS_MEMORY_LIMIT_MB, log);
        if (r2.exit_code != 0)
        {
            res = {id,    "UKE", "ans failed: " + r2.err_msg,
                   std_v, UKE,   data_in,
                   "",    "",    false,
                   ""};
            remove_directory(dir);
            return res;
        }

        auto r3 = run_program(dir, "unknown", "data.in", "data.out", time_ms,
                              mem_mb, log);

        string ans_out = read_file(dir + "\\data.ans");
        string unk_out = read_file(dir + "\\data.out");

        if (r3.exit_code == -2)
        {
            res = {id,      "TLE",   "Time Limit Exceeded",
                   std_v,   TLE,     data_in,
                   ans_out, unk_out, false,
                   ""};
        }
        else if (r3.exit_code == -3)
        {
            res = {id,      "MLE",   "Memory Limit Exceeded",
                   std_v,   MLE,     data_in,
                   ans_out, unk_out, false,
                   ""};
        }
        else if (r3.exit_code != 0)
        {
            res = {id,      "RE",    "Runtime Error", std_v, RE,
                   data_in, ans_out, unk_out,         false, ""};
        }
        else
        {
            if (compare_files(dir + "\\data.ans", dir + "\\data.out"))
            {
                res = {id,      "AC",    "Accepted", std_v, AC,
                       data_in, ans_out, unk_out,    false, ""};
            }
            else
            {
                res = {id,      "WA",    "Wrong Answer", std_v, WA,
                       data_in, ans_out, unk_out,        false, ""};
            }
        }

        if (res.error_type != AC)
        {
            if (save_files(dir, save, id, res, log))
            {
                res.files_saved = true;
                res.saved_path = save + "\\task_" + to_string(id);
            }
        }
    }
    catch (exception &e)
    {
        log.log("Exception: " + string(e.what()));
        res = {id,    "UKE", string("Exception: ") + e.what(),
               std_v, UKE,   "",
               "",    "",    false,
               ""};
    }

    remove_directory(dir);
    return res;
}

int main(int argc, char *argv[])
{
    if (argc != 9)
    {
        cerr << "Usage: " << argv[0]
             << " make.cpp ans.cpp unknown.cpp k std time mem savedir" << endl;
        return 1;
    }

    string make = argv[1], ans = argv[2], unk = argv[3];
    int k = atoi(argv[4]);
    string std_v = argv[5];
    int time = atoi(argv[6]), mem = atoi(argv[7]);
    string save = argv[8];

    transform(std_v.begin(), std_v.end(), std_v.begin(), ::tolower);
    if (find(SUPPORTED_STDS.begin(), SUPPORTED_STDS.end(), std_v) ==
        SUPPORTED_STDS.end())
    {
        cerr << "Bad std: " << std_v << endl;
        return 1;
    }

    if (!save.empty())
    {
        replace(save.begin(), save.end(), '/', '\\');
        ensure_dir(save);
    }

    replace(make.begin(), make.end(), '/', '\\');
    replace(ans.begin(), ans.end(), '/', '\\');
    replace(unk.begin(), unk.end(), '/', '\\');

    int T = min(k, 4);
    ThreadPool pool(T);

    vector<future<JudgeResult>> futs;
    for (int i = 0; i < k; ++i)
    {
        futs.push_back(
            pool.enqueue(judge, i, make, ans, unk, std_v, time, mem, save));
    }

    vector<JudgeResult> rs;
    int cnt[7] = {0};

    for (auto &f : futs)
    {
        auto r = f.get();
        rs.push_back(r);
        cnt[r.error_type]++;
    }

    cout << "{" << endl;
    cout << "  \"total\": " << k << "," << endl;
    cout << "  \"ac\": " << cnt[0] << "," << endl;
    cout << "  \"wa\": " << cnt[1] << "," << endl;
    cout << "  \"re\": " << cnt[2] << "," << endl;
    cout << "  \"tle\": " << cnt[3] << "," << endl;
    cout << "  \"mle\": " << cnt[4] << "," << endl;
    cout << "  \"ce\": " << cnt[5] << "," << endl;
    cout << "  \"uke\": " << cnt[6] << "," << endl;
    cout << "  \"std_version\": \"" << std_v << "\"," << endl;
    cout << "  \"time_limit\": " << time << "," << endl;
    cout << "  \"memory_limit\": " << mem << "," << endl;
    cout << "  \"results\": [" << endl;

    for (size_t i = 0; i < rs.size(); ++i)
    {
        cout << "    {\"id\": " << rs[i].id << ", \"result\": \""
             << rs[i].result << "\", \"message\": \""
             << json_escape(rs[i].message) << "\", \"std\": \""
             << rs[i].std_version << "\", \"files_saved\": "
             << (rs[i].files_saved ? "true" : "false") << "}";
        if (i + 1 < rs.size())
            cout << ",";
        cout << endl;
    }
    cout << "  ]" << endl << "}" << endl;

    return 0;
}