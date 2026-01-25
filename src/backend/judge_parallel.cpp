#include "../../include/ThreadPool.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

using namespace std;

const int UNKNOWN_TIME_LIMIT_MS = 2000;
const int ANS_TIME_LIMIT_MS = 60000;

struct JudgeResult
{
    int id;
    string result;
    string message;
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
        chrono::steady_clock::now().time_since_epoch().count() +
        hash<thread::id>{}(this_thread::get_id()));
    uniform_int_distribution<> dist(0, sizeof(chars) - 2);
    for (int i = 0; i < len; ++i)
        s += chars[dist(gen)];
    return s;
}

string create_task_dir(int task_id)
{
    char temp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    string dir = string(temp_path) + "task_" + to_string(task_id) + "_" +
                 to_string(GetCurrentProcessId()) + "_" + random_string(10);
    if (!CreateDirectoryA(dir.c_str(), NULL))
    {
        cerr << "Failed to create dir: " << dir << endl;
        exit(1);
    }
    return dir;
}

bool copy_file_safe(const string &src, const string &dst)
{
    for (int i = 0; i < 5; ++i)
    {
        ifstream in(src, ios::binary);
        ofstream out(dst, ios::binary | ios::trunc);
        if (in && out)
        {
            out << in.rdbuf();
            out.flush();
            out.close();
            in.close();
            // 验证
            ifstream verify(dst, ios::binary | ios::ate);
            if (verify && verify.tellg() > 0)
                return true;
        }
        Sleep(20);
    }
    return false;
}

// 编译：使用 system()（可靠，能搜索 PATH）
bool compile_in_dir(const string &dir, const string &src, const string &exe,
                    string &err)
{
    string exe_path = dir + "\\" + exe + ".exe";
    string src_path = dir + "\\" + src;
    string err_path = dir + "\\" + exe + "_err.txt";

    // 使用 system 调用 g++，能正确搜索 PATH
    string cmd = "g++ -O2 -std=c++17 -o \"" + exe_path + "\" \"" + src_path +
                 "\" 2>\"" + err_path + "\"";

    int ret = system(cmd.c_str());
    if (ret != 0)
    {
        ifstream f(err_path);
        stringstream b;
        b << f.rdbuf();
        err = b.str();
        if (err.length() > 150)
            err = err.substr(0, 150) + "...";
        return false;
    }
    return true;
}

// 运行：使用 cmd.exe /c 进行重定向，避免句柄继承问题
int run_with_redirect(const string &dir, const string &exe, const string &input,
                      const string &output, int time_limit, bool limit_mem)
{

    string exe_path = dir + "\\" + exe + ".exe";
    string in_path = dir + "\\" + input;
    string out_path = dir + "\\" + output;

    // 等待输入文件就绪
    if (!input.empty())
    {
        for (int i = 0; i < 50; ++i)
        {
            if (GetFileAttributesA(in_path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                // 检查文件是否被锁定（尝试打开）
                HANDLE hTest =
                    CreateFileA(in_path.c_str(), GENERIC_READ, 0, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hTest != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(hTest);
                    break;
                }
            }
            Sleep(10);
        }
    }

    // 构建命令：cmd.exe /c "program < in > out"
    string cmd = "cmd.exe /c \"\"" + exe_path + "\"";
    if (!input.empty())
        cmd += " < \"" + in_path + "\"";
    cmd += " > \"" + out_path + "\"\"";

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, const_cast<char *>(cmd.c_str()), NULL, NULL,
                        FALSE, // FALSE = 不继承句柄
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, NULL,
                        dir.c_str(), &si, &pi))
    {
        return -1;
    }

    // 内存限制（仅 unknown）
    HANDLE hJob = NULL;
    if (limit_mem)
    {
        hJob = CreateJobObject(NULL, NULL);
        if (hJob)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
            jeli.BasicLimitInformation.LimitFlags =
                JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            jeli.ProcessMemoryLimit = 512 * 1024 * 1024;
            SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                    &jeli, sizeof(jeli));
            AssignProcessToJobObject(hJob, pi.hProcess);
        }
    }

    CloseHandle(pi.hThread);

    DWORD wait = WaitForSingleObject(pi.hProcess, time_limit);
    int ret = 0;

    if (wait == WAIT_TIMEOUT)
    {
        TerminateProcess(pi.hProcess, 1);
        ret = -2; // TLE
    }
    else if (wait == WAIT_OBJECT_0)
    {
        DWORD code;
        ret = (GetExitCodeProcess(pi.hProcess, &code) && code == 0) ? 0 : -1;
    }
    else
    {
        ret = -1;
    }

    CloseHandle(pi.hProcess);
    if (hJob)
        CloseHandle(hJob);

    // 等待输出文件写入完成
    if (ret == 0)
    {
        for (int i = 0; i < 50; ++i)
        {
            WIN32_FILE_ATTRIBUTE_DATA attr;
            if (GetFileAttributesExA(out_path.c_str(), GetFileExInfoStandard,
                                     &attr))
            {
                if (attr.nFileSizeLow > 0)
                {
                    // 再检查一次大小是否稳定
                    Sleep(20);
                    WIN32_FILE_ATTRIBUTE_DATA attr2;
                    if (GetFileAttributesExA(out_path.c_str(),
                                             GetFileExInfoStandard, &attr2))
                    {
                        if (attr.nFileSizeLow == attr2.nFileSizeLow)
                            break;
                    }
                }
            }
            Sleep(10);
        }
    }

    return ret;
}

string trim_trailing(const string &s)
{
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == string::npos) ? "" : s.substr(0, end + 1);
}

bool compare_outputs(const string &f1, const string &f2)
{
    // 确保文件存在且稳定
    for (int i = 0; i < 20; ++i)
    {
        WIN32_FILE_ATTRIBUTE_DATA a1, a2;
        bool ok1 = GetFileAttributesExA(f1.c_str(), GetFileExInfoStandard, &a1);
        bool ok2 = GetFileAttributesExA(f2.c_str(), GetFileExInfoStandard, &a2);
        if (ok1 && ok2 && a1.nFileSizeLow > 0 && a2.nFileSizeLow > 0)
        {
            Sleep(30); // 额外等待确保写入完成
            break;
        }
        Sleep(50);
    }

    for (int retry = 0; retry < 5; ++retry)
    {
        ifstream file1(f1), file2(f2);
        if (!file1 || !file2)
        {
            Sleep(50);
            continue;
        }

        vector<string> l1, l2;
        string line;
        while (getline(file1, line))
            l1.push_back(trim_trailing(line));
        while (getline(file2, line))
            l2.push_back(trim_trailing(line));

        while (!l1.empty() && l1.back().empty())
            l1.pop_back();
        while (!l2.empty() && l2.back().empty())
            l2.pop_back();

        if (l1.size() != l2.size())
            return false;
        bool same = true;
        for (size_t i = 0; i < l1.size(); ++i)
            if (l1[i] != l2[i])
            {
                same = false;
                break;
            }

        if (same)
            return true;
        Sleep(50);
    }
    return false;
}

JudgeResult judge_task(int task_id, const string &make_src,
                       const string &ans_src, const string &unknown_src)
{
    JudgeResult res;
    res.id = task_id;
    string work_dir = create_task_dir(task_id);

    try
    {
        // 复制源文件
        if (!copy_file_safe(make_src, work_dir + "\\make.cpp") ||
            !copy_file_safe(ans_src, work_dir + "\\ans.cpp") ||
            !copy_file_safe(unknown_src, work_dir + "\\unknown.cpp"))
        {
            throw runtime_error("File copy failed");
        }

        string err;
        // 编译三个文件
        if (!compile_in_dir(work_dir, "make.cpp", "make", err))
        {
            res.result = "UKE";
            res.message =
                "Task " + to_string(task_id) + " make compile: " + err;
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }

        if (!compile_in_dir(work_dir, "ans.cpp", "ans", err))
        {
            res.result = "UKE";
            res.message = "Task " + to_string(task_id) + " ans compile error";
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }

        if (!compile_in_dir(work_dir, "unknown.cpp", "unknown", err))
        {
            res.result = "UKE";
            res.message =
                "Task " + to_string(task_id) + " unknown compile error";
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }

        // 运行 make
        if (run_with_redirect(work_dir, "make", "", "out1", 5000, false) != 0)
        {
            res.result = "UKE";
            res.message = "Task " + to_string(task_id) + " make runtime error";
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }

        // 运行 ans（不限制）
        if (run_with_redirect(work_dir, "ans", "out1", "std", ANS_TIME_LIMIT_MS,
                              false) != 0)
        {
            res.result = "UKE";
            res.message = "Task " + to_string(task_id) + " ans runtime error";
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }

        // 运行 unknown（限制资源）
        int unk_ret = run_with_redirect(work_dir, "unknown", "out1", "out2",
                                        UNKNOWN_TIME_LIMIT_MS, true);
        if (unk_ret == -2)
        {
            res.result = "UKE";
            res.message = "Task " + to_string(task_id) + " unknown TLE";
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }
        if (unk_ret != 0)
        {
            res.result = "UKE";
            res.message = "Task " + to_string(task_id) + " unknown RE/MLE";
            system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
            return res;
        }

        // 比对
        if (compare_outputs(work_dir + "\\std", work_dir + "\\out2"))
        {
            res.result = "AC";
            res.message = "Task " + to_string(task_id) + " Accepted";
        }
        else
        {
            res.result = "WA";
            res.message = "Task " + to_string(task_id) + " Wrong Answer";
        }
    }
    catch (const exception &e)
    {
        res.result = "UKE";
        res.message =
            string("Task ") + to_string(task_id) + " exception: " + e.what();
    }

    system(("rmdir /S /Q \"" + work_dir + "\" 2>nul").c_str());
    return res;
}

int main(int argc, char *argv[])
{
    if (argc != 5)
    {
        cerr << "Usage: " << argv[0]
             << " <make.cpp> <ans.cpp> <unknown.cpp> <k>" << endl;
        return 1;
    }

    string make_src = argv[1];
    string ans_src = argv[2];
    string unknown_src = argv[3];
    int k = atoi(argv[4]);

    if (k <= 0 || k >= 50)
    {
        cerr << "k must be 1-49" << endl;
        return 1;
    }

    replace(make_src.begin(), make_src.end(), '/', '\\');
    replace(ans_src.begin(), ans_src.end(), '/', '\\');
    replace(unknown_src.begin(), unknown_src.end(), '/', '\\');

    int threads = min(k, 4);
    ThreadPool pool(threads);

    vector<future<JudgeResult>> futures;
    for (int i = 0; i < k; ++i)
    {
        futures.push_back(
            pool.enqueue(judge_task, i, make_src, ans_src, unknown_src));
    }

    vector<JudgeResult> results;
    int ac = 0, wa = 0, uke = 0;

    for (auto &f : futures)
    {
        JudgeResult r = f.get();
        results.push_back(r);
        if (r.result == "AC")
            ac++;
        else if (r.result == "WA")
            wa++;
        else
            uke++;
    }

    cout << "{" << endl;
    cout << "  \"total\": " << k << "," << endl;
    cout << "  \"ac\": " << ac << "," << endl;
    cout << "  \"wa\": " << wa << "," << endl;
    cout << "  \"uke\": " << uke << "," << endl;
    cout << "  \"results\": [" << endl;

    for (size_t i = 0; i < results.size(); ++i)
    {
        cout << "    {\"id\": " << results[i].id << ", \"result\": \""
             << results[i].result << "\", \"message\": \""
             << json_escape(results[i].message) << "\"}";
        if (i + 1 < results.size())
            cout << ",";
        cout << endl;
    }

    cout << "  ]" << endl;
    cout << "}" << endl;

    return 0;
}