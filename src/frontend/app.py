from flask import Flask, render_template, request, jsonify
import subprocess
import os
import tempfile
import json
import shutil
import sys

app = Flask(__name__)
app.config['MAX_CONTENT_LENGTH'] = 16 * 1024 * 1024  # 16MB 文件大小限制

# 路径配置（相对于 app.py 的位置）
FRONTEND_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.dirname(FRONTEND_DIR)
BACKEND_DIR = os.path.join(SRC_DIR, 'backend')
PROJECT_ROOT = os.path.dirname(SRC_DIR)
INCLUDE_DIR = os.path.join(PROJECT_ROOT, 'include')

# C++ 可执行文件路径（Windows 下为 .exe）
JUDGE_EXE = os.path.join(BACKEND_DIR, 'judge_parallel.exe')

# 如果 Windows 下找不到 .exe，尝试无后缀版本（Linux/Mac 兼容）
if not os.path.exists(JUDGE_EXE):
    JUDGE_EXE = os.path.join(BACKEND_DIR, 'judge_parallel')

def check_backend():
    """检查后端可执行文件是否存在"""
    if not os.path.exists(JUDGE_EXE):
        return False, f"Backend not found: {JUDGE_EXE}"
    return True, "OK"

@app.route('/')
def index():
    """渲染主页"""
    backend_ok, msg = check_backend()
    if not backend_ok:
        return f"""
        <h1>Configuration Error</h1>
        <p>{msg}</p>
        <p>Please compile the C++ backend first:</p>
        <pre>cd src/backend
g++ -O2 -std=c++17 -I ../../include -pthread -o judge_parallel judge_parallel.cpp</pre>
        """, 500
    return render_template('index.html')

@app.route('/judge', methods=['POST'])
def judge():
    """处理评测请求"""
    # 检查后端
    backend_ok, msg = check_backend()
    if not backend_ok:
        return jsonify({'error': msg}), 500
    
    # 检查文件上传
    required_files = ['make', 'ans', 'unknown']
    for name in required_files:
        if name not in request.files:
            return jsonify({'error': f'Missing file: {name}.cpp'}), 400
    
    # 获取参数 k
    try:
        k = int(request.form.get('k', '1'))
        if k < 1 or k >= 50:
            return jsonify({'error': 'k must be between 1 and 49'}), 400
    except ValueError:
        return jsonify({'error': 'Invalid k value'}), 400
    
    # 创建临时目录（Windows 兼容）
    temp_dir = tempfile.mkdtemp(prefix='judge_')
    file_paths = {}
    
    try:
        # 保存上传的文件（保持 .cpp 后缀）
        for name in required_files:
            file = request.files[name]
            if file.filename == '':
                return jsonify({'error': f'Empty file: {name}'}), 400
            
            # 统一保存为 .cpp 文件
            save_path = os.path.join(temp_dir, f'{name}.cpp')
            file.save(save_path)
            file_paths[name] = save_path
        
        # 准备命令行参数（Windows 下需要转义路径）
        cmd = [
            JUDGE_EXE,
            file_paths['make'],
            file_paths['ans'],
            file_paths['unknown'],
            str(k)
        ]
        
        # 执行评测（带超时保护）
        # Windows 下 shell=False 避免命令行注入问题
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=300,  # 5分钟总超时
            shell=False,
            cwd=temp_dir  # 在临时目录下运行（避免路径问题）
        )
        
        # 解析 JSON 输出
        if result.returncode != 0:
            return jsonify({
                'error': 'Judge execution failed',
                'stderr': result.stderr,
                'stdout': result.stdout,
                'returncode': result.returncode
            }), 500
        
        try:
            output = json.loads(result.stdout)
            return jsonify(output)
        except json.JSONDecodeError as e:
            return jsonify({
                'error': 'Invalid JSON output from judge',
                'raw_output': result.stdout,
                'parse_error': str(e)
            }), 500
            
    except subprocess.TimeoutExpired:
        return jsonify({'error': 'Judge timeout (>5 minutes)'}), 504
    except Exception as e:
        return jsonify({'error': str(e)}), 500
    finally:
        # 清理临时目录（Windows 下使用 ignore_errors 避免占用问题）
        if os.path.exists(temp_dir):
            shutil.rmtree(temp_dir, ignore_errors=True)

@app.route('/health')
def health():
    """健康检查接口"""
    backend_ok, msg = check_backend()
    return jsonify({
        'status': 'ok' if backend_ok else 'error',
        'backend': 'found' if backend_ok else 'not found',
        'backend_path': JUDGE_EXE,
        'message': msg
    })

if __name__ == '__main__':
    # Windows 下建议使用 127.0.0.1 避免防火墙弹窗
    # 如需局域网访问，改为 '0.0.0.0'
    host = '127.0.0.1'
    port = 5000
    debug = True
    
    print(f"Starting server...")
    print(f"Frontend dir: {FRONTEND_DIR}")
    print(f"Backend dir: {BACKEND_DIR}")
    print(f"Backend exe: {JUDGE_EXE}")
    print(f"Access URL: http://{host}:{port}")
    
    app.run(debug=debug, host=host, port=port)